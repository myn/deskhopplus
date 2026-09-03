/*
 * deskhopplus shared core — the file list a kind-2 CLIP_OFFER carries (#56).
 *
 * The offer's metadata names the files a transfer is about to deliver and how
 * long each one is. The chunk stream itself carries no boundaries: a file's
 * bytes begin at the sum of the sizes ahead of it, which is the same
 * arithmetic reassembly already does for chunks. So this list is not a
 * convenience — without it the payload is one anonymous run of bytes.
 *
 * Layout, per docs/protocol.md: a UTF-8 JSON array of objects, each with
 * `name` then `size`, in that order, with no escapes anywhere in it. That
 * last part is the point of `dh_file_name_clean` below, and it is what lets
 * the decoder hand out views into the buffer it was given rather than copies.
 *
 * ---------------------------------------------------------------------------
 * WHY NAMES ARE CLEANED ON THE WAY OUT AND CHECKED ON THE WAY IN
 *
 * A name that arrives from the other computer is used to build a path inside
 * this one's temporary directory. `../` in that name is a write to somewhere
 * else entirely, so something on the path has to refuse it.
 *
 * The copy side cleans, because it is the end that still knows what the file
 * was called and can make a name both operating systems will open. The paste
 * side **refuses rather than repairs**: repairing would deliver a file under a
 * name the copy side never sent, which is a quieter kind of wrong, and the far
 * end is not necessarily ours.
 *
 * This is not a fidelity carve-out (ADR-0003). Fidelity is the *content*
 * guarantee — the bytes are byte-identical end to end — and a file name is
 * metadata. Windows cannot hold a name macOS allows, so some mapping exists
 * whatever we do; doing it once, here, is what makes both ends agree on what
 * the mapping was.
 */

#ifndef DH_FILE_LIST_H_
#define DH_FILE_LIST_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dh_clip.h"
#include "dh_xfer.h"

/* C++ links these symbols too — the Windows helper is C++ (#49). */
#ifdef __cplusplus
extern "C" {
#endif

/*
 * How many files one transfer may name.
 *
 * The real limit is the wire: the metadata must fit a single CLIP_OFFER
 * (DH_CLIP_OFFER_META_WIRE_MAX, a little under 4 KB), so a list of long names
 * runs out of room well before this. Sixty-four is a bound on the decoded
 * structure so it can be a plain array with no allocation, and the encoder
 * reports the wire limit separately when a list is short but wordy.
 */
#define DH_FILE_LIST_MAX 64u

/* The longest name that goes on the wire, in bytes. Longer ones are truncated
   on a UTF-8 boundary, never mid-character. */
#define DH_FILE_NAME_MAX 255u

/* What a name is called when cleaning leaves nothing usable — an empty name,
   or one that was only dots and separators. */
#define DH_FILE_NAME_FALLBACK "file"

/*
 * One file. `name` is a **view**, valid only as long as the buffer it was
 * decoded from; it is UTF-8 and is not null-terminated.
 */
typedef struct {
    const char *name;
    uint16_t name_len;
    uint64_t size;
} dh_file_entry;

typedef struct {
    dh_file_entry entries[DH_FILE_LIST_MAX];
    uint16_t count;
    /* The sum of every size, which the offer's own `total` must equal. Kept
       here because summing it is where an overflow would be missed. */
    uint64_t total;
} dh_file_list;

/*
 * Rewrite `in` as a name both operating systems will open, into `out`.
 *
 * Returns the length written, which is never zero: a name that cleans away to
 * nothing becomes DH_FILE_NAME_FALLBACK. Returns 0 only when `cap` is too
 * small to hold even that, which a caller with a DH_FILE_NAME_MAX + 1 buffer
 * cannot reach. The result is not null-terminated.
 *
 * Two names can clean to the same result — the caller that writes them to disk
 * owns the collision.
 */
size_t dh_file_name_clean(const char *in, size_t in_len, char *out, size_t cap);

/*
 * The largest metadata `dh_file_list_encode` will produce, and so the smallest
 * buffer worth offering it.
 *
 * It is what the *receiver* accepts, not what the frame could hold. Those are
 * different numbers — an offer's metadata could be nearly 4 KB on the wire —
 * and the smaller one is the real limit for a reason that is not the codec's:
 * `DH_XFER_META_MAX` sizes the board's staging slot (`DH_OUTQ_STAGE_MAX`), and
 * an offer past it is cancelled by the receiver on arrival. An encoder that
 * produced one would send a list nobody accepts and report success, and the
 * copy side would learn of it only as a bare cancel it cannot explain.
 *
 * A function rather than the constant itself because a Swift import cannot
 * follow the arithmetic behind it, and because what a caller wants to know is
 * how big its buffer must be.
 */
static inline size_t dh_file_list_encode_max(void) { return DH_XFER_META_MAX; }

/* Whether `name` is already exactly what `dh_file_name_clean` would produce.
   The decoder's boundary check, and the one predicate that says a name is safe
   to join to a directory path. */
bool dh_file_name_is_clean(const char *name, size_t len);

/*
 * Encode `count` entries, cleaning each name on the way. Returns the length
 * written, or -1 if the list is empty, longer than DH_FILE_LIST_MAX, would
 * overrun `cap`, or would not fit one CLIP_OFFER.
 *
 * The entries' own `name_len` is what is read; the caller's names need not be
 * null-terminated and need not be clean.
 */
int dh_file_list_encode(const dh_file_entry *entries, uint16_t count, char *out, size_t cap);

/*
 * Decode metadata that arrived from the other computer. False on anything
 * malformed, on an empty list, on more entries than DH_FILE_LIST_MAX, on a
 * size or total that will not fit 64 bits, and on any name that is not
 * already clean.
 *
 * The names in `out` point into `json`.
 */
bool dh_file_list_decode(const char *json, size_t len, dh_file_list *out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DH_FILE_LIST_H_ */
