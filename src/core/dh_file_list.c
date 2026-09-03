#include "dh_file_list.h"

#include <string.h>

/*
 * Bytes that may not appear in a name.
 *
 * The union of both platforms' rules plus JSON's, which is what makes the
 * escape-free wire format above true rather than hoped for: `"` and `\` are
 * the only two characters a JSON string would have to escape, and both are
 * already illegal on Windows.
 */
static bool byte_is_forbidden(unsigned char c) {
    if (c < 0x20u || c == 0x7Fu) return true; /* control characters */
    return strchr("/\\:*?\"<>|", (char)c) != NULL;
}

/* Case-insensitive compare of `len` bytes against an upper-case literal. */
static bool matches_upper(const char *name, size_t len, const char *upper) {
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (c != upper[i]) return false;
    }
    return true;
}

/*
 * Names Windows resolves to a device however legal they are elsewhere, matched
 * against the part before the first dot.
 *
 * Two shapes, and they are the same length in the second case, which is what
 * an earlier reading of this got wrong: CON, PRN, AUX and NUL are three
 * characters, and COM0-9 and LPT0-9 are four — three letters and a digit —
 * so both port families are the *same* stem length and neither can be selected
 * by length alone.
 */
static bool stem_is_reserved_device(const char *name, size_t len) {
    static const char *const bare[] = {"CON", "PRN", "AUX", "NUL"};
    size_t stem = 0;
    while (stem < len && name[stem] != '.') stem++;

    if (stem == 3) {
        for (size_t i = 0; i < sizeof bare / sizeof bare[0]; i++)
            if (matches_upper(name, 3, bare[i])) return true;
        return false;
    }
    if (stem == 4 && name[3] >= '0' && name[3] <= '9')
        return matches_upper(name, 3, "COM") || matches_upper(name, 3, "LPT");
    return false;
}

static size_t write_fallback(char *out, size_t cap) {
    const size_t n = strlen(DH_FILE_NAME_FALLBACK);
    if (cap < n) return 0;
    memcpy(out, DH_FILE_NAME_FALLBACK, n);
    return n;
}

size_t dh_file_name_clean(const char *in, size_t in_len, char *out, size_t cap) {
    if (out == NULL || cap == 0) return 0;
    if (in == NULL) in_len = 0;

    /*
     * Truncated on a UTF-8 boundary. Cutting mid-sequence would leave a name
     * neither platform can render, and the sequence's own bytes say where the
     * boundary is: a continuation byte is 10xxxxxx, so backing off over those
     * lands on the lead byte, which is then dropped with them.
     */
    size_t take = in_len < DH_FILE_NAME_MAX ? in_len : DH_FILE_NAME_MAX;
    if (take < in_len) {
        while (take > 0 && ((unsigned char)in[take] & 0xC0u) == 0x80u) take--;
    }

    size_t at = 0;
    for (size_t i = 0; i < take && at < cap; i++) {
        out[at++] = byte_is_forbidden((unsigned char)in[i]) ? '_' : in[i];
    }
    if (at < take) return 0; /* `cap` could not hold the cleaned name */

    /*
     * Windows strips trailing dots and spaces when it opens a file, so a name
     * ending in one is a name that does not round-trip through a paste.
     *
     * This is also what disposes of `.` and `..`, which are the names that
     * matter here: a name of nothing but dots strips away to nothing and
     * becomes the fallback below. There is no separate check for them, and one
     * would be unreachable.
     */
    while (at > 0 && (out[at - 1] == ' ' || out[at - 1] == '.')) at--;

    if (at == 0) return write_fallback(out, cap);

    if (stem_is_reserved_device(out, at)) {
        /* Prefixed rather than refused: the transfer failing over a file
           called `NUL.txt` is a worse answer than one delivered under a name
           a file manager shows slightly differently. */
        if (at + 1 > cap) return 0;
        /*
         * The prefix has to fit inside DH_FILE_NAME_MAX, not merely inside
         * `cap`. A name already at the limit would otherwise leave here one
         * byte over it, go out on the wire, and be refused by the decoder at
         * the far end — the whole transfer lost over a rename this end chose.
         * Room is made on a UTF-8 boundary, as the truncation above is.
         */
        if (at + 1 > DH_FILE_NAME_MAX) {
            at = DH_FILE_NAME_MAX - 1;
            while (at > 0 && ((unsigned char)out[at] & 0xC0u) == 0x80u) at--;
        }
        memmove(out + 1, out, at);
        out[0] = '_';
        at++;
    }
    return at;
}

bool dh_file_name_is_clean(const char *name, size_t len) {
    if (name == NULL) return false;
    /* Cleaned into a scratch buffer and compared, rather than restated as a
       second set of rules that could drift from the first. */
    char cleaned[DH_FILE_NAME_MAX + 1];
    const size_t n = dh_file_name_clean(name, len, cleaned, sizeof cleaned);
    return n == len && n > 0 && memcmp(cleaned, name, n) == 0;
}

int dh_file_list_encode(const dh_file_entry *entries, uint16_t count, char *out, size_t cap) {
    if (entries == NULL || out == NULL || count == 0 || count > DH_FILE_LIST_MAX) return -1;
    if (cap > dh_file_list_encode_max()) cap = dh_file_list_encode_max();

    size_t at = 0;
#define PUT(byte) do { if (at >= cap) return -1; out[at++] = (byte); } while (0)
#define PUT_TEXT(text, n) do { \
        if (at + (n) > cap) return -1; \
        memcpy(out + at, (text), (n)); \
        at += (n); \
    } while (0)

    PUT('[');
    for (uint16_t i = 0; i < count; i++) {
        if (i > 0) PUT(',');
        PUT_TEXT("{\"name\":\"", 9u);

        char cleaned[DH_FILE_NAME_MAX + 1];
        const size_t n = dh_file_name_clean(entries[i].name, entries[i].name_len, cleaned,
                                            sizeof cleaned);
        if (n == 0) return -1;
        PUT_TEXT(cleaned, n);

        PUT_TEXT("\",\"size\":", 9u);

        /* Written back to front into a scratch buffer: 20 digits is the widest
           a uint64_t reaches, and the value's own length is not known until it
           has been divided down. */
        char digits[20];
        size_t ndigits = 0;
        uint64_t value = entries[i].size;
        do {
            digits[ndigits++] = (char)('0' + (value % 10u));
            value /= 10u;
        } while (value != 0);
        if (at + ndigits > cap) return -1;
        while (ndigits > 0) out[at++] = digits[--ndigits];

        PUT('}');
    }
    PUT(']');
#undef PUT
#undef PUT_TEXT
    return (int)at;
}

/* ------------------------------------------------------------------ decoding
 *
 * A parser for exactly the shape above and nothing wider. Whitespace between
 * tokens is tolerated so the format is genuinely JSON rather than a lookalike;
 * everything else — key order, the absence of escapes, unsigned sizes — is
 * required, because a decoder that accepts more than one spelling of the same
 * list is a second format nobody wrote down.
 */

typedef struct {
    const char *at;
    size_t left;
} cursor;

static void skip_space(cursor *c) {
    while (c->left > 0 && (*c->at == ' ' || *c->at == '\t' || *c->at == '\n' || *c->at == '\r')) {
        c->at++;
        c->left--;
    }
}

static bool take(cursor *c, char expected) {
    skip_space(c);
    if (c->left == 0 || *c->at != expected) return false;
    c->at++;
    c->left--;
    return true;
}

static bool take_text(cursor *c, const char *expected) {
    skip_space(c);
    const size_t n = strlen(expected);
    if (c->left < n || memcmp(c->at, expected, n) != 0) return false;
    c->at += n;
    c->left -= n;
    return true;
}

/* A JSON string with no escape in it. One that needs an escape is refused
   rather than unescaped: the copy side's cleaner never produces a name
   containing `"` or `\`, so an escape here is a far end this one did not
   write. */
static bool take_string(cursor *c, const char **out, uint16_t *out_len) {
    if (!take(c, '"')) return false;
    const char *start = c->at;
    size_t n = 0;
    while (n < c->left && c->at[n] != '"') {
        if (c->at[n] == '\\') return false;
        n++;
    }
    if (n >= c->left) return false; /* unterminated */
    if (n > DH_FILE_NAME_MAX) return false;
    c->at += n + 1;
    c->left -= n + 1;
    *out = start;
    *out_len = (uint16_t)n;
    return true;
}

static bool take_u64(cursor *c, uint64_t *out) {
    skip_space(c);
    uint64_t value = 0;
    size_t digits = 0;
    while (c->left > 0 && *c->at >= '0' && *c->at <= '9') {
        const uint64_t digit = (uint64_t)(*c->at - '0');
        if (value > (UINT64_MAX - digit) / 10u) return false;
        value = value * 10u + digit;
        c->at++;
        c->left--;
        digits++;
    }
    if (digits == 0) return false;
    *out = value;
    return true;
}

bool dh_file_list_decode(const char *json, size_t len, dh_file_list *out) {
    if (json == NULL || out == NULL) return false;

    cursor c = {.at = json, .left = len};
    out->count = 0;
    out->total = 0;

    if (!take(&c, '[')) return false;
    skip_space(&c);
    if (c.left == 0) return false;

    while (*c.at != ']') {
        if (out->count > 0 && !take(&c, ',')) return false;
        if (out->count >= DH_FILE_LIST_MAX) return false;

        dh_file_entry *entry = &out->entries[out->count];
        if (!take(&c, '{')) return false;
        if (!take_text(&c, "\"name\"") || !take(&c, ':')) return false;
        if (!take_string(&c, &entry->name, &entry->name_len)) return false;
        if (!dh_file_name_is_clean(entry->name, entry->name_len)) return false;
        if (!take(&c, ',')) return false;
        if (!take_text(&c, "\"size\"") || !take(&c, ':')) return false;
        if (!take_u64(&c, &entry->size)) return false;
        if (!take(&c, '}')) return false;

        if (out->total > UINT64_MAX - entry->size) return false;
        out->total += entry->size;
        out->count++;

        skip_space(&c);
        if (c.left == 0) return false;
    }

    if (!take(&c, ']')) return false;
    skip_space(&c);
    /* An empty list is well formed and delivers nothing, so it is refused
       here rather than left for a caller to discover it has no files. */
    return c.left == 0 && out->count > 0;
}
