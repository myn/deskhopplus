/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#pragma once
/*
 * The notification-area icon and its menu (#85) — what the user can see
 * without opening a log.
 *
 * On the helper's own message-only window, and on the one thread everything
 * else runs on. mkroamer's tray gave itself a thread and a mutex because its
 * agent was multi-threaded; here the transport, the session and the tray are
 * all on the window thread, so there is nothing to marshal and nothing to
 * lock.
 *
 * Three rules the core does not carry, because they are presentation:
 *
 *   - **The icon is always there** (#208, and #54's first criterion). It
 *     takes one of three looks — paired, off, attention (`words::look`) — and
 *     the quiet state is the *off* look, not a missing icon: a device that
 *     disappears for a moment is ordinary USB noise, and config mode is
 *     something the user did, so neither look is alarming. While a file
 *     arrives the icon is the percent, drawn as two digits.
 *   - **A balloon fires only for a state that names a remedy.** Everything
 *     else changes the tooltip silently. Ordinary reconnection is not an event
 *     worth interrupting anyone for, and a balloon per reconnect is how a
 *     helper teaches its user to ignore it.
 *   - **The icon asks to sit on the taskbar, not in the overflow.** See
 *     `promote`.
 *
 * The wording and the look are words.h's. What the *states* are, and whether
 * the config chord may be offered from one, are the shared core's and are
 * called rather than restated (#34).
 */

#include <windows.h>

#include <functional>
#include <string>

#include "clip_service.h"
#include "dh_helper.h"
#include "words.h"

namespace deskhop {

class Tray {
  public:
    /* The window message the shell sends back for icon activity. */
    static constexpr UINT kCallbackMessage = WM_APP + 1;
    /* The window timer `promote` retries on; main.cpp's beat timer is 1. */
    static constexpr UINT_PTR kPromoteTimerId = 2;

    struct Callbacks {
        /* The tray menu is where autostart is turned on and off — an offer,
           never an assumption (#86). */
        std::function<bool()> autostart_enabled;
        std::function<void()> toggle_autostart;
        std::function<void()> quit;

        /* The paste-side acceptance and abort (#56). Nothing has crossed the
           link when `accept_files` is offered, and nothing will until it or
           `decline_files` is called.

           Last in the struct, and deliberately: this is initialised
           positionally at its one call site, so a field added in the middle
           would silently shift every callback after it onto the wrong slot. */
        std::function<void(uint32_t)> accept_files;
        std::function<void(uint32_t)> decline_files;
        std::function<void()> abort_transfer;
        /* Whether something is on its way *out* of this computer, and how to
           stop it (#42, story 7 — a mis-copied folder must not hold anyone
           hostage). Asked rather than pushed: the menu is built when it is
           opened, so it can simply look. */
        std::function<bool()> is_sending;
        std::function<void()> abort_send;
        /* Diagnostics, never shown to the user. */
        std::function<void(const std::string &)> log;
    };

    ~Tray();

    /* Attaching shows the icon; detaching removes it. */
    void attach(HWND window, Callbacks callbacks);
    void detach();

    /* Updates the icon's look and words to match the state. */
    void show(dh_helper_state state);

    /* Whether something is on its way out. The icon does not change for it;
       the tooltip says so. Pushed from the tick, since nothing else here is
       told when a send starts or ends. */
    void show_sending(bool sending);

    /*
     * Files are being offered from the other computer (#56).
     *
     * A balloon *and* two menu entries. The balloon is what makes it a prompt
     * rather than something to discover — a transfer that can take minutes
     * must be agreed to, not stumbled upon — and the menu is what makes a
     * missed balloon recoverable. Neither takes focus from what the user is
     * typing into, which a modal dialog would.
     */
    void ask_about_files(const deskhop::FileOffer &offer);
    void withdraw_file_question(uint32_t id);

    /* How far the arriving transfer has got. `total` of zero means nothing is
       arriving. */
    void show_progress(uint64_t received, uint64_t total);

    /* What the user is being asked to agree to: how many files, how big, and
       how long it will take. The duration is the point — a size alone does not
       tell anyone whether to wait (#39, #56). Static so a test can read it
       without a window. */
    static std::string summary(const deskhop::FileOffer &offer);
    static std::string duration_text(uint32_t seconds);

    /* A kCallbackMessage arrived. */
    void on_callback(LPARAM what);

    /* A WM_TIMER for kPromoteTimerId arrived. */
    void on_timer();

    /* Something the user did produced nothing, and only they can act on why —
       which is the bar this file sets for interrupting anyone. Public because
       the copy side, not just a state change, now has such a thing to say. */
    void balloon(const std::string &message);

  private:
    void add_icon();
    void remove_icon();
    /* The icon and the tooltip, from the state, the question, the transfer
       and the send — every path that changes one of those ends here. */
    void update();
    HICON icon_for(words::Look look);
    HICON digits(unsigned percent);
    void promote();
    void show_menu();
    void refresh_open_menu();

    HWND window_{nullptr};
    Callbacks callbacks_;
    bool icon_shown_{false};
    dh_helper_state state_{DH_HELPER_QUIET};
    bool sending_{false};
    /* The three looks, loaded from the exe's resources on first use, in
       `words::Look` order. */
    HICON looks_[3]{};
    /* The digit icon of the moment, destroyed when the next one replaces it. */
    HICON digits_{nullptr};
    /* Whether the taskbar has been asked to keep the icon out of the
       overflow, and how many times it has been asked. */
    bool promoted_{false};
    int promote_attempts_{0};
    static constexpr int kPromoteAttempts = 5;
    static constexpr UINT kPromoteRetryMs = 2000;
    /* The offer waiting on this computer's user, and the transfer running now.
       Both are what the menu grows extra entries for. */
    bool have_question_{false};
    deskhop::FileOffer question_;
    uint64_t progress_received_{0};
    uint64_t progress_total_{0};
    /* The menu while TrackPopupMenu has it open, so its progress row can be
       rewritten under the user (#262); null otherwise. */
    HMENU open_menu_{nullptr};
    /* The percent and total the open menu's progress row shows (-1 once it
       reads "No longer receiving"), so a repaint happens only when they move.
       Meaningless while the open menu has no progress row. */
    int shown_percent_{-1};
    uint64_t shown_total_{0};
    /* A balloon fires on *entering* a state, not on every call saying it.
       The core emits a state output only on a change, so this is not guarding
       against it — it guards the paths that re-assert the current state
       without one, which today is Explorer restarting and taking the icon
       with it. Without the latch that would re-balloon every time. */
    bool announced_{false};
};

} // namespace deskhop
