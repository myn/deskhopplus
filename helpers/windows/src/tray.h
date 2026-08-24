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
 * Two rules the core does not carry, because they are presentation:
 *
 *   - **The quiet state shows no icon at all.** A device that disappears for a
 *     moment is ordinary USB noise, and config mode is something the user did
 *     on purpose. Neither is a fault worth putting an icon in the tray for.
 *   - **A balloon fires only for a state that names a remedy.** Everything
 *     else changes the tooltip silently. Ordinary reconnection is not an event
 *     worth interrupting anyone for, and a balloon per reconnect is how a
 *     helper teaches its user to ignore it.
 *
 * The wording is words.h's. What the *states* are, and whether the config
 * chord may be offered from one, are the shared core's and are called rather
 * than restated (#34).
 */

#include <windows.h>

#include <functional>
#include <string>

#include "dh_helper.h"

namespace deskhop {

class Tray {
  public:
    /* The window message the shell sends back for icon activity. */
    static constexpr UINT kCallbackMessage = WM_APP + 1;

    struct Callbacks {
        /* The tray menu is where autostart is turned on and off — an offer,
           never an assumption (#86). */
        std::function<bool()> autostart_enabled;
        /* One line saying what autostart is actually doing, which is not the
           same as whether it is switched on. */
        std::function<std::string()> autostart_detail;
        std::function<void()> toggle_autostart;
        std::function<void()> quit;
    };

    ~Tray();

    void attach(HWND window, Callbacks callbacks);
    void detach();

    /* Adds, updates or removes the icon to match the state. */
    void show(dh_helper_state state);

    /* A kCallbackMessage arrived. */
    void on_callback(LPARAM what);

  private:
    void add_icon();
    void remove_icon();
    void update_tooltip();
    void balloon(const std::string &message);
    void show_menu();

    HWND window_{nullptr};
    Callbacks callbacks_;
    bool icon_shown_{false};
    dh_helper_state state_{DH_HELPER_QUIET};
    /* A balloon fires on *entering* a state, not on every call saying it.
       The core emits a state output only on a change, so this is not guarding
       against it — it guards the paths that re-assert the current state
       without one, which today is Explorer restarting and taking the icon
       with it. Without the latch that would re-balloon every time. */
    bool announced_{false};
};

} // namespace deskhop
