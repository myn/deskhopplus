#pragma once
/*
 * Where the helper keeps its identity and the board it is paired with (#84).
 *
 * ---------------------------------------------------------------------------
 * WHAT DPAPI HERE DOES AND DOES NOT PROTECT — stated plainly, because a user
 * who is misled about this makes worse decisions than one who is told nothing.
 *
 * **Does:** binds every stored blob to this Windows user account on this
 * machine. Copying the files to another machine, or to another account on this
 * one, yields bytes that will not unprotect. That is the property being
 * bought, and it is bought with no signature, no elevation and no prompt —
 * which is why it fits a portable unsigned exe where the macOS helper's
 * reasoning does not (Keychain access control keys off a code signature this
 * helper does not have; see SecretStore.swift).
 *
 * **Does not:** defend against a process running as the same user. Anything
 * with this account's token can call CryptUnprotectData on these files and
 * read the private key out. There is no Secure Enclave equivalent here — the
 * macOS helper's key is non-extractable and this one is not, and that
 * asymmetry is real rather than something the wording should smooth over.
 *
 * The mitigation that does apply is **rotation**: a fresh chord press
 * registers a new key with the board and the old one stops being accepted
 * (#46). Re-pairing genuinely revokes.
 *
 * **No optional entropy**, deliberately. A constant salt compiled into an exe
 * published from CI is readable by anyone who has the exe, so it would read as
 * protection while providing none — which is worse than having none. Entropy
 * is only worth passing when it is itself a secret, and there is nowhere here
 * to keep one that is not already this file's problem.
 *
 * CRYPTPROTECT_UI_FORBIDDEN throughout: this runs in the notification area
 * with no window worth blocking, and a modal credential prompt from a
 * background helper is a prompt the user cannot attribute to anything.
 * ---------------------------------------------------------------------------
 *
 * What is *not* stored: any shared secret. The helper re-derives it by ECDH
 * each session and never keeps it.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace deskhop {

/* Unpredictable bytes for the core: nonces and correlation values. Fails hard
   rather than returning something predictable — a nonce nobody chose would
   quietly weaken every session keyed on it. */
bool fill_random(uint8_t *out, size_t len);

class SecretStore {
  public:
    /* Defaults to %LOCALAPPDATA%\deskhopplus. */
    explicit SecretStore(std::wstring directory = {});

    struct Identity {
        std::vector<uint8_t> private_key; /* 32 bytes, this helper's own */
        std::vector<uint8_t> public_key;  /* 64 raw bytes, X || Y, big-endian */
        std::vector<uint8_t> key_id;      /* SHA-256(public)[0..8] */
    };

    /*
     * Load the stored identity, or make one on first run.
     *
     * A blob that will not unprotect is regenerated rather than raised: it is
     * bound to one account on one machine, so a profile restored elsewhere
     * carries a file that cannot be opened here. Regenerating costs one chord
     * press, and the pinned board key is stored separately so nothing else is
     * lost.
     *
     * False only when a key cannot be created at all, which is the RNG
     * failing — there is nothing to retry into.
     */
    bool load_identity(Identity &out);

    /* The 64-byte P-256 public key the board sent in its PAIR_GRANT. Empty
       when this helper has never paired. */
    std::vector<uint8_t> load_board_key();
    bool save_board_key(const std::vector<uint8_t> &key);
    void clear_board_key();

    const std::wstring &directory() const { return directory_; }

  private:
    bool write_protected(const std::wstring &path, const std::vector<uint8_t> &plain);
    std::vector<uint8_t> read_protected(const std::wstring &path);

    std::wstring directory_;
    std::wstring identity_path_;
    std::wstring board_key_path_;
};

} // namespace deskhop
