#pragma once
/*
 * The Win32 end of the channel: find the device, seize every channel or none,
 * carry reports in and out.
 *
 * It decides nothing. It reports what it sees to the session and does what the
 * session asks — and the session is itself only a binding onto the shared
 * core's machine (helper_session.h). The macOS twin is ChannelTransport.swift.
 *
 * **One thread.** Discovery is a SetupAPI sweep plus WM_DEVICECHANGE on the
 * helper's message-only window; reads are overlapped and their events are
 * pumped alongside that window's messages. CM_Register_Notification would call
 * back on a pool thread and need marshalling for no gain, and clipboard
 * ownership pins work to the window thread regardless. There is nothing here
 * to lock (#49).
 *
 * Platform-boundary code, verified by hand and by use rather than at a seam
 * (#42, "Not tested at a seam"). What *is* testable — the ladder's decisions —
 * lives in autostart_ladder.h with no Win32 in it.
 */

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "dh_helper.h"

namespace deskhop {

class HidTransport {
  public:
    /* Events for the session, in the vocabulary the core's inputs take. */
    struct Events {
        std::function<void(dh_device_identity)> device_appeared;
        std::function<void()> device_disappeared;
        /* Every channel was opened exclusively. A partial acquisition is not
           this event — it is `acquisition_refused`. */
        std::function<void(uint8_t count)> channels_acquired;
        std::function<void(uint8_t acquired, uint8_t of)> acquisition_refused;
        std::function<void(const uint8_t *data, size_t len)> received;
        std::function<void(const std::string &reason)> transport_failed;
        std::function<void(const std::string &message)> log;
    };

    ~HidTransport();

    /* Register for device notifications on `window` and take the first sweep.
       A helper started a minute after logon finds the device that is already
       there, which is why the sweep exists at all beside the notifications. */
    void start(HWND window, Events events);
    void stop();

    /* A WM_DEVICECHANGE arrived. Any device event re-sweeps: this is also the
       retry that matters when another program holds the channel, because that
       program *releasing* its handle produces no device notification at all. */
    void on_device_change(WPARAM event, LPARAM data);

    /* Seize every channel or none (ADR-0002). ADR-0001 measured that even a
       zero-access second open is refused under dwShareMode = 0, so there is no
       probe worth performing — this opens, and rolls back what it opened if
       any channel refuses. */
    void acquire();
    void release();

    /* One complete frame, packed into fixed-size reports with a padded tail. */
    void send(const uint8_t *frame, size_t len);

    /* The read completions the run loop waits on, alongside the message
       queue. Recomputed on demand: the set changes as channels come and go. */
    std::vector<HANDLE> wait_handles() const;
    /* Take whatever finished. Called after the wait returns, for any reason —
       a completion that arrived while the loop was elsewhere is still there. */
    void pump_reads();

    bool has_device() const { return !channels_.empty(); }
    bool holding_channels() const;

  private:
    struct Channel {
        std::wstring path;
        HANDLE handle{INVALID_HANDLE_VALUE};
        HANDLE read_event{nullptr};
        OVERLAPPED overlapped{};
        std::vector<uint8_t> buffer;
        /* Windows prepends a report-ID byte to every buffer, even on a
           collection that declares no report IDs — so these are one longer
           than the 64 bytes the framing layer owns. Read off HIDP_CAPS rather
           than assumed. */
        USHORT input_report_len{0};
        USHORT output_report_len{0};
        bool opened{false};
        bool read_pending{false};
    };

    struct Found {
        std::wstring path;
        std::wstring serial;
        USHORT input_report_len{0};
        USHORT output_report_len{0};
    };

    /* A fresh sweep, diffed against what is held. Arrival and removal both
       land here, so there is one description of what "the device is present"
       means rather than two that can disagree.

       True when it announced an arrival — which is the session's cue to ask
       for the channels, and so the difference between a device event that has
       already triggered an acquisition and one that has not. */
    bool refresh();
    std::vector<Found> sweep(size_t &config_mode_nodes) const;
    void close(Channel &channel);
    bool start_read(Channel &channel);
    void note(const std::string &message) const;

    Events events_;
    HWND window_{nullptr};
    HDEVNOTIFY notification_{nullptr};
    std::vector<Channel> channels_;
    size_t config_mode_nodes_{0};

    /* The serial of the device this helper is talking to. Every channel must
       belong to it: behaviour with more than one device attached is out of
       scope (#42), and the first serial seen wins rather than a second board
       being silently mixed into one session. */
    std::wstring serial_;
};

} // namespace deskhop
