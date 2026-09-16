/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

/*
 * deskhopplus shared core — the bundle a kind-3 CLIP_OFFER carries (#195).
 *
 * Every Office application puts a rendered picture of a text selection on
 * the clipboard beside the text, and a copy side that sends one kind per copy
 * has to choose. A bundle sends both, as one transfer, so that the pasting
 * application picks the representation it wants — the way it would from a
 * native clipboard (ADR-0013).
 *
 * Layout, per docs/protocol.md: a list of parts, each `part_kind:u8 len:u32
 * bytes`, integers little-endian like every other integer on the wire.
 * `part_kind` reuses the offer kinds — 0 is UTF-8 text, 1 is PNG — so that
 * RTF or HTML later is a new part kind and not a new offer kind. The offer's
 * metadata is empty; there is nothing to say that the parts do not say.
 *
 * Here rather than in each helper for the reason `dh_file_list` is: two
 * implementations of a format are two formats.
 */

#ifndef DH_BUNDLE_H_
#define DH_BUNDLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* C++ links these symbols too — the Windows helper is C++ (#49). */
#ifdef __cplusplus
extern "C" {
#endif

/* The part kinds a helper writes. The same numbers as the offer kinds. */
#define DH_BUNDLE_PART_TEXT 0u
#define DH_BUNDLE_PART_PNG 1u

/*
 * How many parts one bundle may hold once unpacked. Two kinds exist and a
 * bundle is one copy, so two is the working number; four leaves room for RTF
 * and HTML without a wire change. A bound on the decoded structure so it can
 * be a plain array with no allocation.
 */
#define DH_BUNDLE_PARTS_MAX 4u

/* The bytes a part carries a header of: kind (1) and length (4). */
#define DH_BUNDLE_PART_HEAD 5u

/*
 * One part. `bytes` is a **view**: on the way out it is the caller's buffer,
 * and on the way in it points into the payload it was unpacked from and is
 * valid only as long as that is.
 */
typedef struct {
    uint8_t kind;
    const uint8_t *bytes;
    uint32_t len;
} dh_bundle_part;

typedef struct {
    dh_bundle_part parts[DH_BUNDLE_PARTS_MAX];
    uint8_t count;
} dh_bundle;

/* How long `dh_bundle_pack` will make these parts: what the offer's total is,
   and the smallest buffer worth handing it. Zero when the list is empty. */
size_t dh_bundle_packed_len(const dh_bundle_part *parts, uint8_t count);

/*
 * Pack `count` parts, in order. Returns the length written, or 0 if the list
 * is empty, longer than DH_BUNDLE_PARTS_MAX, or would overrun `cap`. A packed
 * bundle is never zero bytes, so zero is unambiguous.
 */
size_t dh_bundle_pack(const dh_bundle_part *parts, uint8_t count, uint8_t *out, size_t cap);

/*
 * Unpack a payload that arrived from the other computer. False when a part's
 * length runs past the end of the payload, when more parts arrive than
 * DH_BUNDLE_PARTS_MAX, or when nothing this helper writes is in it — a bundle
 * of nothing has nothing to deliver, as an empty file list has.
 *
 * A part of a kind this helper does not know is skipped, not refused: a
 * newer far helper adding RTF must not cost this one the text beside it.
 *
 * The parts in `out` point into `payload`.
 */
bool dh_bundle_unpack(const uint8_t *payload, size_t len, dh_bundle *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DH_BUNDLE_H_ */
