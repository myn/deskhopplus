#include "secret_store.h"

#include <windows.h>

#include <bcrypt.h>
#include <dpapi.h>
#include <shlobj.h>

#include <cstring>

#include "dh_p256.h"

namespace deskhop {

namespace {

/* Zero a buffer that held key material before it goes back to the allocator.
   SecureZeroMemory rather than memset: the compiler is entitled to delete a
   memset whose result is never read, and here that is the whole point. */
void wipe(std::vector<uint8_t> &bytes) {
    if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size());
}

std::wstring local_app_data() {
    PWSTR folder = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &folder) != S_OK) return {};
    std::wstring path(folder);
    CoTaskMemFree(folder);
    return path;
}

std::vector<uint8_t> read_file(const std::wstring &path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};

    LARGE_INTEGER size{};
    std::vector<uint8_t> bytes;
    if (GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < 64 * 1024) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) ||
            read != bytes.size())
            bytes.clear();
    }
    CloseHandle(file);
    return bytes;
}

bool write_file(const std::wstring &path, const std::vector<uint8_t> &bytes) {
    /* Written to a neighbour and moved into place: a half-written identity
       would be indistinguishable from a corrupt one, and the recovery for a
       corrupt one is regenerating the key — which costs a re-pair. */
    const std::wstring temporary = path + L".new";
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                              nullptr) &&
                    written == bytes.size();
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temporary.c_str());
        return false;
    }
    return MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
}

} // namespace

bool fill_random(uint8_t *out, size_t len) {
    /* The system preferred RNG, so there is no algorithm handle to open and
       nothing to install. bcrypt.dll is inbox, which is the whole of what
       ADR-0006 asks of a dependency. */
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(len),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

SecretStore::SecretStore(std::wstring directory) {
    directory_ = directory.empty() ? local_app_data() + L"\\deskhopplus" : std::move(directory);
    identity_path_ = directory_ + L"\\identity";
    board_key_path_ = directory_ + L"\\board_key";
}

bool SecretStore::write_protected(const std::wstring &path, const std::vector<uint8_t> &plain) {
    DATA_BLOB in{static_cast<DWORD>(plain.size()), const_cast<BYTE *>(plain.data())};
    DATA_BLOB out{};
    /* No optional entropy, and no UI. Both are decisions, not omissions — see
       the header. */
    if (!CryptProtectData(&in, L"deskhopplus helper", nullptr, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;

    std::vector<uint8_t> blob(out.pbData, out.pbData + out.cbData);
    LocalFree(out.pbData);

    SHCreateDirectoryExW(nullptr, directory_.c_str(), nullptr);
    return write_file(path, blob);
}

std::vector<uint8_t> SecretStore::read_protected(const std::wstring &path) {
    std::vector<uint8_t> blob = read_file(path);
    if (blob.empty()) return {};

    DATA_BLOB in{static_cast<DWORD>(blob.size()), blob.data()};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                            &out))
        return {};

    std::vector<uint8_t> plain(out.pbData, out.pbData + out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return plain;
}

bool SecretStore::load_identity(Identity &out) {
    std::vector<uint8_t> stored = read_protected(identity_path_);

    if (stored.size() == DH_P256_PRIVATE_SIZE) {
        out.private_key = stored;
    } else {
        /*
         * First run, or a blob this account cannot unprotect. Either way there
         * is a key to make: 32 random bytes, redrawn on the roughly one draw
         * in 2^32 that is not a scalar the curve accepts.
         */
        wipe(stored);
        std::vector<uint8_t> candidate(DH_P256_PRIVATE_SIZE);
        std::vector<uint8_t> public_key(DH_P256_PUBLIC_SIZE);
        bool made = false;
        for (int attempt = 0; attempt < 8 && !made; ++attempt) {
            if (!fill_random(candidate.data(), candidate.size())) break;
            made = dh_p256_public_from_private(candidate.data(), public_key.data());
        }
        if (!made) {
            wipe(candidate);
            return false;
        }
        out.private_key = candidate;
        /* A key that cannot be stored still works for this run. The helper
           re-pairs on the next start instead of refusing to run now, and the
           caller says so in the log. */
        write_protected(identity_path_, out.private_key);
    }

    out.public_key.assign(DH_P256_PUBLIC_SIZE, 0);
    if (!dh_p256_public_from_private(out.private_key.data(), out.public_key.data())) {
        wipe(out.private_key);
        return false;
    }
    out.key_id.assign(DH_KEY_ID_SIZE, 0);
    dh_p256_key_id(out.public_key.data(), out.key_id.data());
    return true;
}

std::vector<uint8_t> SecretStore::load_board_key() {
    std::vector<uint8_t> key = read_protected(board_key_path_);
    if (key.size() != DH_P256_PUBLIC_SIZE) return {};
    return key;
}

bool SecretStore::save_board_key(const std::vector<uint8_t> &key) {
    if (key.size() != DH_P256_PUBLIC_SIZE) return false;
    /* Protected like the private key even though a public key is not a secret.
       What DPAPI buys here is that a blob planted by another *account* will
       not unprotect, so the pin cannot be quietly repointed at another board
       by anyone who could not already run as this user. */
    return write_protected(board_key_path_, key);
}

void SecretStore::clear_board_key() { DeleteFileW(board_key_path_.c_str()); }

} // namespace deskhop
