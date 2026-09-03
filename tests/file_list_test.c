/*
 * The file-list metadata a kind-2 CLIP_OFFER carries (#56).
 *
 * The expected JSON in these tests is written by hand rather than produced by
 * the encoder, so a change of shape fails here instead of agreeing with
 * itself.
 *
 * The refusals matter more than the round trips. A name arrives from the other
 * computer and is used to build a path in this one's temporary directory, so
 * the decoder is the boundary that stops `../` reaching a real filesystem
 * call. It refuses rather than repairs, deliberately: repairing would deliver
 * a file under a name the copy side never sent, and the copy side's own
 * encoder has already made every name it produces acceptable.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "dh_file_list.h"

static int failures;
#define CHECK(condition, message) do { \
    if (!(condition)) { fprintf(stderr, "FAIL file_list: %s\n", message); failures++; } \
} while (0)

static bool entry_is(const dh_file_entry *entry, const char *name, uint64_t size) {
    const size_t len = strlen(name);
    return entry->size == size && entry->name_len == len &&
           memcmp(entry->name, name, len) == 0;
}

static void test_one_file_round_trips(void) {
    const dh_file_entry out[] = {{.name = "notes.txt", .name_len = 9, .size = 1234}};
    char json[256];
    const int written = dh_file_list_encode(out, 1, json, sizeof json);

    CHECK(written > 0, "a one-file list would not encode");
    CHECK(written == (int)strlen("[{\"name\":\"notes.txt\",\"size\":1234}]") &&
              memcmp(json, "[{\"name\":\"notes.txt\",\"size\":1234}]", (size_t)written) == 0,
          "a one-file list did not encode to the layout docs/protocol.md states");

    dh_file_list list;
    CHECK(dh_file_list_decode(json, (size_t)written, &list), "the encoded list would not decode");
    CHECK(list.count == 1, "a one-file list did not decode to one entry");
    CHECK(entry_is(&list.entries[0], "notes.txt", 1234), "the entry did not survive the round trip");
    CHECK(list.total == 1234, "the total is not the entry's size");
}

static void test_several_files_keep_their_order_and_sum(void) {
    const dh_file_entry out[] = {
        {.name = "a.txt", .name_len = 5, .size = 10},
        {.name = "b.bin", .name_len = 5, .size = 0},
        {.name = "c.png", .name_len = 5, .size = 4096},
    };
    char json[256];
    const int written = dh_file_list_encode(out, 3, json, sizeof json);
    CHECK(written > 0, "a three-file list would not encode");

    dh_file_list list;
    CHECK(dh_file_list_decode(json, (size_t)written, &list), "a three-file list would not decode");
    CHECK(list.count == 3, "a three-file list did not decode to three entries");
    CHECK(entry_is(&list.entries[0], "a.txt", 10) && entry_is(&list.entries[1], "b.bin", 0) &&
              entry_is(&list.entries[2], "c.png", 4096),
          "the entries did not survive the round trip in order");
    CHECK(list.total == 4106, "the total is not the sum of the entries");
}

/*
 * The chunk stream carries no boundaries — a file's bytes are found by summing
 * the sizes ahead of it — so an entry of zero length is the one case where two
 * files start at the same offset. It has to survive, or an empty file
 * silently takes the next file's contents.
 */
static void test_a_zero_length_file_keeps_its_place(void) {
    const dh_file_entry out[] = {
        {.name = "empty", .name_len = 5, .size = 0},
        {.name = "after", .name_len = 5, .size = 7},
    };
    char json[128];
    const int written = dh_file_list_encode(out, 2, json, sizeof json);
    dh_file_list list;
    CHECK(written > 0 && dh_file_list_decode(json, (size_t)written, &list),
          "a list beginning with an empty file would not round trip");
    CHECK(list.count == 2 && list.entries[0].size == 0 && list.entries[1].size == 7,
          "an empty file did not keep its place in the list");
}

static void test_the_encoder_makes_every_name_safe_to_write(void) {
    const dh_file_entry out[] = {
        {.name = "reports/2026/q3.pdf", .name_len = 19, .size = 1},
        {.name = "..", .name_len = 2, .size = 2},
        {.name = "he said \"hi\".txt", .name_len = 16, .size = 3},
    };
    char json[512];
    const int written = dh_file_list_encode(out, 3, json, sizeof json);
    CHECK(written > 0, "a list of awkward names would not encode");

    dh_file_list list;
    CHECK(dh_file_list_decode(json, (size_t)written, &list),
          "the encoder produced names its own decoder refuses");
    CHECK(entry_is(&list.entries[0], "reports_2026_q3.pdf", 1),
          "a path separator survived encoding");
    CHECK(list.entries[1].name_len > 0 && list.entries[1].name[0] != '.',
          "a name of nothing but dots survived encoding");
    CHECK(memchr(json, '\\', (size_t)written) == NULL &&
              entry_is(&list.entries[2], "he said _hi_.txt", 3),
          "a quote survived encoding, so the JSON needed an escape");
}

/*
 * The security boundary. Everything here is a name this end could be handed by
 * a far helper that is not ours, and each one is a path that would leave the
 * temporary directory if it reached a filesystem call unexamined.
 */
static void test_a_name_that_could_escape_is_refused(void) {
    static const char *const hostile[] = {
        "[{\"name\":\"../escape.txt\",\"size\":1}]",
        "[{\"name\":\"..\",\"size\":1}]",
        "[{\"name\":\".\",\"size\":1}]",
        "[{\"name\":\"/etc/passwd\",\"size\":1}]",
        "[{\"name\":\"sub/dir.txt\",\"size\":1}]",
        "[{\"name\":\"C:\\\\windows\\\\evil\",\"size\":1}]",
        "[{\"name\":\"\",\"size\":1}]",
    };
    for (size_t i = 0; i < sizeof hostile / sizeof hostile[0]; i++) {
        dh_file_list list;
        CHECK(!dh_file_list_decode(hostile[i], strlen(hostile[i]), &list),
              "a name that could leave the temporary directory was accepted");
    }
}

static void test_malformed_metadata_is_refused(void) {
    static const char *const bad[] = {
        "",
        "[",
        "[]x",
        "[{\"name\":\"a\"}]",                      /* no size */
        "[{\"size\":1}]",                          /* no name */
        "[{\"name\":\"a\",\"size\":-1}]",          /* sizes are unsigned */
        "[{\"name\":\"a\",\"size\":1}",            /* unterminated */
        "[{\"name\":a,\"size\":1}]",               /* unquoted name */
        "[{\"name\":\"a\",\"size\":99999999999999999999}]", /* past 64 bits */
        "{\"name\":\"a\",\"size\":1}",             /* an object, not a list */
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        dh_file_list list;
        CHECK(!dh_file_list_decode(bad[i], strlen(bad[i]), &list),
              "malformed file metadata was accepted");
    }
}

/*
 * An empty list is well-formed and carries no files. It is refused all the
 * same: a transfer of nothing has nothing to deliver, and accepting one would
 * put an empty file reference on the pasting computer's clipboard.
 */
static void test_an_empty_list_is_refused(void) {
    dh_file_list list;
    CHECK(!dh_file_list_decode("[]", 2, &list), "a list naming no files was accepted");
    CHECK(dh_file_list_encode(NULL, 0, (char[64]){0}, 64) < 0, "a list of no files encoded");
}

static void test_more_files_than_the_list_holds_is_refused(void) {
    char json[8192];
    size_t at = 0;
    json[at++] = '[';
    for (unsigned i = 0; i <= DH_FILE_LIST_MAX; i++) {
        const int n = snprintf(json + at, sizeof json - at, "%s{\"name\":\"f%u\",\"size\":1}",
                               i == 0 ? "" : ",", i);
        at += (size_t)n;
    }
    json[at++] = ']';

    dh_file_list list;
    CHECK(!dh_file_list_decode(json, at, &list), "a list longer than the decoder holds was accepted");
}

static void test_a_total_that_overflows_is_refused(void) {
    static const char *const json =
        "[{\"name\":\"a\",\"size\":18446744073709551615},"
        "{\"name\":\"b\",\"size\":1}]";
    dh_file_list list;
    CHECK(!dh_file_list_decode(json, strlen(json), &list),
          "a list whose sizes overflow their total was accepted");
}

static void test_encoding_refuses_a_buffer_it_would_overrun(void) {
    const dh_file_entry out[] = {{.name = "a-fairly-long-name.txt", .name_len = 22, .size = 1}};
    char json[8];
    CHECK(dh_file_list_encode(out, 1, json, sizeof json) < 0,
          "encoding into too small a buffer reported success");
}

/*
 * A name Windows will not open, however legal it is on the Mac it came from.
 * The suffix is what makes it openable; refusing the transfer over a file
 * called `NUL.txt` would be a worse answer than delivering it under a name
 * one file manager shows slightly differently.
 */
static void test_a_reserved_device_name_is_made_openable(void) {
    /* Every family, because the two are the same stem length — three letters
       and a digit for the ports, three letters alone for the rest — and a
       reading that selects between them by length matches only one. */
    static const char *const reserved[] = {"NUL", "con", "PRN.txt", "aux",
                                           "com1.txt", "LPT1", "lpt9.bin", "COM0"};
    for (size_t i = 0; i < sizeof reserved / sizeof reserved[0]; i++) {
        char cleaned[DH_FILE_NAME_MAX + 1];
        const size_t n = dh_file_name_clean(reserved[i], strlen(reserved[i]), cleaned,
                                            sizeof cleaned);
        CHECK(n == strlen(reserved[i]) + 1 && cleaned[0] == '_',
              "a Windows reserved device name was left openable only on the Mac");
    }

    /* And an ordinary name that merely looks like one is left alone. */
    static const char *const ordinary[] = {"common.txt", "com.txt", "lptx", "comm1"};
    for (size_t i = 0; i < sizeof ordinary / sizeof ordinary[0]; i++) {
        char cleaned[DH_FILE_NAME_MAX + 1];
        const size_t n = dh_file_name_clean(ordinary[i], strlen(ordinary[i]), cleaned,
                                            sizeof cleaned);
        CHECK(n == strlen(ordinary[i]) && memcmp(cleaned, ordinary[i], n) == 0,
              "an ordinary name was renamed as though it were a device");
    }
}

/*
 * The prefix has to fit inside the limit the wire states, not merely inside
 * the caller's buffer. A name already at the limit would otherwise leave the
 * cleaner one byte over it and be refused by the decoder at the far end — the
 * whole transfer lost over a rename this end chose.
 */
static void test_a_prefixed_name_still_fits_the_wire_limit(void) {
    char name[DH_FILE_NAME_MAX + 1];
    memcpy(name, "NUL.", 4);
    memset(name + 4, 'a', DH_FILE_NAME_MAX - 4);

    char cleaned[DH_FILE_NAME_MAX + 1];
    const size_t n = dh_file_name_clean(name, DH_FILE_NAME_MAX, cleaned, sizeof cleaned);
    CHECK(n > 0 && n <= DH_FILE_NAME_MAX, "a prefixed name at the limit went over it");
    CHECK(cleaned[0] == '_', "a name at the limit lost its device prefix");
    CHECK(dh_file_name_is_clean(cleaned, n),
          "the cleaner produced a name its own decoder refuses");
}

/*
 * The encoder must never produce metadata the receiving transfer machine
 * refuses. Those are two different numbers — an offer's metadata could be
 * nearly 4 KB on the wire, and DH_XFER_META_MAX is 1 KB because it sizes the
 * board's staging slot — and an offer over the smaller one is cancelled on
 * arrival with nothing said. Before #56 nothing carried metadata at all, so
 * nothing had ever reached the gap between them.
 */
static void test_the_encoder_never_produces_more_than_a_receiver_accepts(void) {
    CHECK(dh_file_list_encode_max() <= DH_XFER_META_MAX,
          "the encoder may produce metadata the transfer machine refuses");

    /* A list right at the edge: as many 30-byte names as will fit. */
    dh_file_entry entries[DH_FILE_LIST_MAX];
    char names[DH_FILE_LIST_MAX][32];
    for (unsigned i = 0; i < DH_FILE_LIST_MAX; i++) {
        snprintf(names[i], sizeof names[i], "a-fairly-long-file-name-%03u.txt", i);
        entries[i].name = names[i];
        entries[i].name_len = (uint16_t)strlen(names[i]);
        entries[i].size = 1;
    }

    char json[8192];
    for (uint16_t count = 1; count <= DH_FILE_LIST_MAX; count++) {
        const int written = dh_file_list_encode(entries, count, json, sizeof json);
        if (written < 0) continue; /* refused, which is the honest answer */
        CHECK((size_t)written <= DH_XFER_META_MAX,
              "an encoded list went past what the transfer machine accepts");
        dh_file_list list;
        CHECK(dh_file_list_decode(json, (size_t)written, &list) && list.count == count,
              "a list inside the limit would not decode");
    }
}

int main(void) {
    test_one_file_round_trips();
    test_several_files_keep_their_order_and_sum();
    test_a_zero_length_file_keeps_its_place();
    test_the_encoder_makes_every_name_safe_to_write();
    test_a_name_that_could_escape_is_refused();
    test_malformed_metadata_is_refused();
    test_an_empty_list_is_refused();
    test_more_files_than_the_list_holds_is_refused();
    test_a_total_that_overflows_is_refused();
    test_encoding_refuses_a_buffer_it_would_overrun();
    test_a_reserved_device_name_is_made_openable();
    test_a_prefixed_name_still_fits_the_wire_limit();
    test_the_encoder_never_produces_more_than_a_receiver_accepts();

    if (failures == 0) printf("file_list: all checks passed\n");
    return failures == 0 ? 0 : 1;
}
