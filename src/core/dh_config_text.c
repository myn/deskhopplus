#include "dh_config_text.h"

#include <string.h>

typedef struct {
    const char *name;
    uint8_t usage;
} key_name_t;

static const key_name_t named_keys[] = {
#define DH_KEY_NAME(name, usage) {name, usage},
#include "dh_key_names.def"
#undef DH_KEY_NAME
};

static bool ascii_equal(const char *input, size_t length, const char *known) {
    if (strlen(known) != length)
        return false;
    for (size_t i = 0; i < length; ++i) {
        char c = input[i];
        if (c >= 'A' && c <= 'Z')
            c = (char)(c + ('a' - 'A'));
        if (c != known[i])
            return false;
    }
    return true;
}

bool dh_key_name_parse(const char *name, size_t length, uint8_t *usage) {
    uint8_t parsed = 0;
    if (!name || !usage || length == 0)
        return false;

    char c = name[0];
    if (c >= 'A' && c <= 'Z')
        c = (char)(c + ('a' - 'A'));
    if (length == 1) {
        if (c >= 'a' && c <= 'z')
            parsed = (uint8_t)(0x04 + c - 'a');
        else if (c >= '1' && c <= '9')
            parsed = (uint8_t)(0x1e + c - '1');
        else if (c == '0')
            parsed = 0x27;
    } else if (c == 'f' && length >= 2 && length <= 3) {
        unsigned number = 0;
        for (size_t i = 1; i < length; ++i) {
            if (name[i] < '0' || name[i] > '9')
                return false;
            number = number * 10u + (unsigned)(name[i] - '0');
        }
        if (number >= 1 && number <= 12)
            parsed = (uint8_t)(0x3a + number - 1);
    }

    if (!parsed) {
        for (size_t i = 0; i < sizeof(named_keys) / sizeof(named_keys[0]); ++i) {
            if (ascii_equal(name, length, named_keys[i].name)) {
                parsed = named_keys[i].usage;
                break;
            }
        }
    }
    if (!parsed)
        return false;
    *usage = parsed;
    return true;
}

const char *dh_key_name(uint8_t usage) {
    static const char letters[][2] = {
        "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m",
        "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z"};
    static const char digits[][2] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
    static const char functions[][4] = {"f1", "f2", "f3", "f4", "f5", "f6",
                                        "f7", "f8", "f9", "f10", "f11", "f12"};
    if (usage >= 0x04 && usage <= 0x1d)
        return letters[usage - 0x04];
    if (usage >= 0x1e && usage <= 0x26)
        return digits[usage - 0x1e];
    if (usage == 0x27)
        return "0";
    if (usage >= 0x3a && usage <= 0x45)
        return functions[usage - 0x3a];
    for (size_t i = 0; i < sizeof(named_keys) / sizeof(named_keys[0]); ++i)
        if (named_keys[i].usage == usage)
            return named_keys[i].name;
    return NULL;
}

static bool space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

static void clear_error(dh_config_text_error_t *error) {
    if (error)
        memset(error, 0, sizeof(*error));
}

static bool fail(dh_config_text_error_t *error, dh_config_text_error_code_t code,
                 size_t line, size_t column, const char *token, size_t length) {
    if (error) {
        clear_error(error);
        error->code = code;
        error->line = line;
        error->column = column;
        if (token) {
            if (length >= sizeof(error->token))
                length = sizeof(error->token) - 1;
            memcpy(error->token, token, length);
            error->token[length] = '\0';
        }
    }
    return false;
}

static bool token(const char **cursor, const char *end, const char **start, size_t *length) {
    while (*cursor < end && space(**cursor))
        ++*cursor;
    *start = *cursor;
    while (*cursor < end && !space(**cursor) && **cursor != '=' && **cursor != ',' &&
           **cursor != '+' && **cursor != '\n')
        ++*cursor;
    *length = (size_t)(*cursor - *start);
    while (*cursor < end && space(**cursor))
        ++*cursor;
    return *length != 0;
}

bool dh_config_text_parse_overrides(const char *text, dh_key_override_t *output,
                                    size_t capacity, size_t *count,
                                    dh_config_text_error_t *error) {
    dh_key_override_t parsed[DH_CONFIG_TEXT_OVERRIDE_CAPACITY];
    size_t parsed_count = 0, line = 1;
    const char *cursor = text;
    if (!text || !output || !count || capacity > DH_CONFIG_TEXT_OVERRIDE_CAPACITY)
        return fail(error, DH_CONFIG_TEXT_CAPACITY, 1, 1, NULL, 0);
    clear_error(error);

    while (*cursor) {
        while (*cursor == '\n' || space(*cursor)) {
            if (*cursor++ == '\n')
                ++line;
        }
        if (!*cursor)
            break;
        const char *from_name, *to_name;
        size_t from_length, to_length;
        const char *line_start = cursor;
        if (!token(&cursor, cursor + strlen(cursor), &from_name, &from_length))
            return fail(error, DH_CONFIG_TEXT_EXPECTED_KEY, line, 1, NULL, 0);
        if (*cursor != '=')
            return fail(error, DH_CONFIG_TEXT_EXPECTED_EQUALS, line,
                        (size_t)(cursor - line_start + 1), from_name, from_length);
        ++cursor;
        if (!token(&cursor, cursor + strlen(cursor), &to_name, &to_length))
            return fail(error, DH_CONFIG_TEXT_EXPECTED_KEY, line,
                        (size_t)(cursor - line_start + 1), NULL, 0);
        if (*cursor && *cursor != '\n')
            return fail(error, DH_CONFIG_TEXT_EXPECTED_EQUALS, line,
                        (size_t)(cursor - line_start + 1), cursor, 1);
        if (parsed_count >= capacity)
            return fail(error, DH_CONFIG_TEXT_CAPACITY, line, 1, from_name, from_length);
        if (!dh_key_name_parse(from_name, from_length, &parsed[parsed_count].from))
            return fail(error, DH_CONFIG_TEXT_UNKNOWN_KEY, line,
                        (size_t)(from_name - line_start + 1), from_name, from_length);
        if (!dh_key_name_parse(to_name, to_length, &parsed[parsed_count].to))
            return fail(error, DH_CONFIG_TEXT_UNKNOWN_KEY, line,
                        (size_t)(to_name - line_start + 1), to_name, to_length);
        ++parsed_count;
    }
    memcpy(output, parsed, parsed_count * sizeof(parsed[0]));
    *count = parsed_count;
    return true;
}

bool dh_config_text_parse_keys(const char *text, uint8_t *output, size_t capacity,
                               size_t *count, dh_config_text_error_t *error) {
    uint8_t parsed[DH_CONFIG_TEXT_PASSTHROUGH_CAPACITY];
    size_t parsed_count = 0, line = 1;
    const char *cursor = text;
    if (!text || !output || !count || capacity > DH_CONFIG_TEXT_PASSTHROUGH_CAPACITY)
        return fail(error, DH_CONFIG_TEXT_CAPACITY, 1, 1, NULL, 0);
    clear_error(error);
    const char *end = text + strlen(text);
    while (cursor < end) {
        while (cursor < end && (space(*cursor) || *cursor == '\n')) {
            if (*cursor++ == '\n')
                ++line;
        }
        if (cursor == end)
            break;
        if (*cursor == ',')
            return fail(error, DH_CONFIG_TEXT_EXPECTED_KEY, line, 1, NULL, 0);
        const char *name;
        size_t length;
        const char *line_start = cursor;
        if (!token(&cursor, end, &name, &length))
            return fail(error, DH_CONFIG_TEXT_EXPECTED_KEY, line, 1, NULL, 0);
        if (parsed_count >= capacity)
            return fail(error, DH_CONFIG_TEXT_CAPACITY, line, 1, name, length);
        if (!dh_key_name_parse(name, length, &parsed[parsed_count]))
            return fail(error, DH_CONFIG_TEXT_UNKNOWN_KEY, line,
                        (size_t)(name - line_start + 1), name, length);
        ++parsed_count;
        if (cursor < end && *cursor != ',' && *cursor != '\n')
            return fail(error, DH_CONFIG_TEXT_EXPECTED_KEY, line,
                        (size_t)(cursor - line_start + 1), cursor, 1);
        if (cursor < end && *cursor == ',') {
            ++cursor;
            const char *lookahead = cursor;
            while (lookahead < end && space(*lookahead))
                ++lookahead;
            if (lookahead == end || *lookahead == ',' || *lookahead == '\n')
                return fail(error, DH_CONFIG_TEXT_EXPECTED_KEY, line,
                            (size_t)(lookahead - line_start + 1), NULL, 0);
        }
    }
    memcpy(output, parsed, parsed_count);
    *count = parsed_count;
    return true;
}

bool dh_config_text_parse_chord(const char *text, dh_key_chord_t *output,
                                dh_config_text_error_t *error) {
    dh_key_chord_t parsed = {0};
    const char *cursor = text;
    if (!text || !output)
        return fail(error, DH_CONFIG_TEXT_EXPECTED_KEY, 1, 1, NULL, 0);
    clear_error(error);
    const char *end = text + strlen(text);
    while (cursor < end) {
        const char *name;
        size_t length;
        if (!token(&cursor, end, &name, &length))
            return fail(error, DH_CONFIG_TEXT_EXPECTED_KEY, 1,
                        (size_t)(cursor - text + 1), NULL, 0);
        if (parsed.key_count >= DH_CONFIG_TEXT_CHORD_CAPACITY)
            return fail(error, DH_CONFIG_TEXT_CAPACITY, 1,
                        (size_t)(name - text + 1), name, length);
        if (!dh_key_name_parse(name, length, &parsed.keys[parsed.key_count]))
            return fail(error, DH_CONFIG_TEXT_UNKNOWN_KEY, 1,
                        (size_t)(name - text + 1), name, length);
        ++parsed.key_count;
        if (cursor == end)
            break;
        if (*cursor != '+')
            return fail(error, DH_CONFIG_TEXT_EXPECTED_KEY, 1,
                        (size_t)(cursor - text + 1), cursor, 1);
        ++cursor;
        if (cursor == end || *cursor == '+')
            return fail(error, DH_CONFIG_TEXT_EXPECTED_KEY, 1,
                        (size_t)(cursor - text + 1), NULL, 0);
    }
    if (parsed.key_count == 0)
        return fail(error, DH_CONFIG_TEXT_EXPECTED_KEY, 1, 1, NULL, 0);
    *output = parsed;
    return true;
}
