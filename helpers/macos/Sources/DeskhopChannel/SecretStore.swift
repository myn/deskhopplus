import DHCore
import Foundation

/*
 * Where the helper keeps the secret the device gave it (#46). It is the only
 * local state the helper has — everything else lives on the device.
 *
 * A file, not the Keychain, and the reasoning is worth stating because the
 * Keychain is the obvious answer:
 *
 * Keychain items are access-controlled by the *code signature* of the process
 * reading them. This helper is unsigned and unbundled — that is the state in
 * which every measurement behind ADR-0001 and #41 was taken — so its identity
 * changes with every build, and each new identity is met with an "allow
 * access" prompt. The transport was chosen, and its evidence gathered,
 * precisely to avoid permission prompts; buying one back to store the
 * credential that makes the channel usable would be self-defeating.
 *
 * What this costs, stated rather than hidden: any process running as the same
 * user can read the file. On macOS there is no strong same-user isolation
 * without a signed, bundled application, so the alternatives are a prompt or
 * this. When packaging arrives (out of scope for the channel work, per #42)
 * a signed bundle makes the Keychain viable and this should move there.
 *
 * The mitigations that do apply are applied: 0700 on the directory, 0600 on
 * the file, and rotation (#46) means a leaked secret stops working the moment
 * the user presses the chord again.
 */
public struct SecretStore {
    public static let length = Int(DH_PAIR_SECRET_LEN)

    private let url: URL

    public init(url: URL? = nil) {
        self.url = url ?? FileManager.default
            .urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("deskhopplus", isDirectory: true)
            .appendingPathComponent("secret")
    }

    /// The stored secret, or nil when there is none — a fresh install, or a
    /// device whose configuration was wiped. Either way the remedy is one
    /// chord press, and the helper says so.
    public func load() -> [UInt8]? {
        guard let data = try? Data(contentsOf: url), data.count == Self.length else {
            return nil
        }
        return [UInt8](data)
    }

    /// Store a freshly granted secret, replacing whatever was there. Rotation
    /// means the old one is already dead on the device.
    @discardableResult
    public func save(_ secret: [UInt8]) -> Bool {
        guard secret.count == Self.length else { return false }

        let directory = url.deletingLastPathComponent()
        do {
            try FileManager.default.createDirectory(
                at: directory, withIntermediateDirectories: true,
                attributes: [.posixPermissions: 0o700])
            try Data(secret).write(to: url, options: [.atomic, .completeFileProtection])
            try FileManager.default.setAttributes([.posixPermissions: 0o600],
                                                  ofItemAtPath: url.path)
            return true
        } catch {
            return false
        }
    }

    public func clear() {
        try? FileManager.default.removeItem(at: url)
    }
}
