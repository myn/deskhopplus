#!/usr/bin/env python3
"""Regenerate test-vectors/frames.txt from docs/protocol.md's v2 layouts.

    python3 tools/gen-frame-vectors.py > test-vectors/frames.txt

Why this exists, and why it is Python with no imports outside the standard
library: the vector file is the cross-implementation gate, and v2 puts a real
HMAC-SHA256 tag on almost every frame and a real AES-256-GCM seal inside the
two clipboard messages that carry user bytes. A vector with an invented tag
would be worse than no vector at all — the first implementation to disagree
with it would "fix" the file rather than the code, which is the failure shape
#108 found three times in one probe.

So the primitives are reimplemented here, deliberately, rather than bound to
src/core. A generator that shares its encoder with the thing it gates cannot
catch the encoder being wrong. `python3 tools/gen-frame-vectors.py --self-test`
runs the known-answer checks that keep this copy honest.

All key material below is fixed, published, and obviously fake. It is test
material and nothing else: no board and no helper ever holds these bytes.
"""

import hashlib
import hmac
import struct
import sys
import zlib

# --------------------------------------------------------------------------
# P-256, enough of it for one ECDH.
# --------------------------------------------------------------------------

P = 2**256 - 2**224 + 2**192 + 2**96 - 1
A = P - 3
B = 0x5AC635D8AA3A93E7B3EBBD55769886BC651D06B0CC53B0F63BCE3C3E27D2604B
GX = 0x6B17D1F2E12C4247F8BCE6E563A440F277037D812DEB33A0F4A13945D898C296
GY = 0x4FE342E2FE1A7F9B8EE7EB4A7C0F9E162BCE33576B315ECECBB6406837BF51F5
N = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551

# Points are (x, y) or None for the point at infinity.


def point_add(p1, p2):
    if p1 is None:
        return p2
    if p2 is None:
        return p1
    x1, y1 = p1
    x2, y2 = p2
    if x1 == x2 and (y1 + y2) % P == 0:
        return None
    if p1 == p2:
        lam = (3 * x1 * x1 + A) * pow(2 * y1, P - 2, P) % P
    else:
        lam = (y2 - y1) * pow(x2 - x1, P - 2, P) % P
    x3 = (lam * lam - x1 - x2) % P
    return (x3, (lam * (x1 - x3) - y1) % P)


def scalar_mult(k, point):
    result = None
    addend = point
    while k:
        if k & 1:
            result = point_add(result, addend)
        addend = point_add(addend, addend)
        k >>= 1
    return result


def on_curve(point):
    x, y = point
    return (y * y - x * x * x - A * x - B) % P == 0


def public_key(private: bytes) -> bytes:
    """The uncompressed point minus its 0x04 prefix: X || Y, 64 bytes.

    This is micro-ecc's native form and CryptoKit's `rawRepresentation`, so no
    end has to strip or add a prefix.
    """
    x, y = scalar_mult(int.from_bytes(private, "big"), (GX, GY))
    return x.to_bytes(32, "big") + y.to_bytes(32, "big")


def ecdh(private: bytes, peer_public: bytes) -> bytes:
    """The shared secret: the X coordinate of d·Q, 32 bytes."""
    peer = (int.from_bytes(peer_public[:32], "big"), int.from_bytes(peer_public[32:], "big"))
    if not on_curve(peer):
        raise ValueError("peer public key is not on P-256")
    x, _ = scalar_mult(int.from_bytes(private, "big"), peer)
    return x.to_bytes(32, "big")


# --------------------------------------------------------------------------
# AES-256, and GCM over it.
# --------------------------------------------------------------------------

def _rotl8(x, s):
    return ((x << s) | (x >> (8 - s))) & 0xFF


def _build_sbox():
    sbox = bytearray(256)
    sbox[0] = 0x63
    p = q = 1
    while True:
        p = (p ^ (p << 1) ^ (0x1B if p & 0x80 else 0)) & 0xFF
        q ^= q << 1
        q ^= q << 2
        q ^= q << 4
        q &= 0xFF
        if q & 0x80:
            q ^= 0x09
        sbox[p] = q ^ _rotl8(q, 1) ^ _rotl8(q, 2) ^ _rotl8(q, 3) ^ _rotl8(q, 4) ^ 0x63
        if p == 1:
            return sbox


SBOX = _build_sbox()


def xtime(b):
    return ((b << 1) ^ 0x1B) & 0xFF if b & 0x80 else b << 1


def aes256_expand(key: bytes):
    words = [list(key[i : i + 4]) for i in range(0, 32, 4)]
    rcon = 1
    for i in range(8, 60):
        t = list(words[i - 1])
        if i % 8 == 0:
            t = t[1:] + t[:1]
            t = [SBOX[b] for b in t]
            t[0] ^= rcon
            rcon = xtime(rcon)
        elif i % 8 == 4:
            t = [SBOX[b] for b in t]
        words.append([words[i - 8][j] ^ t[j] for j in range(4)])
    return [bytes(b for w in words[r * 4 : r * 4 + 4] for b in w) for r in range(15)]


def aes256_encrypt_block(round_keys, block: bytes) -> bytes:
    s = bytearray(x ^ y for x, y in zip(block, round_keys[0]))
    for rnd in range(1, 15):
        s = bytearray(SBOX[b] for b in s)
        # ShiftRows: the state is column-major, so row r of column c is byte 4c+r
        # and row r rotates left by r columns.
        s = bytearray(s[4 * ((i // 4 + i % 4) % 4) + i % 4] for i in range(16))
        if rnd != 14:
            t = bytearray(16)
            for c in range(4):
                col = s[c * 4 : c * 4 + 4]
                for r in range(4):
                    t[c * 4 + r] = (
                        xtime(col[r])
                        ^ (xtime(col[(r + 1) % 4]) ^ col[(r + 1) % 4])
                        ^ col[(r + 2) % 4]
                        ^ col[(r + 3) % 4]
                    )
            s = t
        s = bytearray(x ^ y for x, y in zip(s, round_keys[rnd]))
    return bytes(s)


def _gf_mult(x: int, y: int) -> int:
    r = 0xE1 << 120
    z = 0
    v = y
    for i in range(128):
        if (x >> (127 - i)) & 1:
            z ^= v
        v = (v >> 1) ^ r if v & 1 else v >> 1
    return z


def _ghash(h: int, data: bytes) -> int:
    y = 0
    for i in range(0, len(data), 16):
        block = data[i : i + 16].ljust(16, b"\x00")
        y = _gf_mult(y ^ int.from_bytes(block, "big"), h)
    return y


def aes256_gcm_seal(key: bytes, nonce: bytes, plaintext: bytes, aad: bytes):
    """Returns (ciphertext, 16-byte tag). Nonce is 96 bits, as everywhere here."""
    assert len(nonce) == 12, "this protocol only ever uses a 96-bit GCM nonce"
    rk = aes256_expand(key)
    h = int.from_bytes(aes256_encrypt_block(rk, b"\x00" * 16), "big")
    j0 = nonce + b"\x00\x00\x00\x01"

    ciphertext = bytearray()
    counter = int.from_bytes(j0, "big")
    for i in range(0, len(plaintext), 16):
        counter = (counter & ~0xFFFFFFFF) | ((counter + 1) & 0xFFFFFFFF)
        stream = aes256_encrypt_block(rk, counter.to_bytes(16, "big"))
        ciphertext += bytes(x ^ y for x, y in zip(plaintext[i : i + 16], stream))

    padded = aad + b"\x00" * (-len(aad) % 16) + bytes(ciphertext) + b"\x00" * (-len(ciphertext) % 16)
    s = _ghash(h, padded + struct.pack(">QQ", len(aad) * 8, len(ciphertext) * 8))
    tag = bytes(
        x ^ y for x, y in zip(s.to_bytes(16, "big"), aes256_encrypt_block(rk, j0))
    )
    return bytes(ciphertext), tag


# --------------------------------------------------------------------------
# HKDF-SHA256 and the frame tag.
# --------------------------------------------------------------------------


def hkdf(ikm: bytes, salt: bytes, info: bytes, length: int = 32) -> bytes:
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    out, block, counter = b"", b"", 1
    while len(out) < length:
        block = hmac.new(prk, block + info + bytes([counter]), hashlib.sha256).digest()
        out += block
        counter += 1
    return out[:length]


TAG_LEN = 16
AUTH_PREFIX_LEN = 8 + TAG_LEN


def frame(msg_type: int, body: bytes, key: bytes = None, counter: int = 0) -> bytes:
    """One frame, with the authentication prefix when the type carries one.

    Types 0x08-0x0F are the pairing and refusal messages, which exist before
    or instead of a session key and therefore carry no prefix.
    """
    if key is None:
        assert 0x08 <= msg_type <= 0x0F, "only 0x08-0x0F may go untagged"
        payload = body
    else:
        header = struct.pack("<BBH", msg_type, 0, AUTH_PREFIX_LEN + len(body))
        covered = header + struct.pack("<Q", counter) + body
        tag = hmac.new(key, covered, hashlib.sha256).digest()[:TAG_LEN]
        payload = struct.pack("<Q", counter) + tag + body
    return struct.pack("<BBH", msg_type, 0, len(payload)) + payload


# --------------------------------------------------------------------------
# Test key material. Fixed, published, and obviously fake.
# --------------------------------------------------------------------------

HELPER_PRIV = bytes(range(1, 33))
BOARD_PRIV = bytes(range(33, 65))
SEAL_OFFER_PRIV = bytes(range(65, 97))
SEAL_ACCEPT_PRIV = bytes(range(97, 129))

HELPER_PUB = public_key(HELPER_PRIV)
BOARD_PUB = public_key(BOARD_PRIV)
SEAL_OFFER_PUB = public_key(SEAL_OFFER_PRIV)
SEAL_ACCEPT_PUB = public_key(SEAL_ACCEPT_PRIV)

HELPER_NONCE = bytes(range(0x00, 0x10))
BOARD_NONCE = bytes(range(0x10, 0x20))
SEAL_OFFER_NONCE = bytes(range(0x20, 0x30))
SEAL_ACCEPT_NONCE = bytes(range(0x30, 0x40))

CORRELATION = 0xDEADBEEFCAFEF00D
SEAL_ID = 0x11223344

SHARED = ecdh(HELPER_PRIV, BOARD_PUB)
K_HELLO = hkdf(SHARED, HELPER_NONCE, b"deskhopplus/2 hello")
K_H2B = hkdf(SHARED, HELPER_NONCE + BOARD_NONCE, b"deskhopplus/2 h2b")
K_B2H = hkdf(SHARED, HELPER_NONCE + BOARD_NONCE, b"deskhopplus/2 b2h")

K_SEAL = hkdf(
    ecdh(SEAL_OFFER_PRIV, SEAL_ACCEPT_PUB),
    SEAL_OFFER_NONCE + SEAL_ACCEPT_NONCE,
    b"deskhopplus/2 seal",
)

HELPER_KEY_ID = hashlib.sha256(HELPER_PUB).digest()[:8]


def seal(counter: int, plaintext: bytes, aad: bytes) -> bytes:
    ciphertext, tag = aes256_gcm_seal(
        K_SEAL, struct.pack("<Q", counter) + b"\x00" * 4, plaintext, aad
    )
    return ciphertext + tag


# --------------------------------------------------------------------------
# The vectors.
# --------------------------------------------------------------------------


def build():
    v = []

    # --- session, helper to device -----------------------------------------
    hello_body = (
        struct.pack("<HBBBH", 2, 1, 0, 1, 1024)  # version, mac, release, 1 channel, 1024
        + struct.pack("<Q", CORRELATION)
        + HELPER_KEY_ID
        + HELPER_NONCE
    )
    v.append(("hello_mac", frame(0x01, hello_body, K_HELLO, 0)))

    ack_body = (
        struct.pack("<Q", CORRELATION)
        + struct.pack("<HBBH", 2, 0, 1, 1024)  # version, release, 1 channel, 1024
        + BOARD_NONCE
    )
    v.append(("hello_ack_ok", frame(0x02, ack_body, K_B2H, 0)))

    v.append(("heartbeat", frame(0x05, b"", K_H2B, 0)))
    v.append(("device_heartbeat", frame(0x06, b"", K_B2H, 1)))

    for name, reason, counter in [
        ("session_end_liveness", 1, 2),
        ("session_end_protocol_error", 2, 3),
        ("session_end_unpaired", 3, 4),
    ]:
        v.append((name, frame(0x07, bytes([reason]), K_B2H, counter)))

    # 4 refused frames in a 10 s window: a rate, not an event.
    v.append(("listener_alert", frame(0x03, struct.pack("<II", 10000, 4), K_B2H, 5)))

    # --- pairing and refusal, the untagged band ----------------------------
    v.append(("pair_request", frame(0x08, struct.pack("<Q", CORRELATION) + HELPER_PUB)))
    v.append(("pair_grant", frame(0x09, struct.pack("<Q", CORRELATION) + BOARD_PUB)))
    v.append(("pair_refused_no_window", frame(0x0A, struct.pack("<QB", CORRELATION, 0))))
    v.append(("pair_refused_registered", frame(0x0A, struct.pack("<QB", CORRELATION, 1))))
    v.append(("hello_refused_version", frame(0x0B, struct.pack("<QHB", CORRELATION, 2, 2))))
    v.append(("hello_refused_unpaired", frame(0x0B, struct.pack("<QHB", CORRELATION, 2, 3))))

    # --- placement ----------------------------------------------------------
    v.append(("place_chain2_mid", frame(0x20, struct.pack("<BBH", 2, 1, 0x8000), K_B2H, 6)))
    v.append(("pos_query", frame(0x21, b"", K_B2H, 7)))
    v.append(("pos_response_chain1", frame(0x22, struct.pack("<BHH", 1, 0x4000, 0xC000), K_H2B, 1)))

    # --- the seal exchange --------------------------------------------------
    v.append((
        "seal_offer",
        frame(0x37, struct.pack("<I", SEAL_ID) + SEAL_OFFER_NONCE + SEAL_OFFER_PUB, K_H2B, 2),
    ))
    v.append((
        "seal_accept",
        frame(0x38, struct.pack("<I", SEAL_ID) + SEAL_ACCEPT_NONCE + SEAL_ACCEPT_PUB, K_H2B, 3),
    ))
    v.append(("seal_stale", frame(0x39, struct.pack("<I", SEAL_ID), K_H2B, 4)))

    # --- clipboard bulk -----------------------------------------------------
    # CLIP_OFFER: id 1, two bytes of text, no metadata.
    offer_clear = struct.pack("<IIQ", 1, SEAL_ID, 0)
    offer_plain = struct.pack("<BQH", 0, 2, 0)  # kind utf8-text, total 2, meta_len 0
    v.append(("clip_offer_text2", frame(0x30, offer_clear + seal(0, offer_plain, offer_clear),
                                        K_H2B, 5)))

    v.append(("clip_request_1", frame(0x31, struct.pack("<I", 1), K_H2B, 6)))

    # CLIP_CHUNK: id 2, seq 0, the two bytes "hi" and their CRC32.
    data = b"hi"
    chunk_clear = struct.pack("<IIIQ", 2, 0, SEAL_ID, 1)
    chunk_plain = struct.pack("<I", zlib.crc32(data)) + data
    v.append(("clip_chunk_hi", frame(0x32, chunk_clear + seal(1, chunk_plain, chunk_clear),
                                     K_H2B, 7)))

    v.append(("clip_done_2", frame(0x33, struct.pack("<I", 2), K_H2B, 8)))
    v.append(("clip_cancel_2", frame(0x34, struct.pack("<I", 2), K_H2B, 9)))
    v.append(("clip_retransmit_seq5", frame(0x35, struct.pack("<II", 2, 5), K_H2B, 10)))
    v.append(("clip_credit_8", frame(0x36, struct.pack("<IH", 2, 8), K_H2B, 11)))

    return v


HEADER = """\
# deskhopplus golden protocol frames — the cross-implementation compatibility gate.
# Format: <name> | <hex bytes, spaces ignored>
# The shared C core (src/core/) and every binding of it must decode each frame and
# re-encode it to the identical bytes. Any protocol change updates docs/protocol.md,
# this file, and the core in the same change.
#
# GENERATED. Do not hand-edit: run `python3 tools/gen-frame-vectors.py > test-vectors/frames.txt`.
# The tags and seals below are real, computed from the fixed test key material published
# in that script. A hand-written tag would make this file agree with nothing.
#
# Protocol v2 (ADR-0008, #109). Every frame outside types 0x08-0x0F carries the
# 24-byte authentication prefix — an 8-byte counter and a 16-byte HMAC-SHA256 tag —
# between the 4-byte header and the body. mkroamer's five byte-identical vectors did
# not survive that: the names are kept, the bytes are not, because the frames now
# carry a tag mkroamer's protocol had no field for.
#
# Directions and counters, so a verifier knows which key to try:
#   hello_mac                  k_hello, counter 0
#   helper to device (k_h2b)   heartbeat 0, pos_response 1, seal_offer 2, seal_accept 3,
#                              seal_stale 4, and the clipboard messages 5 through 11
#   device to helper (k_b2h)   hello_ack 0, device_heartbeat 1, session_end 2/3/4,
#                              listener_alert 5, place 6, pos_query 7
#   pair_* and hello_refused_* carry no prefix and no counter at all
#
# The bulk vectors are all in the helper-to-device direction. Which way a relayed
# message travels is not a property of its bytes — the tag is rewritten at each hop —
# so one direction is enough to gate the layout.
"""


def emit():
    lines = [HEADER]
    for name, raw in build():
        lines.append(f"{name:<28} | " + " ".join(f"{b:02x}" for b in raw))
    return "\n".join(lines) + "\n"


# --------------------------------------------------------------------------
# Known-answer checks. A generator that cannot fail is not a check.
# --------------------------------------------------------------------------


def self_test():
    ok = True

    def check(cond, what):
        nonlocal ok
        if not cond:
            ok = False
            print(f"FAIL {what}", file=sys.stderr)

    # AES-256 against the published all-zero block, and GCM against NIST's
    # test cases 13 and 14.
    rk = aes256_expand(b"\x00" * 32)
    check(
        aes256_encrypt_block(rk, b"\x00" * 16).hex() == "dc95c078a2408989ad48a21492842087",
        "AES-256 zero block",
    )
    _, tag13 = aes256_gcm_seal(b"\x00" * 32, b"\x00" * 12, b"", b"")
    check(tag13.hex() == "530f8afbc74536b9a963b4f1c4cb738b", "GCM test case 13")
    ct14, tag14 = aes256_gcm_seal(b"\x00" * 32, b"\x00" * 12, b"\x00" * 16, b"")
    check(ct14.hex() == "cea7403d4d606b6e074ec5d3baf39d18", "GCM test case 14 ciphertext")
    check(tag14.hex() == "d0d1c8a799996bf0265b98b5d48ab919", "GCM test case 14 tag")

    # Cases 15 and 16 are the ones that matter for this generator: several
    # blocks, a partial trailing block, and additional data. Every vector this
    # script emits is one block long, so without these the multi-block counter
    # and the GHASH padding would be untested — a check that cannot fail.
    key = bytes.fromhex("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f9467308308")
    iv = bytes.fromhex("cafebabefacedbaddecaf888")
    p15 = bytes.fromhex(
        "d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c303d8a318a72"
        "1c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de657ba637b391aafd255"
    )
    ct15, tag15 = aes256_gcm_seal(key, iv, p15, b"")
    check(
        ct15.hex() == "522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2bd2555d1aa"
        "8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7a0abcc9f662898015ad",
        "GCM test case 15 ciphertext",
    )
    check(tag15.hex() == "b094dac5d93471bdec1a502270e3cc6c", "GCM test case 15 tag")

    aad16 = bytes.fromhex("feedfacedeadbeeffeedfacedeadbeefabaddad2")
    ct16, tag16 = aes256_gcm_seal(key, iv, p15[:60], aad16)
    check(ct16 == ct15[:60], "GCM test case 16 ciphertext")
    check(tag16.hex() == "76fc6ece0f4e1768cddf8853bb2d551b", "GCM test case 16 tag")

    # The curve is the curve: G has order n, and every test key lands on it.
    check(scalar_mult(N, (GX, GY)) is None, "P-256 base point order")
    for name, pub in [("helper", HELPER_PUB), ("board", BOARD_PUB),
                      ("seal offerer", SEAL_OFFER_PUB), ("seal accepter", SEAL_ACCEPT_PUB)]:
        point = (int.from_bytes(pub[:32], "big"), int.from_bytes(pub[32:], "big"))
        check(on_curve(point), f"{name} public key on curve")

    # ECDH agrees from both sides — the property the pairing rests on.
    check(ecdh(HELPER_PRIV, BOARD_PUB) == ecdh(BOARD_PRIV, HELPER_PUB), "ECDH agreement")

    # RFC 5869 test case 1, so the KDF is the one everyone else has.
    check(
        hkdf(b"\x0b" * 22, bytes(range(0x00, 0x0D)), bytes(range(0xF0, 0xFA)), 42).hex()
        == "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
        "HKDF-SHA256 RFC 5869 case 1",
    )

    print("self-test ok" if ok else "self-test FAILED", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    if self_test() != 0:
        sys.exit(1)
    sys.stdout.write(emit())
