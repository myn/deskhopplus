import CryptoKit
import DHCore
import Foundation

/*
 * Where the helper keeps its identity and the board it is paired with.
 *
 * **Identity**: a Secure Enclave P-256 key, generated on first run and never
 * extractable. Same-user malware can *use* the key while it runs — keychain
 * ACLs that would prevent that bind to a stable code signature, and this
 * helper is unsigned and frequently rebuilt — but it cannot copy the key off
 * the machine, so the credential cannot outlive the infection and re-pairing
 * genuinely revokes. ADR-0008 records this trade.
 *
 * **Pinned board key**: the 64-byte P-256 public key the board sent in its
 * PAIR_GRANT. A board whose key changes is a board that was wiped past its
 * identity sector, re-flashed, or swapped.
 *
 * What is *not* here: any shared secret. The helper re-derives it inside the
 * Secure Enclave each session (~1 ms) and never stores it.
 *
 * On first run after an upgrade from v1, the old `secret` file is deleted.
 */

/// Secure Enclave identity implementing HelperIdentity.
public final class EnclaveIdentity: HelperIdentity {
    public let publicKey: [UInt8]
    public let keyId: [UInt8]
    private let enclaveKey: SecureEnclave.P256.KeyAgreement.PrivateKey

    public init(enclaveKey: SecureEnclave.P256.KeyAgreement.PrivateKey) {
        self.enclaveKey = enclaveKey
        let raw = enclaveKey.publicKey.rawRepresentation
        self.publicKey = [UInt8](raw)

        var kid = [UInt8](repeating: 0, count: Int(DH_KEY_ID_SIZE))
        publicKey.withUnsafeBufferPointer { pub in
            kid.withUnsafeMutableBufferPointer { out in
                dh_p256_key_id(pub.baseAddress, out.baseAddress)
            }
        }
        self.keyId = kid
    }

    /*
     * The one ECDH the enclave can do, which is the whole of what the shared
     * core asks a platform for. `nil` rather than a thrown error, because the
     * core's seam is a `bool`: a key the enclave will not agree against and a
     * board key that is not a point on the curve are the same unusable device,
     * and `dh_helper` reports both as one.
     */
    public func sharedSecret(with boardPublicKey: [UInt8]) -> [UInt8]? {
        guard let peerKey = try? P256.KeyAgreement.PublicKey(rawRepresentation: boardPublicKey),
              let ss = try? enclaveKey.sharedSecretFromKeyAgreement(with: peerKey) else {
            return nil
        }
        /*
         * The raw 32 bytes, not a CryptoKit-derived key: `SharedSecret` is
         * `ContiguousBytes` over the X coordinate of the agreed point, which
         * is exactly what `dh_p256_ecdh` writes on the board. HKDF is applied
         * afterwards, by the C core, so that both sides run the same
         * derivation rather than two that happen to agree.
         *
         * This equivalence is the one thing in the crypto path the host tests
         * cannot check — they run the C curve on both sides. It is confirmed
         * against hardware, not here.
         */
        return ss.withUnsafeBytes { ptr in [UInt8](ptr) }
    }
}

/// The store on disk: enclave key data and pinned board key.
public struct SecretStore {
    private let directory: URL
    private let enclaveKeyURL: URL
    private let boardKeyURL: URL
    private let legacySecretURL: URL

    public init(directory: URL? = nil) {
        let dir = directory ?? FileManager.default
            .urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("deskhopplus", isDirectory: true)
        self.directory = dir
        self.enclaveKeyURL = dir.appendingPathComponent("identity")
        self.boardKeyURL = dir.appendingPathComponent("board_key")
        self.legacySecretURL = dir.appendingPathComponent("secret")
    }

    /// Load or create the Secure Enclave identity, and delete the v1 secret
    /// file if it is still around.
    public func loadIdentity() throws -> EnclaveIdentity {
        deleteLegacySecret()

        /*
         * A stored blob that will not decode is regenerated, not raised. It is
         * sealed to one machine's enclave, so a home directory restored onto a
         * new Mac carries a file that cannot be opened here — and an error
         * thrown from a LaunchAgent's init is a crash launchd repeats for ever.
         * Regenerating costs a re-pair, which is one chord press, and the
         * board key is pinned separately so nothing else is lost.
         */
        if let data = try? Data(contentsOf: enclaveKeyURL),
           let key = try? SecureEnclave.P256.KeyAgreement.PrivateKey(dataRepresentation: data) {
            return EnclaveIdentity(enclaveKey: key)
        }

        let key = try SecureEnclave.P256.KeyAgreement.PrivateKey()
        try ensureDirectory()
        try key.dataRepresentation.write(to: enclaveKeyURL, options: [.atomic])
        try FileManager.default.setAttributes([.posixPermissions: 0o600],
                                              ofItemAtPath: enclaveKeyURL.path)
        return EnclaveIdentity(enclaveKey: key)
    }

    public func loadBoardKey() -> [UInt8]? {
        guard let data = try? Data(contentsOf: boardKeyURL),
              data.count == Int(DH_P256_PUBLIC_SIZE) else { return nil }
        return [UInt8](data)
    }

    @discardableResult
    public func saveBoardKey(_ key: [UInt8]) -> Bool {
        guard key.count == Int(DH_P256_PUBLIC_SIZE) else { return false }
        do {
            try ensureDirectory()
            try Data(key).write(to: boardKeyURL, options: [.atomic])
            try FileManager.default.setAttributes([.posixPermissions: 0o600],
                                                  ofItemAtPath: boardKeyURL.path)
            return true
        } catch {
            return false
        }
    }

    public func clearBoardKey() {
        try? FileManager.default.removeItem(at: boardKeyURL)
    }

    private func ensureDirectory() throws {
        try FileManager.default.createDirectory(
            at: directory, withIntermediateDirectories: true,
            attributes: [.posixPermissions: 0o700])
    }

    private func deleteLegacySecret() {
        try? FileManager.default.removeItem(at: legacySecretURL)
    }
}
