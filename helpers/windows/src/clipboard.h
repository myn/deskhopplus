/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

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
 * WHY THIS LIVES ON THE HELPER'S OWN WINDOW, AND READS ON A THREAD OF ITS OWN
 *
 * Setting clipboard data requires being the clipboard owner, which requires a
 * window handle — so a console-only helper is ruled out (spec #42). The helper
 * already has a message-only window for WM_DEVICECHANGE, and it already runs
 * everything on the thread that owns it. Writes, ownership and the act on a
 * copy stay there: the shared core assumes one caller.
 *
 * The read of a local copy does not (#329). GetClipboardData waits for as long
 * as the source app takes to render a delayed format, and on this thread that
 * wait stopped the heartbeat until the board dropped the helper (#327). So
 * each read runs on a short-lived thread with the clipboard opened by no
 * window, on plain copies of what it needs, and posts its `ClipboardCopy`
 * back as kReadDoneMessage; `act_on_copy` then runs here as before. While a
 * read waits, a write of what arrived is held, the latest only, and runs when
 * the read ends (`ClipboardReads::write`) — it would find the clipboard open
 * and lose its payload otherwise.
 *
 * A message-only window does not receive *broadcast* messages, but
 * WM_CLIPBOARDUPDATE is sent to listeners registered with
 * AddClipboardFormatListener rather than broadcast, so it arrives.
 *
 * ---------------------------------------------------------------------------
 * NO DELAYED RENDERING
 *
 * Every write puts real data on the clipboard. A large image is prefetched
 * first, in main.cpp, and written only when it has all arrived (ADR-0010):
 * Windows asks for a delayed format within milliseconds, and the wait for the
 * bytes then locks the clipboard for the whole transfer.
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

#include "clipboard_copy.h"

namespace deskhop {

class Clipboard {
  public:
    using Callbacks = ClipboardCallbacks;

    /* The helper's message-only window. Registers for WM_CLIPBOARDUPDATE. */
    void attach(HWND window, Callbacks callbacks);
    void detach();

    /* A read's thread posts this to the window when it ends, with its
       heap-allocated ClipboardCopy in LPARAM (#329). */
    static constexpr UINT kReadDoneMessage = WM_APP + 2;

    /* Called from the window procedure. True when the message was this
       object's and needs no further handling. */
    bool handle(UINT message, LPARAM detail);

    /* From the helper's tick: a read that has waited past 5 s names the
       program that owns the clipboard, once (#330). */
    void tick(uint32_t now);

    /* Write what arrived from the other computer. The bytes are handed to the
       platform exactly as they arrived: ADR-0003 makes this channel
       fidelity-preserving, so the only transform is CF_UNICODETEXT's own
       encoding conversion at this edge. Malformed input converts best-effort
       with the OS default and is never rejected. Every write here is held
       while a read has the clipboard, and runs when it ends (#329). */
    void deliver_text(const std::vector<uint8_t> &utf8);
    /* expected_sequence is used by a background prefetch: after opening the
       clipboard, refuse to overwrite a local copy made while bytes crossed. */
    void deliver_image(const std::vector<uint8_t> &png,
                       std::optional<DWORD> expected_sequence = std::nullopt);
    /* Both parts of a bundle (#195) in one clipboard write: CF_UNICODETEXT
       beside the PNG / CF_DIBV5 / CF_DIB set `deliver_image` publishes. Two
       writes would bump the sequence twice and the second would clear the
       first. Neither part is empty. */
    void deliver_bundle(const std::vector<uint8_t> &utf8, const std::vector<uint8_t> &png);

    /* Put references to files that arrived on the clipboard as CF_HDROP. The
       files themselves are already on disk (FileStore); this is only what the
       user pastes. */
    void deliver_files(const std::vector<std::wstring> &paths);

  private:
    /* A clipboard update, or a read's end: start a read if one is owed. */
    void on_update();
    void start_read();
    /* The writes themselves, behind the deliver_ calls above. */
    void write_text(const std::vector<uint8_t> &utf8);
    bool write_image(const std::vector<uint8_t> &png, std::optional<DWORD> expected_sequence);
    void write_bundle(const std::vector<uint8_t> &utf8, const std::vector<uint8_t> &png);
    bool write_files(const std::vector<std::wstring> &paths);
    bool open_with_retry();

    HWND window_{nullptr};
    bool listening_{false};
    Callbacks callbacks_;
    /* The sequence number our own write produced. Without it, writing what
       arrived from the other computer looks exactly like a fresh local copy,
       and the two helpers hand the same payload back and forth for ever. */
    DWORD self_sequence_{0};
    /* The sequence number the last update was acted on at, and whether a read
       is in flight. WM_CLIPBOARDUPDATE is posted, so by the time it is handled
       the number is whatever the clipboard holds *now* — two messages queued
       behind one change would otherwise both read the same clipboard and send
       the same payload twice. */
    ClipboardReads reads_;
    /* How long the read in flight has waited (#330). */
    SlowRead slow_read_;
    ULONG_PTR gdiplus_token_{0};
    UINT png_format_{0};
};

} // namespace deskhop
