/*
 * Golden-vector + codec tests for the shared core's frame codec (seam 1).
 * Style follows mkroamer's suite: an assertion macro, a main, a printed
 * failure line, a non-zero exit — no framework.
 *
 * Vector file: test-vectors/frames.txt, the cross-implementation gate.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dh_frame.h"

static int failures = 0;

#define CHECK(cond, name, what)                                                   \
    do {                                                                          \
        if (!(cond)) {                                                            \
            ++failures;                                                           \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, (name), (what));   \
        }                                                                         \
    } while (0)

#define MAX_VECTORS 64
#define MAX_VECTOR_BYTES (DH_FRAME_MAX_SIZE)

struct vector {
    char name[64];
    uint8_t bytes[MAX_VECTOR_BYTES];
    size_t len;
};

static int hex_nibble(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t load_vectors(const char *path, struct vector *out, size_t cap) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[16384];
    size_t n = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;
        char *bar = strchr(p, '|');
        if (!bar) continue;
        if (n >= cap) { /* never silently stop gating vectors */
            ++failures;
            printf("FAIL vector capacity (%zu) exceeded — raise MAX_VECTORS\n", cap);
            break;
        }
        struct vector *v = &out[n];
        size_t name_len = 0;
        for (char *q = p; q < bar && name_len + 1 < sizeof v->name; q++)
            if (!isspace((unsigned char)*q)) v->name[name_len++] = *q;
        v->name[name_len] = '\0';
        v->len = 0;
        int hi = -1;
        for (char *q = bar + 1; *q; q++) {
            if (isspace((unsigned char)*q)) continue;
            int nib = hex_nibble((unsigned char)*q);
            if (nib < 0) { hi = -2; break; }
            if (hi < 0) {
                hi = nib;
            } else {
                if (v->len >= MAX_VECTOR_BYTES) { hi = -2; break; }
                v->bytes[v->len++] = (uint8_t)((hi << 4) | nib);
                hi = -1;
            }
        }
        if (hi != -1) {
            ++failures;
            printf("FAIL bad hex in vector %s\n", v->name);
            continue;
        }
        n++;
    }
    fclose(f);
    return n;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : DH_TEST_VECTORS;
    static struct vector vectors[MAX_VECTORS];
    const size_t nvec = load_vectors(path, vectors, MAX_VECTORS);
    CHECK(nvec >= 15, "load", "vector file missing or too small");

    /* Golden round-trips: decode each vector as exactly one frame, re-encode
     * byte-identically — one-shot and through the incremental reader. */
    for (size_t i = 0; i < nvec; i++) {
        const struct vector *v = &vectors[i];

        dh_frame_view fv;
        size_t consumed = 0;
        CHECK(dh_frame_decode(v->bytes, v->len, &fv, &consumed) == DH_FRAME_OK,
              v->name, "one-shot decode failed");
        CHECK(consumed == v->len, v->name, "decode did not consume whole vector");
        CHECK(dh_msg_type_known(fv.hdr.type), v->name, "type not in registry");

        uint8_t enc[MAX_VECTOR_BYTES];
        size_t enc_len = 0;
        CHECK(dh_frame_encode(fv.hdr.type, fv.hdr.flags, fv.payload, fv.hdr.len,
                              enc, sizeof enc, &enc_len) == DH_FRAME_OK,
              v->name, "re-encode failed");
        CHECK(enc_len == v->len && memcmp(enc, v->bytes, v->len) == 0,
              v->name, "re-encode mismatch");

        /* Header-only parse agrees with the full decode. */
        dh_frame_header hdr;
        CHECK(dh_frame_header_parse(v->bytes, v->len, &hdr) == DH_FRAME_OK,
              v->name, "header parse failed");
        CHECK(hdr.type == fv.hdr.type && hdr.flags == fv.hdr.flags && hdr.len == fv.hdr.len,
              v->name, "header parse disagrees with decode");
    }

    /* Byte-at-a-time delivery of every vector through one reader. */
    {
        dh_frame_reader r;
        dh_frame_reader_init(&r);
        size_t frames = 0;
        for (size_t i = 0; i < nvec; i++) {
            for (size_t j = 0; j < vectors[i].len; j++) {
                size_t consumed = 0;
                dh_frame_view fv;
                dh_frame_result rc =
                    dh_frame_reader_push(&r, &vectors[i].bytes[j], 1, &consumed, &fv);
                CHECK(rc == DH_FRAME_OK || rc == DH_FRAME_AGAIN, vectors[i].name,
                      "reader error on split delivery");
                CHECK(consumed == 1, vectors[i].name, "reader did not consume the byte");
                if (rc == DH_FRAME_OK) {
                    frames++;
                    uint8_t enc[MAX_VECTOR_BYTES];
                    size_t enc_len = 0;
                    CHECK(dh_frame_encode(fv.hdr.type, fv.hdr.flags, fv.payload, fv.hdr.len,
                                          enc, sizeof enc, &enc_len) == DH_FRAME_OK &&
                              enc_len == vectors[i].len &&
                              memcmp(enc, vectors[i].bytes, enc_len) == 0,
                          vectors[i].name, "reader frame re-encode mismatch");
                }
            }
        }
        CHECK(frames == nvec, "split", "frame count mismatch");
    }

    /* All vectors concatenated into one buffer, pushed in one call, then again
     * split at an arbitrary point mid-frame — the transport decides where the
     * boundaries land, not us. */
    {
        static uint8_t wire[MAX_VECTORS * MAX_VECTOR_BYTES];
        size_t wire_len = 0;
        for (size_t i = 0; i < nvec; i++) {
            memcpy(wire + wire_len, vectors[i].bytes, vectors[i].len);
            wire_len += vectors[i].len;
        }

        for (size_t cut = 0; cut <= 2; cut++) {
            const size_t split = cut == 0 ? wire_len : (cut == 1 ? 7 : wire_len - 3);
            dh_frame_reader r;
            dh_frame_reader_init(&r);
            size_t frames = 0;
            const uint8_t *parts[2] = {wire, wire + split};
            const size_t lens[2] = {split, wire_len - split};
            for (int p = 0; p < 2; p++) {
                size_t off = 0;
                while (off < lens[p]) {
                    size_t consumed = 0;
                    dh_frame_view fv;
                    dh_frame_result rc = dh_frame_reader_push(&r, parts[p] + off,
                                                              lens[p] - off, &consumed, &fv);
                    CHECK(rc == DH_FRAME_OK || rc == DH_FRAME_AGAIN, "batched",
                          "reader error on batched delivery");
                    if (rc != DH_FRAME_OK && rc != DH_FRAME_AGAIN) return 1;
                    if (rc == DH_FRAME_OK) frames++;
                    CHECK(consumed > 0, "batched", "reader consumed nothing");
                    if (consumed == 0) return 1;
                    off += consumed;
                }
            }
            CHECK(frames == nvec, "batched", "frame count mismatch");
        }
    }

    /* Carrier padding: the channel's reports are a fixed 64 bytes with no
     * length field of their own, so a sender fills the tail with DH_FRAME_PAD.
     * The reader skips it between frames — and only between frames, since the
     * same byte inside a payload is data the header already accounted for. */
    {
        uint8_t report[64];
        memset(report, DH_FRAME_PAD, sizeof report);
        /* Two frames packed into one report, the second with a zero-heavy
         * payload, then padding to the report boundary. */
        const uint8_t body[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x00};
        size_t len = 0, at = 0;
        CHECK(dh_frame_encode(DH_MSG_HEARTBEAT, 0, NULL, 0, report + at, sizeof report - at,
                              &len) == DH_FRAME_OK,
              "padding", "heartbeat encode failed");
        at += len;
        CHECK(dh_frame_encode(DH_MSG_CLIP_CREDIT, 0, body, sizeof body, report + at,
                              sizeof report - at, &len) == DH_FRAME_OK,
              "padding", "credit encode failed");
        at += len;

        dh_frame_reader r;
        dh_frame_reader_init(&r);
        uint8_t seen[2] = {0};
        size_t frames = 0, off = 0;
        while (off < sizeof report) {
            size_t consumed = 0;
            dh_frame_view fv;
            const dh_frame_result rc =
                dh_frame_reader_push(&r, report + off, sizeof report - off, &consumed, &fv);
            CHECK(rc == DH_FRAME_OK || rc == DH_FRAME_AGAIN, "padding",
                  "padding read as a frame");
            if (rc != DH_FRAME_OK && rc != DH_FRAME_AGAIN) break;
            CHECK(consumed > 0, "padding", "reader consumed nothing");
            if (consumed == 0) break;
            if (rc == DH_FRAME_OK && frames < 2) {
                seen[frames] = fv.hdr.type;
                CHECK(fv.hdr.type != DH_MSG_CLIP_CREDIT || (fv.hdr.len == sizeof body &&
                      memcmp(fv.payload, body, sizeof body) == 0),
                      "padding", "zero bytes inside a payload were skipped");
                frames++;
            }
            off += consumed;
        }
        CHECK(frames == 2, "padding", "packed frames not both recovered");
        CHECK(seen[0] == DH_MSG_HEARTBEAT && seen[1] == DH_MSG_CLIP_CREDIT, "padding",
              "frames recovered out of order");

        /* A report that is nothing but padding yields nothing and errors not. */
        memset(report, DH_FRAME_PAD, sizeof report);
        dh_frame_reader_init(&r);
        size_t consumed = 0;
        dh_frame_view fv;
        CHECK(dh_frame_reader_push(&r, report, sizeof report, &consumed, &fv) == DH_FRAME_AGAIN,
              "padding", "an idle report was not silent");
        CHECK(consumed == sizeof report, "padding", "idle report not consumed");
    }

    /* Over-long length is a protocol error everywhere. */
    {
        const uint8_t bad[] = {DH_MSG_HEARTBEAT, 0x00, 0x01, 0x10}; /* len 0x1001 = 4097 */
        dh_frame_header hdr;
        CHECK(dh_frame_header_parse(bad, sizeof bad, &hdr) == DH_FRAME_ERR_OVERSIZE,
              "oversize", "header parse accepted len 4097");
        dh_frame_view fv;
        size_t consumed = 0;
        CHECK(dh_frame_decode(bad, sizeof bad, &fv, &consumed) == DH_FRAME_ERR_OVERSIZE,
              "oversize", "decode accepted len 4097");
        dh_frame_reader r;
        dh_frame_reader_init(&r);
        CHECK(dh_frame_reader_push(&r, bad, sizeof bad, &consumed, &fv) ==
                  DH_FRAME_ERR_OVERSIZE,
              "oversize", "reader accepted len 4097");
    }

    /* Unknown type is a protocol error everywhere. */
    {
        const uint8_t unknown[] = {0xEE, 0x00, 0x00, 0x00};
        dh_frame_header hdr;
        CHECK(dh_frame_header_parse(unknown, sizeof unknown, &hdr) ==
                  DH_FRAME_ERR_UNKNOWN_TYPE,
              "unknown", "header parse accepted type 0xEE");
        dh_frame_view fv;
        size_t consumed = 0;
        CHECK(dh_frame_decode(unknown, sizeof unknown, &fv, &consumed) ==
                  DH_FRAME_ERR_UNKNOWN_TYPE,
              "unknown", "decode accepted type 0xEE");
        dh_frame_reader r;
        dh_frame_reader_init(&r);
        CHECK(dh_frame_reader_push(&r, unknown, sizeof unknown, &consumed, &fv) ==
                  DH_FRAME_ERR_UNKNOWN_TYPE,
              "unknown", "reader accepted type 0xEE");
        CHECK(!dh_msg_type_known(0xEE), "unknown", "0xEE claimed known");
        CHECK(!dh_msg_type_known(0x00), "unknown", "0x00 claimed known");
    }

    /* A truncated frame is incomplete, never a short frame presented whole —
     * and the reader finishes it when the rest arrives. */
    {
        const uint8_t whole[] = {DH_MSG_CLIP_DONE, 0x00, 0x04, 0x00, 0x02, 0x00, 0x00, 0x00};
        dh_frame_view fv;
        size_t consumed = 0;
        CHECK(dh_frame_decode(whole, 6, &fv, &consumed) == DH_FRAME_AGAIN,
              "truncated", "decode returned a frame from a truncated buffer");
        CHECK(dh_frame_decode(whole, 2, &fv, &consumed) == DH_FRAME_AGAIN,
              "truncated", "decode returned a frame from a partial header");

        dh_frame_reader r;
        dh_frame_reader_init(&r);
        CHECK(dh_frame_reader_push(&r, whole, 6, &consumed, &fv) == DH_FRAME_AGAIN &&
                  consumed == 6,
              "truncated", "reader mishandled the first half");
        CHECK(dh_frame_reader_push(&r, whole + 6, 2, &consumed, &fv) == DH_FRAME_OK &&
                  consumed == 2,
              "truncated", "reader did not complete the frame");
        CHECK(fv.hdr.type == DH_MSG_CLIP_DONE && fv.hdr.len == 4 &&
                  memcmp(fv.payload, whole + 4, 4) == 0,
              "truncated", "completed frame is wrong");
    }

    /* Encode guards: oversize payload, unknown type, short output buffer. */
    {
        static uint8_t big[DH_FRAME_MAX_PAYLOAD + 1];
        uint8_t out[16];
        size_t out_len = 0;
        CHECK(dh_frame_encode(DH_MSG_CLIP_CHUNK, 0, big, sizeof big, out, sizeof out,
                              &out_len) == DH_FRAME_ERR_OVERSIZE,
              "encode", "accepted a 4097-byte payload");
        CHECK(dh_frame_encode(0xEE, 0, NULL, 0, out, sizeof out, &out_len) ==
                  DH_FRAME_ERR_UNKNOWN_TYPE,
              "encode", "accepted an unknown type");
        CHECK(dh_frame_encode(DH_MSG_CLIP_DONE, 0, big, 32, out, sizeof out, &out_len) ==
                  DH_FRAME_ERR_BUFFER,
              "encode", "wrote past a short output buffer");
    }

    /* flags is reserved but preserved verbatim by the codec (docs/protocol.md) —
     * golden vectors all carry 0, so exercise the property here. */
    {
        uint8_t wire[DH_FRAME_HEADER_SIZE];
        size_t wire_len = 0;
        CHECK(dh_frame_encode(DH_MSG_HEARTBEAT, 0x7f, NULL, 0, wire, sizeof wire,
                              &wire_len) == DH_FRAME_OK,
              "flags", "encode with nonzero flags failed");
        dh_frame_view fv;
        size_t consumed = 0;
        CHECK(dh_frame_decode(wire, wire_len, &fv, &consumed) == DH_FRAME_OK &&
                  fv.hdr.flags == 0x7f,
              "flags", "nonzero flags not preserved through decode");
    }

    /* The 4096-byte boundary round-trips. */
    {
        static uint8_t payload[DH_FRAME_MAX_PAYLOAD];
        static uint8_t wire[DH_FRAME_MAX_SIZE];
        for (size_t i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)i;
        size_t wire_len = 0;
        CHECK(dh_frame_encode(DH_MSG_CLIP_CHUNK, 0, payload, sizeof payload, wire,
                              sizeof wire, &wire_len) == DH_FRAME_OK &&
                  wire_len == DH_FRAME_MAX_SIZE,
              "max", "encode of a maximum frame failed");
        dh_frame_view fv;
        size_t consumed = 0;
        CHECK(dh_frame_decode(wire, wire_len, &fv, &consumed) == DH_FRAME_OK &&
                  fv.hdr.len == DH_FRAME_MAX_PAYLOAD &&
                  memcmp(fv.payload, payload, sizeof payload) == 0,
              "max", "decode of a maximum frame failed");
    }

    /* Banding: both firmware decisions are the one comparison. */
    {
        CHECK(!dh_msg_is_bulk(DH_MSG_HELLO) && !dh_msg_is_bulk(DH_MSG_HEARTBEAT) &&
                  !dh_msg_is_bulk(DH_MSG_PAIR_GRANT) && !dh_msg_is_bulk(DH_MSG_PLACE) &&
                  !dh_msg_is_bulk(DH_MSG_POS_RESPONSE),
              "bands", "session/placement type claimed bulk");
        CHECK(dh_msg_is_bulk(DH_MSG_CLIP_OFFER) && dh_msg_is_bulk(DH_MSG_CLIP_CHUNK) &&
                  dh_msg_is_bulk(DH_MSG_CLIP_CREDIT),
              "bands", "bulk type not claimed bulk");
    }

    /* Header-only parsing needs exactly four bytes — the firmware's view. */
    {
        const uint8_t just_header[] = {DH_MSG_CLIP_CHUNK, 0x00, 0x00, 0x10}; /* len 4096 */
        dh_frame_header hdr;
        CHECK(dh_frame_header_parse(just_header, sizeof just_header, &hdr) == DH_FRAME_OK &&
                  hdr.len == DH_FRAME_MAX_PAYLOAD,
              "header-only", "could not parse a header without its payload");
        CHECK(dh_frame_header_parse(just_header, 3, &hdr) == DH_FRAME_AGAIN,
              "header-only", "parsed a 3-byte header");
    }

    if (failures == 0)
        printf("frame_test: all checks passed (%zu vectors)\n", nvec);
    return failures == 0 ? 0 : 1;
}
