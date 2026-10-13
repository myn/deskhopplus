/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

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
 * When a clipboard update starts a read (#329). The read runs off the window's
 * thread, so a source app slow to render cannot stop the heartbeat — and only
 * one runs at a time. An update that comes during a read starts nothing and
 * leaves the sequence unhandled; the read's end asks `update` again with the
 * sequence then, so a copy made meanwhile is read once, and an unmoved
 * sequence is not read twice.
 *
 * `external` is `clipboard_update_is_external`. The helper's own write is
 * handled without a read, as before.
 */
class ClipboardReads {
  public:
    explicit ClipboardReads(uint32_t handled = 0) : handled_(handled) {}

    /* True: start a read now. */
    bool update(uint32_t sequence, bool external) {
        if (reading_ || sequence == handled_) return false;
        handled_ = sequence;
        reading_ = external;
        return external;
    }
    void read_done() { reading_ = false; }
    bool reading() const { return reading_; }
    uint32_t handled() const { return handled_; }

  private:
    uint32_t handled_;
    bool reading_{false};
};

/*
 * When a read that waits is logged (#330). The tick asks `warn` while a read
 * runs: true once, the first time it has waited past kWarnAfterMs, and the
 * helper then names the clipboard's owner. `end` says whether a total line
 * follows — only after a warning, so a fast copy logs nothing new. Times are
 * the helper's 32-bit ms clock; the unsigned difference survives its wrap.
 */
class SlowRead {
  public:
    static constexpr uint32_t kWarnAfterMs = 5000;

    void start(uint32_t now) {
        started_ = now;
        running_ = true;
        warned_ = false;
    }
    bool warn(uint32_t now) {
        if (!running_ || warned_ || waited(now) <= kWarnAfterMs) return false;
        warned_ = true;
        return true;
    }
    bool end() {
        running_ = false;
        return warned_;
    }
    uint32_t waited(uint32_t now) const { return now - started_; }

  private:
    uint32_t started_{0};
    bool running_{false};
    bool warned_{false};
};

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
