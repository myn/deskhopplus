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

/* A prefetched remote image may be published only while the clipboard still
   contains what was present when its offer arrived. This catches a local copy
   even when its posted WM_CLIPBOARDUPDATE has not run yet. */
constexpr bool prefetched_image_is_current(uint32_t offered_sequence,
                                           uint32_t current_sequence) {
    return offered_sequence == current_sequence;
}

} // namespace deskhop
