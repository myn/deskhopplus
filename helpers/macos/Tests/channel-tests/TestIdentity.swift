import DHCore
import DeskhopChannel

/*
 * A software P-256 identity, and the two derivations the *board* side of a
 * test needs. Both are calls straight into the shared core, so a test that
 * builds a board's reply builds it the way the firmware would.
 *
 * They live in the test target rather than in DeskhopChannel because the
 * helper has exactly one identity — the Secure Enclave's, which cannot hand
 * its private half to anything. A software identity shipped beside it would
 * be a private key the product can hold in memory, and the two `derive*`
 * methods it used to carry are the rule both ends must agree on, which now
 * lives once in `dh_auth`.
 */
final class TestIdentity: HelperIdentity {
    let publicKey: [UInt8]
    let keyId: [UInt8]
    private let privateKey: [UInt8]

    /// `nil` when the private key is not a valid scalar for the curve.
    init?(privateKey: [UInt8]) {
        guard privateKey.count == Int(DH_P256_PRIVATE_SIZE) else { return nil }
        var pub = [UInt8](repeating: 0, count: Int(DH_P256_PUBLIC_SIZE))
        guard privateKey.withUnsafeBufferPointer({ priv in
            pub.withUnsafeMutableBufferPointer { out in
                dh_p256_public_from_private(priv.baseAddress, out.baseAddress)
            }
        }) else { return nil }

        var kid = [UInt8](repeating: 0, count: Int(DH_KEY_ID_SIZE))
        pub.withUnsafeBufferPointer { pubPtr in
            kid.withUnsafeMutableBufferPointer { out in
                dh_p256_key_id(pubPtr.baseAddress, out.baseAddress)
            }
        }

        self.privateKey = privateKey
        self.publicKey = pub
        self.keyId = kid
    }

    func sharedSecret(with boardPublicKey: [UInt8]) -> [UInt8]? {
        guard boardPublicKey.count == Int(DH_P256_PUBLIC_SIZE) else { return nil }
        var ss = [UInt8](repeating: 0, count: Int(DH_P256_SHARED_SIZE))
        let ok = privateKey.withUnsafeBufferPointer { priv in
            boardPublicKey.withUnsafeBufferPointer { peer in
                ss.withUnsafeMutableBufferPointer { out in
                    dh_p256_ecdh(priv.baseAddress, peer.baseAddress, out.baseAddress)
                }
            }
        }
        return ok ? ss : nil
    }

    /// k_hello, as the board derives it to check an arriving hello.
    func helloKey(peer: [UInt8], helperNonce: [UInt8]) -> [UInt8]? {
        guard let ss = sharedSecret(with: peer), helperNonce.count == Int(DH_NONCE_SIZE) else {
            return nil
        }
        var key = [UInt8](repeating: 0, count: Int(DH_SESSION_KEY_SIZE))
        ss.withUnsafeBufferPointer { s in
            helperNonce.withUnsafeBufferPointer { n in
                key.withUnsafeMutableBufferPointer { out in
                    dh_auth_derive_hello_key(s.baseAddress, n.baseAddress, out.baseAddress)
                }
            }
        }
        return key
    }

    /// (k_h2b, k_b2h). Symmetric: the board runs ECDH against the *helper's*
    /// public key and reaches the same shared secret, so `peer` is whichever
    /// side this identity is not.
    func sessionKeys(peer: [UInt8], helperNonce: [UInt8],
                     boardNonce: [UInt8]) -> (kH2B: [UInt8], kB2H: [UInt8])? {
        guard let ss = sharedSecret(with: peer),
              helperNonce.count == Int(DH_NONCE_SIZE),
              boardNonce.count == Int(DH_NONCE_SIZE) else { return nil }
        var kH2B = [UInt8](repeating: 0, count: Int(DH_SESSION_KEY_SIZE))
        var kB2H = [UInt8](repeating: 0, count: Int(DH_SESSION_KEY_SIZE))
        ss.withUnsafeBufferPointer { s in
            helperNonce.withUnsafeBufferPointer { hn in
                boardNonce.withUnsafeBufferPointer { bn in
                    kH2B.withUnsafeMutableBufferPointer { h2b in
                        kB2H.withUnsafeMutableBufferPointer { b2h in
                            dh_auth_derive_session_keys(s.baseAddress, hn.baseAddress,
                                                        bn.baseAddress, h2b.baseAddress,
                                                        b2h.baseAddress)
                        }
                    }
                }
            }
        }
        return (kH2B, kB2H)
    }
}

/// An identity whose enclave will not answer — the shape of a Mac that has
/// locked the key, and of a pinned board key that is not a point on the curve.
/// Both reach the core as one ECDH returning false.
struct RefusingIdentity: HelperIdentity {
    let publicKey = [UInt8](repeating: 0x11, count: Int(DH_P256_PUBLIC_SIZE))
    let keyId = [UInt8](repeating: 0x22, count: Int(DH_KEY_ID_SIZE))
    func sharedSecret(with boardPublicKey: [UInt8]) -> [UInt8]? { nil }
}
