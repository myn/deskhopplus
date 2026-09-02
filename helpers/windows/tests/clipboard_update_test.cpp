#include <cstdio>

#include "clipboard_update.h"

using deskhop::clipboard_update_is_external;

int main() {
    int failures = 0;
#define CHECK(condition, message)                    \
    do {                                             \
        if (!(condition)) {                          \
            ++failures;                              \
            std::printf("FAIL %s\n", (message));    \
        }                                            \
    } while (0)

    CHECK(!clipboard_update_is_external(6320, 6318, 3612416, 3612416),
          "a delayed-format close was mistaken for an external copy");
    CHECK(!clipboard_update_is_external(6318, 6318, 0, 3612416),
          "the exact self sequence was mistaken for an external copy");
    CHECK(clipboard_update_is_external(6321, 6318, 462350, 3612416),
          "a different owner's copy was mistaken for the helper's write");

    if (failures != 0) return 1;
    std::printf("clipboard update tests passed\n");
    return 0;
}
