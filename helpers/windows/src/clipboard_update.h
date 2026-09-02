#pragma once

#include <cstdint>

namespace deskhop {

/* Delayed clipboard formats may advance the sequence when CloseClipboard
   finalises them. Ownership is therefore the authoritative second signal for
   deciding whether a queued update describes this helper's own write. */
constexpr bool clipboard_update_is_external(uint32_t sequence, uint32_t self_sequence,
                                            uintptr_t owner, uintptr_t helper) {
    return sequence != self_sequence && owner != helper;
}

} // namespace deskhop
