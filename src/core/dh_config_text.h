#ifndef DH_CONFIG_TEXT_H
#define DH_CONFIG_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DH_CONFIG_TEXT_OVERRIDE_CAPACITY 32u
#define DH_CONFIG_TEXT_PASSTHROUGH_CAPACITY 16u
#define DH_CONFIG_TEXT_CHORD_CAPACITY 6u
#define DH_CONFIG_TEXT_ERROR_TOKEN_CAPACITY 32u

typedef struct {
    uint8_t from;
    uint8_t to;
} dh_key_override_t;

typedef struct {
    uint8_t keys[DH_CONFIG_TEXT_CHORD_CAPACITY];
    size_t key_count;
} dh_key_chord_t;

typedef enum {
    DH_CONFIG_TEXT_OK = 0,
    DH_CONFIG_TEXT_EXPECTED_KEY,
    DH_CONFIG_TEXT_EXPECTED_EQUALS,
    DH_CONFIG_TEXT_UNKNOWN_KEY,
    DH_CONFIG_TEXT_CAPACITY
} dh_config_text_error_code_t;

typedef struct {
    dh_config_text_error_code_t code;
    size_t line;
    size_t column;
    char token[DH_CONFIG_TEXT_ERROR_TOKEN_CAPACITY];
} dh_config_text_error_t;

/* Key names are ASCII and case-insensitive on input. Returned names are
 * canonical lower-case static strings, or NULL when the usage is unnamed. */
bool dh_key_name_parse(const char *name, size_t length, uint8_t *usage);
const char *dh_key_name(uint8_t usage);

/* These parsers are transactional: on failure, output and count are unchanged. */
bool dh_config_text_parse_overrides(const char *text, dh_key_override_t *output,
                                    size_t capacity, size_t *count,
                                    dh_config_text_error_t *error);
bool dh_config_text_parse_keys(const char *text, uint8_t *output, size_t capacity,
                               size_t *count, dh_config_text_error_t *error);
bool dh_config_text_parse_chord(const char *text, dh_key_chord_t *output,
                                dh_config_text_error_t *error);

#endif
