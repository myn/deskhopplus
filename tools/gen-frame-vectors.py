#!/usr/bin/env python3
"""Regenerate the golden vectors from docs/protocol.md's v2 layouts.

    python3 tools/gen-frame-vectors.py              > test-vectors/frames.txt
    python3 tools/gen-frame-vectors.py --primitives > test-vectors/primitives.txt

The second file is the same generator one layer down: the four primitives the
board runs (#110), each gated on its own rather than only through a frame.
Both come from this one script because they must agree, and two scripts would
be two implementations of the same arithmetic.

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

    # A reason byte then how long the board had gone unheard, u32 LE (#107).
    # Only a liveness end is asserting anything about a clock, so only it
    # carries a figure; the others are zero.
    for name, reason, counter, silent in [
        ("session_end_liveness", 1, 2, 3000),
        ("session_end_protocol_error", 2, 3, 0),
        ("session_end_unpaired", 3, 4, 0),
    ]:
        v.append((name, frame(0x07, struct.pack("<BI", reason, silent), K_B2H, counter)))

    # 4 refused frames in a 10 s window: a rate, not an event.
    v.append(("listener_alert", frame(0x03, struct.pack("<II", 10000, 4), K_B2H, 5)))

    # The clipboard's two directions, as the two verbs a helper acts on (#52),
    # and the size cap beside them (#56). Both flag forms, because the
    # interesting failure is a flags byte read the wrong way round and the
    # all-allowed case cannot show one; two different caps for the same reason,
    # and neither is the default, so a cap silently dropped on the floor cannot
    # pass as the value the board meant.
    v.append(("clip_policy_both", frame(0x04, bytes([0x03, 0x40]), K_B2H, 8)))
    v.append(("clip_policy_receive_only", frame(0x04, bytes([0x02, 0x20]), K_B2H, 9)))

    # The twelve totals (#133, #142, #107), each a different value so that a field
    # read in the wrong order cannot pass. The first seven are in the order the
    # config page's fields 91-97 already used; the two added by #142 are
    # appended, so those seven keep their offsets.
    v.append(("device_drops",
              frame(0x10, struct.pack("<12I", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12), K_B2H, 10)))

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
#                              listener_alert 5, place 6, pos_query 7,
#                              clip_policy_both 8, clip_policy_receive_only 9,
#                              device_drops 10
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
# The primitive vectors (#110).
#
# frames.txt gates the wire. This gates the four things underneath it, one
# layer down, so that a core which computes SHA-256 wrongly fails on a line
# that says "sha256" rather than on twenty-eight frames at once.
#
# Every vector below whose name carries an RFC or FIPS number is that
# document's published answer, copied in and checked in self_test(). The rest
# are this generator's own, and are the material frames.txt was built from.
# --------------------------------------------------------------------------

# FIPS 180-4, appendix B. The 'a' runs are the padding boundaries: 55 is the
# last length that still fits its padding in one block, 56 is the first that
# does not, and 119/120 are the same boundary one block later.
SHA256_KATS = [
    ("sha256_fips_empty", b""),
    ("sha256_fips_abc", b"abc"),
    ("sha256_fips_two_block", b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
    ("sha256_pad_55", b"a" * 55),
    ("sha256_pad_56", b"a" * 56),
    ("sha256_pad_64", b"a" * 64),
    ("sha256_pad_119", b"a" * 119),
    ("sha256_pad_120", b"a" * 120),
]

# RFC 4231. Case 5 is the truncation case, which this protocol needs because
# the frame tag is HMAC-SHA256 truncated to 16 bytes; cases 6 and 7 are the
# over-long key, which is hashed rather than padded.
HMAC_KATS = [
    ("hmac_rfc4231_1", b"\x0b" * 20, b"Hi There"),
    ("hmac_rfc4231_2", b"Jefe", b"what do ya want for nothing?"),
    ("hmac_rfc4231_3", b"\xaa" * 20, b"\xdd" * 50),
    ("hmac_rfc4231_4", bytes(range(1, 26)), b"\xcd" * 50),
    ("hmac_rfc4231_5", b"\x0c" * 20, b"Test With Truncation"),
    ("hmac_rfc4231_6", b"\xaa" * 131,
     b"Test Using Larger Than Block-Size Key - Hash Key First"),
    ("hmac_rfc4231_7", b"\xaa" * 131,
     b"This is a test using a larger than block-size key and a larger "
     b"than block-size data. The key needs to be hashed before being used"
     b" by the HMAC algorithm."),
]

# RFC 5869. Case 2 is the only one whose output needs more than one expand
# round (82 bytes), and case 3 has an empty salt and empty info.
HKDF_KATS = [
    ("hkdf_rfc5869_1", b"\x0b" * 22, bytes(range(0x00, 0x0D)), bytes(range(0xF0, 0xFA)), 42),
    ("hkdf_rfc5869_2", bytes(range(0x00, 0x50)), bytes(range(0x60, 0xB0)),
     bytes(range(0xB0, 0x100)), 82),
    ("hkdf_rfc5869_3", b"\x0b" * 22, b"", b"", 42),
]

# The three info strings the protocol actually uses, over this generator's own
# material, so a core that derives a session key from the wrong salt or the
# wrong info string fails here and not on a frame tag.
HKDF_PROTOCOL_INFOS = [
    ("hkdf_hello", b"deskhopplus/2 hello"),
    ("hkdf_h2b", b"deskhopplus/2 h2b"),
    ("hkdf_b2h", b"deskhopplus/2 b2h"),
    ("hkdf_seal", b"deskhopplus/2 seal"),
]

# RFC 6979 A.2.5's P-256 key pair — a published pair from outside this
# project, so the curve arithmetic is gated by something this repository did
# not compute itself.
RFC6979_PRIV = bytes.fromhex(
    "C9AFA9D845BA75166B5C215767B1D6934E50C3DB36E89B127B8A622B120F6721")
RFC6979_PUB = bytes.fromhex(
    "60FED4BA255A9D31C961EB74C6356D68C049B8923B61FA6CE669622E60F29FB6"
    "7903FE1008B8BC99A41AE9E95628BC64F2F1B20C2D7E9F5177A3C294D4462299")


def build_primitives():
    """(name, [field, ...]) pairs; every field is raw bytes, printed as hex."""
    v = []

    for name, msg in SHA256_KATS:
        v.append((name, [msg, hashlib.sha256(msg).digest()]))

    for name, key, msg in HMAC_KATS:
        v.append((name, [key, msg, hmac.new(key, msg, hashlib.sha256).digest()]))

    for name, ikm, salt, info, length in HKDF_KATS:
        v.append((name, [ikm, salt, info, hkdf(ikm, salt, info, length)]))

    for name, info in HKDF_PROTOCOL_INFOS:
        salt = HELPER_NONCE if info.endswith(b"hello") else HELPER_NONCE + BOARD_NONCE
        ikm = SHARED
        if info.endswith(b"seal"):
            salt = SEAL_OFFER_NONCE + SEAL_ACCEPT_NONCE
            ikm = ecdh(SEAL_OFFER_PRIV, SEAL_ACCEPT_PUB)
        v.append((name, [ikm, salt, info, hkdf(ikm, salt, info)]))

    # Private key in, public key out. Deterministic: this core generates a key
    # from entropy the caller hands it, and has no entropy source of its own.
    for name, priv in [
        ("p256_pub_rfc6979", RFC6979_PRIV),
        ("p256_pub_helper", HELPER_PRIV),
        ("p256_pub_board", BOARD_PRIV),
        ("p256_pub_seal_offer", SEAL_OFFER_PRIV),
        ("p256_pub_seal_accept", SEAL_ACCEPT_PRIV),
    ]:
        v.append((name, [priv, public_key(priv)]))

    # Both directions of each agreement, because "the two sides compute the
    # same secret" is the property pairing rests on and a one-sided vector
    # cannot express it.
    for name, priv, peer in [
        ("p256_ecdh_helper_to_board", HELPER_PRIV, BOARD_PUB),
        ("p256_ecdh_board_to_helper", BOARD_PRIV, HELPER_PUB),
        ("p256_ecdh_seal_offer", SEAL_OFFER_PRIV, SEAL_ACCEPT_PUB),
        ("p256_ecdh_seal_accept", SEAL_ACCEPT_PRIV, SEAL_OFFER_PUB),
    ]:
        v.append((name, [priv, peer, ecdh(priv, peer)]))

    # Must be refused. A public key arrives from the wire, so "is this a point
    # on P-256" is a decision the core makes about hostile input, not an
    # assertion about its own material.
    v.append(("p256_reject_pub_zero", [bytes(64)]))
    v.append(("p256_reject_pub_off_curve", [BOARD_PUB[:63] + bytes([BOARD_PUB[63] ^ 1])]))
    v.append(("p256_reject_pub_x_is_p", [P.to_bytes(32, "big") + BOARD_PUB[32:]]))
    # And these are not scalars in [1, n-1], which is the range a private key
    # must land in — the caller hands in 32 raw bytes and draws again on false.
    v.append(("p256_reject_priv_zero", [bytes(32)]))
    v.append(("p256_reject_priv_n", [N.to_bytes(32, "big")]))
    v.append(("p256_reject_priv_max", [b"\xff" * 32]))

    for name, pub in [("keyid_helper", HELPER_PUB), ("keyid_board", BOARD_PUB)]:
        v.append((name, [pub, hashlib.sha256(pub).digest()[:8]]))

    # One line carrying everything a session's keys are made of, in the order
    # docs/protocol.md derives them. It is what lets the C suite verify every
    # tag in frames.txt rather than only re-encoding its bytes.
    v.append(("session_material", [
        HELPER_PRIV, BOARD_PRIV, HELPER_NONCE, BOARD_NONCE,
        SHARED, K_HELLO, K_H2B, K_B2H,
    ]))

    return v


PRIMITIVES_HEADER = """\
# deskhopplus golden primitive vectors — the gate on what the frame tags are made of.
# Format: <name> | <field> | <field> ... — hex bytes, spaces ignored, a field may be empty.
# The field list depends on the name's prefix, and is written above each section below.
#
# GENERATED. Do not hand-edit: run
#   python3 tools/gen-frame-vectors.py --primitives > test-vectors/primitives.txt
#
# test-vectors/frames.txt gates the wire. This file gates the four primitives underneath
# it (#110, ADR-0008) one layer down, so a core that computes SHA-256 wrongly fails on a
# line that says so rather than on twenty-eight frames at once. The board runs exactly
# these four: SHA-256, HMAC-SHA256, HKDF-SHA256 and P-256 ECDH. There is no AEAD here —
# the clipboard seal is AES-256-GCM and runs helper to helper, never on the firmware.
#
# Every vector named for an RFC or a FIPS publication is that document's own published
# answer, so the curve and the hashes are gated by something this repository did not
# compute for itself. The rest are the generator's fixed test material — published,
# obviously fake, and the same bytes frames.txt was built from.
"""

PRIMITIVE_SECTIONS = [
    ("sha256_", "# sha256      | message | digest"),
    ("hmac_", "# hmac_sha256 | key | message | mac"),
    ("hkdf_", "# hkdf_sha256 | ikm | salt | info | okm   (the output length is the okm's)"),
    ("p256_pub_", "# p256_pub    | private | public (X || Y, big-endian)"),
    ("p256_ecdh_", "# p256_ecdh   | private | peer public | shared secret (the X coordinate)"),
    ("p256_reject_", "# p256_reject | the value, which every one of these must refuse"),
    ("keyid_", "# keyid       | public | SHA-256(public)[0..8]"),
    ("session_", "# session     | helper private | board private | helper nonce | board nonce |\n"
                 "#             | shared secret | k_hello | k_h2b | k_b2h"),
]


def emit_primitives():
    lines = [PRIMITIVES_HEADER]
    remaining = build_primitives()
    for prefix, caption in PRIMITIVE_SECTIONS:
        lines.append(caption)
        for name, fields in remaining:
            if name.startswith(prefix):
                lines.append(
                    f"{name:<28} | " + " | ".join(" ".join(f"{b:02x}" for b in f) for f in fields)
                )
        remaining = [(n, f) for n, f in remaining if not n.startswith(prefix)]
        lines.append("")
    assert not remaining, f"vectors in no section: {[n for n, _ in remaining]}"
    return "\n".join(lines[:-1]) + "\n"


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

    # The primitive vectors' published answers (#110). These are the numbers
    # printed in FIPS 180-4, RFC 4231, RFC 5869 and RFC 6979 — the point of
    # copying them in is that this generator agrees with documents nobody here
    # wrote, so the C core is gated by more than a second opinion of our own.
    for name, expected in [
        ("sha256_fips_empty",
         "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
        ("sha256_fips_abc",
         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
        ("sha256_fips_two_block",
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"),
    ]:
        msg = dict((n, m) for n, m in SHA256_KATS)[name]
        check(hashlib.sha256(msg).hexdigest() == expected, f"FIPS 180-4 {name}")

    rfc4231 = [
        "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
        "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
        "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe",
        "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b",
        "a3b6167473100ee06e0c796c2955552bfa6f7c0a6a8aef8b93f860aab0cd20c5",
        "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54",
        "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2",
    ]
    for (name, key, msg), expected in zip(HMAC_KATS, rfc4231):
        got = hmac.new(key, msg, hashlib.sha256).hexdigest()
        check(got == expected, f"RFC 4231 {name}")

    rfc5869 = [
        "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf"
        "34007208d5b887185865",
        "b11e398dc80327a1c8e7f78c596a49344f012eda2d4efad8a050cc4c19afa97c"
        "59045a99cac7827271cb41c65e590e09da3275600c2f09b8367793a9aca3db71"
        "cc30c58179ec3e87c14c01d5c1f3434f1d87",
        "8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d"
        "9d201395faa4b61a96c8",
    ]
    for (name, ikm, salt, info, length), expected in zip(HKDF_KATS, rfc5869):
        check(hkdf(ikm, salt, info, length).hex() == expected, f"RFC 5869 {name}")

    # RFC 6979 A.2.5: a P-256 key pair computed by somebody else. This is also
    # an ECDH known answer, the peer public key being the base point.
    check(public_key(RFC6979_PRIV) == RFC6979_PUB, "RFC 6979 A.2.5 P-256 key pair")

    # The rejects have to actually be rejectable, or they gate nothing.
    check(not on_curve((int.from_bytes(BOARD_PUB[:32], "big"),
                        int.from_bytes(BOARD_PUB[32:], "big") ^ 1)),
          "off-curve reject vector is off the curve")

    print("self-test ok" if ok else "self-test FAILED", file=sys.stderr)
    return 0 if ok else 1


if __name__ == "__main__":
    if "--self-test" in sys.argv:
        sys.exit(self_test())
    if self_test() != 0:
        sys.exit(1)
    sys.stdout.write(emit_primitives() if "--primitives" in sys.argv else emit())
