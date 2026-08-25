#pragma once
/*
 * This computer's clipboard: what was copied here, and what arrives from the
 * other computer (#52).
 *
 * Only text in this slice — images are #55, files are #56.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS LIVES ON THE HELPER'S OWN WINDOW, AND NOT A THREAD OF ITS OWN
 *
 * Setting clipboard data requires being the clipboard owner, which requires a
 * window handle — so a console-only helper is ruled out (spec #42). The helper
 * already has a message-only window for WM_DEVICECHANGE, and it already runs
 * everything on the thread that owns it. Attaching here rather than starting a
 * second thread keeps that true: the shared core assumes one caller, and there
 * is nothing to lock.
 *
 * A message-only window does not receive *broadcast* messages, but
 * WM_CLIPBOARDUPDATE is sent to listeners registered with
 * AddClipboardFormatListener rather than broadcast, so it arrives.
 *
 * ---------------------------------------------------------------------------
 * DELAYED RENDERING, AND WHY THIS SLICE DOES NOT USE IT
 *
 * Delayed rendering is the natural fit for *lazy* transfers: the helper claims
 * a format with a null handle and serves WM_RENDERFORMAT when something
 * actually pastes. That message must be serviced **synchronously**, so any
 * channel I/O behind it would hang the application that is pasting for as long
 * as the round trip takes (spec #42).
 *
 * Text is eager. The bytes are already in hand before anything is written, so
 * this writes them directly and there is no round trip to be slow — the hazard
 * is absent rather than mitigated. Nothing here claims a format it cannot
 * immediately produce, which is the property the warning is really about, and
 * it is checked by construction: there is no SetClipboardData(fmt, nullptr) in
 * this file. The lazy path arrives with files (#56) and is where a render
 * handler belongs.
 *
 * ---------------------------------------------------------------------------
 * OPENING THE CLIPBOARD CAN FAIL
 *
 * OpenClipboard fails outright when another window holds it, with no built-in
 * wait, so every access here is a bounded retry with a widening delay. A single
 * failed attempt would drop a payload that has already crossed the link,
 * silently.
 */

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace deskhop {

class Clipboard {
  public:
    struct Callbacks {
        /* Text copied on this computer, already filtered for this helper's own
           writes. UTF-8. */
        std::function<void(std::vector<uint8_t>)> local_copy;
        std::function<void(const std::string &)> log;
    };

    /* The helper's message-only window. Registers for WM_CLIPBOARDUPDATE. */
    void attach(HWND window, Callbacks callbacks);
    void detach();

    /* Called from the window procedure. True when the message was this
       object's and needs no further handling. */
    bool handle(UINT message);

    /* Write what arrived from the other computer. The bytes are handed to the
       platform exactly as they arrived: ADR-0003 makes this channel
       fidelity-preserving, so the only transform is CF_UNICODETEXT's own
       encoding conversion at this edge. Malformed input converts best-effort
       with the OS default and is never rejected. */
    void deliver_text(const std::vector<uint8_t> &utf8);

  private:
    void read_clipboard();
    bool open_with_retry();

    HWND window_{nullptr};
    bool listening_{false};
    Callbacks callbacks_;
    /* The sequence number our own write produced. Without it, writing what
       arrived from the other computer looks exactly like a fresh local copy,
       and the two helpers hand the same payload back and forth for ever. */
    DWORD self_sequence_{0};
    /* The sequence number the last update was acted on at. WM_CLIPBOARDUPDATE
       is posted, so by the time it is handled the number is whatever the
       clipboard holds *now* — two messages queued behind one change would
       otherwise both read the same clipboard and send the same payload twice. */
    DWORD handled_sequence_{0};
};

} // namespace deskhop
