#pragma once
/*
 * This computer's clipboard: what was copied here, and what arrives from the
 * other computer (#52, #55, #56).
 *
 * Text, images and files are carried. A file copy is read as a *list* and not
 * as bytes — the contents are not touched until the other computer's user
 * accepts the transfer, which is the whole of #56 and the reason `local_files`
 * hands over a callable rather than a payload.
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
 * WINDOWS DELAYED RENDERING FOR LAZY IMAGES
 *
 * A large image is claimed with a null handle and requested from the copy side
 * only when WM_RENDERFORMAT says an application pasted it. The handler pumps
 * the channel and window messages while it waits, bounded by the measured
 * 49 KB/s route plus margin, so the helper remains live during the synchronous
 * Win32 callback. Text and small images remain eager and always publish real
 * handles immediately.
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
#include <optional>
#include <string>
#include <vector>

#include "clip_service.h"

namespace deskhop {

class Clipboard {
  public:
    struct Callbacks {
        /* Text copied on this computer, already filtered for this helper's own
           writes. UTF-8. */
        std::function<void(std::vector<uint8_t>)> local_copy;
        std::function<void(std::vector<uint8_t>)> local_image;
        /* Any external sequence, before inspecting its formats or send policy. */
        std::function<void()> local_replaced;
        std::function<std::optional<std::vector<uint8_t>>(uint32_t, uint64_t)> request_image;
        std::function<void(uint32_t)> lazy_image_replaced;
        /* Files copied here (#56): what they are called and how long they are,
           plus the callable that reads them — invoked only if the transfer is
           accepted on the other computer. */
        std::function<void(std::vector<FileEntry>,
                           std::function<bool(std::vector<uint8_t> &)>)> local_files;
        std::function<void(const std::string &)> log;
    };

    /* The helper's message-only window. Registers for WM_CLIPBOARDUPDATE. */
    void attach(HWND window, Callbacks callbacks);
    void detach();

    /* Called from the window procedure. True when the message was this
       object's and needs no further handling. */
    bool handle(UINT message, WPARAM parameter = 0);

    /* Write what arrived from the other computer. The bytes are handed to the
       platform exactly as they arrived: ADR-0003 makes this channel
       fidelity-preserving, so the only transform is CF_UNICODETEXT's own
       encoding conversion at this edge. Malformed input converts best-effort
       with the OS default and is never rejected. */
    void deliver_text(const std::vector<uint8_t> &utf8);
    /* expected_sequence is used by a background prefetch: after opening the
       clipboard, refuse to overwrite a local copy made while bytes crossed. */
    bool deliver_image(const std::vector<uint8_t> &png,
                       std::optional<DWORD> expected_sequence = std::nullopt);
    void lazy_image(uint32_t id, uint64_t total);
    void cancel_lazy_image(uint32_t id);

    /* Put references to files that arrived on the clipboard as CF_HDROP. The
       files themselves are already on disk (FileStore); this is only what the
       user pastes. */
    bool deliver_files(const std::vector<std::wstring> &paths);

  private:
    void read_clipboard();
    /* The files on the clipboard, if any, as a list and a way to read them
       later. Called with the clipboard already open, and it does not invoke the
       callback — the caller closes the clipboard first, because the clipboard
       is a machine-wide lock and that callback runs the whole offer path. */
    bool read_files(std::vector<FileEntry> &entries,
                    std::function<bool(std::vector<uint8_t> &)> &read);
    bool open_with_retry();
    bool load_lazy_image();

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
    ULONG_PTR gdiplus_token_{0};
    UINT png_format_{0};
    uint32_t lazy_image_id_{0};
    uint64_t lazy_image_total_{0};
    std::vector<uint8_t> lazy_image_png_;
    /* Temporary #55 diagnostics remain armed just long enough to observe the
       external copy that replaces a received lazy image. */
    bool trace_lazy_lifecycle_{false};
};

} // namespace deskhop
