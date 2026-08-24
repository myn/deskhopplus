#include "seal_aead.h"

#include <windows.h>

#include <bcrypt.h>

#include <cstring>

namespace deskhop {

namespace {

#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

/*
 * One AES-GCM provider for the process. Opening one per frame would be a
 * registry lookup per clipboard chunk; a provider handle is documented as safe
 * to share, and the per-call key handle below is what actually holds material.
 */
struct Provider {
    BCRYPT_ALG_HANDLE handle = nullptr;

    Provider() {
        if (BCryptOpenAlgorithmProvider(&handle, BCRYPT_AES_ALGORITHM, nullptr, 0) !=
            STATUS_SUCCESS) {
            handle = nullptr;
            return;
        }
        if (BCryptSetProperty(handle, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(
                                  BCRYPT_CHAIN_MODE_GCM)),
                              sizeof(BCRYPT_CHAIN_MODE_GCM), 0) != STATUS_SUCCESS) {
            BCryptCloseAlgorithmProvider(handle, 0);
            handle = nullptr;
        }
    }

    ~Provider() {
        if (handle) BCryptCloseAlgorithmProvider(handle, 0);
    }
};

BCRYPT_ALG_HANDLE provider() {
    /* Function-local static: initialised once, and thread-safely, on the first
       call rather than at some unspecified point during startup. */
    static Provider shared;
    return shared.handle;
}

/* A key handle for one call. GCM's rule is that a nonce is used once per key,
   and this code has no reason to keep either alive past the frame. */
struct Key {
    BCRYPT_KEY_HANDLE handle = nullptr;

    explicit Key(const uint8_t *bytes) {
        BCRYPT_ALG_HANDLE alg = provider();
        if (!alg) return;
        if (BCryptGenerateSymmetricKey(alg, &handle, nullptr, 0,
                                       reinterpret_cast<PUCHAR>(const_cast<uint8_t *>(bytes)),
                                       DH_SEAL_KEY_SIZE, 0) != STATUS_SUCCESS)
            handle = nullptr;
    }

    ~Key() {
        if (handle) BCryptDestroyKey(handle);
    }
};

/* The authenticated-mode block both directions fill in the same way. */
BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO auth_info(const uint8_t *nonce, const uint8_t *aad,
                                                size_t aad_len, uint8_t *tag) {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<uint8_t *>(nonce));
    info.cbNonce = DH_SEAL_NONCE_SIZE;
    info.pbAuthData = aad_len ? reinterpret_cast<PUCHAR>(const_cast<uint8_t *>(aad)) : nullptr;
    info.cbAuthData = static_cast<ULONG>(aad_len);
    info.pbTag = tag;
    info.cbTag = DH_SEAL_TAG_SIZE;
    return info;
}

bool seal(void *, const uint8_t key[DH_SEAL_KEY_SIZE], const uint8_t nonce[DH_SEAL_NONCE_SIZE],
          const uint8_t *aad, size_t aad_len, const uint8_t *plain, size_t plain_len,
          uint8_t *cipher_out, uint8_t tag_out[DH_SEAL_TAG_SIZE]) {
    Key k(key);
    if (!k.handle) return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info = auth_info(nonce, aad, aad_len, tag_out);
    ULONG written = 0;
    /* pbInput may equal pbOutput: CNG supports encrypting in place, which is
       what keeps a clipboard chunk from being copied again on its way out. */
    return BCryptEncrypt(k.handle, reinterpret_cast<PUCHAR>(const_cast<uint8_t *>(plain)),
                         static_cast<ULONG>(plain_len), &info, nullptr, 0, cipher_out,
                         static_cast<ULONG>(plain_len), &written, 0) == STATUS_SUCCESS &&
           written == static_cast<ULONG>(plain_len);
}

bool open(void *, const uint8_t key[DH_SEAL_KEY_SIZE], const uint8_t nonce[DH_SEAL_NONCE_SIZE],
          const uint8_t *aad, size_t aad_len, const uint8_t *cipher, size_t cipher_len,
          const uint8_t tag[DH_SEAL_TAG_SIZE], uint8_t *plain_out) {
    Key k(key);
    if (!k.handle) return false;

    uint8_t tag_copy[DH_SEAL_TAG_SIZE];
    memcpy(tag_copy, tag, sizeof tag_copy);

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info = auth_info(nonce, aad, aad_len, tag_copy);
    ULONG written = 0;
    const NTSTATUS status =
        BCryptDecrypt(k.handle, reinterpret_cast<PUCHAR>(const_cast<uint8_t *>(cipher)),
                      static_cast<ULONG>(cipher_len), &info, nullptr, 0, plain_out,
                      static_cast<ULONG>(cipher_len), &written, 0);
    if (status != STATUS_SUCCESS || written != static_cast<ULONG>(cipher_len)) {
        /* A frame that did not authenticate must leave nothing behind: CNG
           documents the output as undefined on failure, not as untouched. */
        if (cipher_len) SecureZeroMemory(plain_out, cipher_len);
        return false;
    }
    return true;
}

const dh_seal_aead aead = {nullptr, seal, open};

} /* namespace */

const dh_seal_aead *seal_aead() { return provider() ? &aead : nullptr; }

} /* namespace deskhop */
