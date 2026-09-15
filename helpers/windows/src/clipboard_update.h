#pragma once

#include <cstddef>
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


/*
 * What one copy sends when it holds text, a picture, or both (#195).
 *
 * A size of zero is "not there" — which is also what a read that came back
 * empty is, so a picture the clipboard offered and then would not give up
 * still lets the text beside it travel.
 *
 * Both present and small enough together: a bundle, so the pasting
 * application picks. Both present and over the limit: the text alone, and the
 * picture does not travel — text is never made late by a rendering beside it
 * (ADR-0013). The macOS twin is `ClipboardSend.select` in ClipboardService.swift,
 * and a divergence is a clipboard that behaves differently on each computer.
 */
enum class ClipboardSend { Nothing, Text, Image, Bundle };

constexpr ClipboardSend select_clipboard_send(size_t text_bytes, size_t image_bytes,
                                              size_t bundle_limit) {
    if (text_bytes > 0 && image_bytes > 0)
        return text_bytes + image_bytes <= bundle_limit ? ClipboardSend::Bundle
                                                        : ClipboardSend::Text;
    if (image_bytes > 0) return ClipboardSend::Image;
    if (text_bytes > 0) return ClipboardSend::Text;
    return ClipboardSend::Nothing;
}

} // namespace deskhop
