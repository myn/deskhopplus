/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#include "hid_transport.h"

#include <initguid.h> /* must precede hidclass.h so GUID_DEVINTERFACE_HID is defined here */

#include <hidclass.h>

/*
 * hidpi.h declares every HidP_ function as returning NTSTATUS, and nothing on
 * the user-mode include path defines that type — windows.h does not, and the
 * header that would is a driver header. Without this, MSVC reads a hundred
 * prototypes as implicit-int variable declarations and reports the last of
 * them as a redefinition of the first.
 *
 * An identical redeclaration is legal in both C and C++, so this stays correct
 * on a toolchain whose headers do define it (mingw's, via ntdef.h).
 */
typedef LONG NTSTATUS;

/* Order matters and is not alphabetical: hidpi.h declares the preparsed-data
   and usage types hidsdi.h then uses in its prototypes. */
extern "C" {
#include <hidusage.h>
#include <hidpi.h>
#include <hidsdi.h>
}

#include <dbt.h>
#include <setupapi.h>

#include <algorithm>
#include <cstring>

#include "channel_identity.h"
#include "dh_frame.h"

namespace deskhop {

namespace {

/* A bounded wait on a write. Blocking for ever on a device that has stopped
   draining would freeze the one thread the tray also lives on; a write this
   slow is a dropped connection either way. */
constexpr DWORD kWriteTimeoutMs = 250;

/*
 * How many input reports hidclass.sys buffers for this handle.
 *
 * The default is 32, and 32 is what #143 desynchronised on. The ring is
 * overwritten rather than stalled when it fills, so a helper that falls behind
 * loses reports with nothing said anywhere. The reader now bridges a lost
 * report on the frame-start flag (ADR-0012), so it costs a frame rather than
 * the session — but a frame lost here is still a frame lost.
 *
 * The board emits one 64-byte report per millisecond, so 32 buffers is 32ms of
 * slack on a single-threaded loop that also pumps window messages, owns the
 * clipboard and seals a 1 KiB chunk per frame. 512 is the maximum hidclass
 * accepts and buys about half a second, which is longer than anything on this
 * thread blocks for.
 *
 * Not the whole of the fix — pump_reads draining the ring rather than one
 * report per pass is the other half — and not a guarantee either: this is a
 * deeper bucket, not back-pressure. DH_NOTE_BOARD_SENDS is what says
 * whether it was deep enough.
 */
constexpr ULONG kInputBuffers = 512;

std::string narrow(const std::wstring &text) {
    if (text.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr,
                                           nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), needed, nullptr, nullptr);
    return out;
}

std::string error_text(const char *what, DWORD error) {
    return std::string(what) + ": win32 error " + std::to_string(error);
}

std::string last_error(const char *what) { return error_text(what, GetLastError()); }

/*
 * Whether an open failed because the device is not there, rather than because
 * something else has it.
 *
 * The distinction is the whole of #125. Contention is ERROR_SHARING_VIOLATION
 * or ERROR_ACCESS_DENIED; everything below means the path itself has gone,
 * which is the ordinary end of a config-mode round trip or an unplug. Reading
 * the second as the first made "every channel refused" — since #114 the only
 * remaining way to detect a channel someone else holds — fire on the happy
 * path, and sent a reader hunting for a process that was never there.
 */
bool device_is_gone(DWORD error) {
    return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
           error == ERROR_NO_SUCH_DEVICE || error == ERROR_DEVICE_NOT_CONNECTED ||
           error == ERROR_NOT_FOUND;
}

} // namespace

HidTransport::~HidTransport() { stop(); }

void HidTransport::note(const std::string &message) const {
    if (events_.log) events_.log(message);
}

void HidTransport::start(HWND window, Events events) {
    events_ = std::move(events);
    window_ = window;

    DEV_BROADCAST_DEVICEINTERFACE_W filter{};
    filter.dbcc_size = sizeof(filter);
    filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    filter.dbcc_classguid = GUID_DEVINTERFACE_HID;
    notification_ = RegisterDeviceNotificationW(window_, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!notification_) note(last_error("could not register for device notifications"));

    /* The sweep is not an optimisation. A helper started tens of seconds after
       logon never sees an arrival for a device that was already plugged in,
       and would otherwise wait for one for ever (#83). */
    refresh();
}

void HidTransport::stop() {
    release();
    for (Channel &channel : channels_) close(channel);
    channels_.clear();
    if (notification_) {
        UnregisterDeviceNotification(notification_);
        notification_ = nullptr;
    }
}

void HidTransport::on_device_change(WPARAM event, LPARAM) {
    if (event != DBT_DEVICEARRIVAL && event != DBT_DEVICEREMOVECOMPLETE) return;
    rescan();
}

void HidTransport::rescan() {
    /*
     * Every device event is also a retry of a refused acquisition, which is
     * why this is not just the sweep.
     *
     * When the sweep announces an arrival the session asks for the channels
     * itself, and doing it again here would charge the backoff twice for one
     * event. When it does not — our device was already here and something
     * else on the bus moved — nothing else would try, and the channel we were
     * refused might be free now. The program that held it does not announce
     * letting go, so a helper that only ever retried on *its own* device's
     * events would be waiting for a notification that is never sent.
     *
     * The run loop's idle rescan reaches the same two lines for the same
     * reason: a sweep whose answer has gone stale is indistinguishable from
     * one that was never asked for.
     */
    const bool announced = refresh();
    if (!announced && has_device() && !holding_channels()) acquire();
}

std::vector<HidTransport::Found> HidTransport::sweep(size_t &config_api_nodes) const {
    std::vector<Found> found;
    config_api_nodes = 0;

    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_HID, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) {
        note(last_error("could not enumerate HID interfaces"));
        return found;
    }

    SP_DEVICE_INTERFACE_DATA interface_data{};
    interface_data.cbSize = sizeof(interface_data);
    for (DWORD index = 0;
         SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_HID, index, &interface_data);
         ++index) {

        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &interface_data, nullptr, 0, &needed, nullptr);
        if (needed == 0) continue;

        std::vector<uint8_t> storage(needed);
        auto *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(storage.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &interface_data, detail, needed, nullptr,
                                              nullptr))
            continue;

        /* A held channel already has an exclusive handle. Reuse it: a fresh
           probe of our own collection may be refused by hidclass.sys and a
           later config API arrival must not make that look like removal. */
        const auto held = std::find_if(channels_.begin(), channels_.end(), [&](const Channel &c) {
            return c.path == detail->DevicePath && c.opened;
        });
        const bool borrowed = held != channels_.end();
        HANDLE probe = borrowed ? held->handle
                                : CreateFileW(detail->DevicePath, 0,
                                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                                              nullptr, OPEN_EXISTING, 0, nullptr);
        if (probe == INVALID_HANDLE_VALUE) continue;

        HIDD_ATTRIBUTES attrs{};
        attrs.Size = sizeof(attrs);
        PHIDP_PREPARSED_DATA preparsed = nullptr;
        HIDP_CAPS caps{};
        const bool described = HidD_GetAttributes(probe, &attrs) &&
                               HidD_GetPreparsedData(probe, &preparsed) &&
                               HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS;

        wchar_t serial[256] = {0};
        if (described) HidD_GetSerialNumberString(probe, serial, sizeof serial);
        if (preparsed) HidD_FreePreparsedData(preparsed);
        if (!borrowed) CloseHandle(probe);
        if (!described) continue;

        switch (classify_collection(attrs.VendorID, attrs.ProductID, caps.UsagePage, caps.Usage)) {
        case Collection::ConfigApi:
            ++config_api_nodes;
            break;
        case Collection::NormalChannel:
        case Collection::ConfigChannel: {
            const Mode mode = attrs.VendorID == kConfigVendorId ? Mode::Config : Mode::Normal;
            found.push_back(Found{detail->DevicePath, serial, caps.InputReportByteLength,
                                  caps.OutputReportByteLength,
                                  mode == Mode::Config ? uint8_t{0}
                                                       : static_cast<uint8_t>(caps.Usage - kUsage),
                                  mode});
            break;
        }
        case Collection::None:
            break;
        }
    }

    SetupDiDestroyDeviceInfoList(set);
    return found;
}

bool HidTransport::refresh() {
    size_t config_apis = 0;
    std::vector<Found> found = sweep(config_apis);

    /* The first serial seen wins. Behaviour with more than one device attached
       is out of scope (#42), and the alternative to ignoring the second is
       silently mixing two boards into one session. */
    if (serial_.empty() && !found.empty()) serial_ = found.front().serial;
    if (!serial_.empty()) {
        const size_t before = found.size();
        found.erase(std::remove_if(found.begin(), found.end(),
                                   [&](const Found &f) {
                                       return !f.serial.empty() && f.serial != serial_;
                                   }),
                    found.end());
        if (found.size() != before)
            note("ignoring " + std::to_string(before - found.size()) +
                 " channel(s) on another serial; holding " + narrow(serial_));
    }

    const size_t normal_count = static_cast<size_t>(std::count_if(
        found.begin(), found.end(), [](const Found &f) { return f.mode == Mode::Normal; }));
    const size_t config_count = found.size() - normal_count;
    const Mode previous_mode = mode_;
    const Mode selected = discover(previous_mode, normal_count, config_count, config_apis, false).mode;
    found.erase(std::remove_if(found.begin(), found.end(),
                               [&](const Found &f) { return f.mode != selected; }),
                found.end());

    const bool changed = found.size() != channels_.size() ||
        std::any_of(found.begin(), found.end(), [&](const Found &f) {
            return std::none_of(channels_.begin(), channels_.end(),
                                [&](const Channel &c) { return c.path == f.path; });
        });
    const bool was_holding = holding_channels();
    /* OVERLAPPED addresses must remain stable until cancellation completes. */
    if (changed) release();

    /* Removals first: a channel that is gone must not be counted towards the
       set the next acquisition rolls back. */
    for (auto it = channels_.begin(); it != channels_.end();) {
        const bool still_there = std::any_of(found.begin(), found.end(), [&](const Found &f) {
            return f.path == it->path;
        });
        if (still_there) {
            ++it;
            continue;
        }
        close(*it);
        it = channels_.erase(it);
    }

    size_t added = 0;
    for (const Found &f : found) {
        const bool known = std::any_of(channels_.begin(), channels_.end(), [&](const Channel &c) {
            return c.path == f.path;
        });
        if (known) continue;
        Channel channel;
        channel.path = f.path;
        channel.index = f.index;
        channel.input_report_len = f.input_report_len;
        channel.output_report_len = f.output_report_len;
        channels_.push_back(std::move(channel));
        ++added;
    }

    /* The failure callback feeds the session and presence immediately. It
       must see the new mode before it reports loss of the old channel set. */
    mode_ = selected;
    if (changed) {
        std::sort(channels_.begin(), channels_.end(),
                  [](const Channel &a, const Channel &b) { return a.index < b.index; });
        if (was_holding && events_.transport_failed)
            events_.transport_failed("channel set changed");
    }
    const Discovery decision = discover(previous_mode, normal_count, config_count, config_apis,
                                        added > 0);
    if (added > 0) {
        note("channel(s) found on serial " +
             (serial_.empty() ? std::string("(none exposed)") : narrow(serial_)) + ": " +
             std::to_string(channels_.size()) + " so far");
    }

    if (decision.appeared && events_.device_appeared)
        events_.device_appeared(mode_ == Mode::Config ? DH_DEVICE_CONFIG_MODE : DH_DEVICE_NORMAL);
    if (decision.disappeared) {
        /*
         * Said out loud, as the counterpart of "channel(s) found on serial …".
         * Neither this nor dh_helper_device_left writes anything, so a device
         * going away was silent until the deferred state surfaced five seconds
         * later — and the minutes of quiet that follow, which are simply a
         * device that is not there, read as a helper that had stopped trying.
         * That cost #127, filed against the helper for a board that was in the
         * middle of a firmware pull.
         */
        note("the device went away" + (serial_.empty() ? std::string()
                                                       : " (serial " + narrow(serial_) + ")"));
        serial_.clear();
        if (events_.device_disappeared) events_.device_disappeared();
    }
    return decision.appeared || decision.disappeared;
}

bool HidTransport::holding_channels() const {
    return std::any_of(channels_.begin(), channels_.end(),
                       [](const Channel &c) { return c.opened; });
}

void HidTransport::acquire() {
    if (channels_.empty()) return;
    for (size_t i = 0; i < channels_.size(); ++i) {
        if (channels_[i].index == i) continue;
        release();
        if (events_.acquisition_refused)
            events_.acquisition_refused(0, static_cast<uint8_t>(channels_.size()));
        return;
    }
    /* Nothing to do when every channel is already held. With more than one
       channel (#63) the nodes arrive one at a time, so this runs again as each
       turns up and the session is re-established on the full set. */
    if (std::all_of(channels_.begin(), channels_.end(), [](const Channel &c) { return c.opened; }))
        return;

    uint8_t acquired = 0;
    for (Channel &channel : channels_) {
        if (channel.opened) {
            ++acquired;
            continue;
        }
        /*
         * dwShareMode = 0 is the whole of the exclusivity claim. ADR-0001
         * measured that even a dwDesiredAccess = 0 second open is then
         * refused — enforcement sits in hidclass.sys, uniform across ten
         * collections from four vendors — so there is no probe worth
         * performing and no race to lose between probing and opening.
         */
        channel.handle = CreateFileW(channel.path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                     nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (channel.handle == INVALID_HANDLE_VALUE) {
            /* Read before anything else can overwrite it. Naming which of the
               two this was is the whole of #125: the note that follows cannot
               know, and used to guess. */
            const DWORD error = GetLastError();
            note(error_text(device_is_gone(error) ? "open failed: the device is no longer there"
                                                  : "exclusive open refused",
                            error));
            break;
        }
        channel.read_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!channel.read_event) {
            note(last_error("could not create a read event"));
            CloseHandle(channel.handle);
            channel.handle = INVALID_HANDLE_VALUE;
            break;
        }
        /* Best effort: an older hidclass refuses a count this high, and the
           default it keeps is workable rather than good. Worth a line in the
           log either way, because it changes how much of a stall this helper
           survives. */
        if (!HidD_SetNumInputBuffers(channel.handle, kInputBuffers))
            note(last_error("could not deepen the input report buffer"));

        channel.buffer.assign(channel.input_report_len, uint8_t{0});
        channel.opened = true;
        if (!start_read(channel)) break;
        ++acquired;
    }

    if (acquired != channels_.size()) {
        /*
         * Partial acquisition is the dangerous outcome, not the tolerable one:
         * a second process holding one channel would silently receive part of
         * every bulk transfer while both sides looked healthy. Everything
         * opened is rolled back and the whole attempt is reported as refused.
         */
        release();

        /*
         * Reported as a refusal whichever it was, because this is the event
         * that carries the backoff retry and the once-only deferral the core
         * arms from it (dh_helper.c:419-425). Routing an absent device to
         * device_disappeared instead loses both: nothing re-arms acquisition,
         * so a helper that lost a race against the last notification of a
         * re-enumeration would wait for a notification that never comes.
         *
         * What #125 asked for is that the *log* stop asserting a cause nobody
         * checked. The truthful line is the one above, carrying the real Win32
         * error; the note no longer claims to know which happened.
         */
        if (events_.acquisition_refused)
            events_.acquisition_refused(acquired, static_cast<uint8_t>(channels_.size()));
        return;
    }

    note("holding " + std::to_string(acquired) + " channel(s) exclusively");
    if (events_.channels_acquired) events_.channels_acquired(acquired);
}

void HidTransport::release() {
    next_striped_ = 0;
    for (Channel &channel : channels_) close(channel);
}

void HidTransport::close(Channel &channel) {
    if (channel.handle != INVALID_HANDLE_VALUE) {
        if (channel.read_pending) {
            /*
             * Cancel *and wait*. A pending read owns this OVERLAPPED and this
             * buffer, and on the removal path refresh() erases the whole
             * Channel a moment later — so a cancellation still in flight would
             * complete into freed memory. CancelIoEx only asks; the wait is
             * what makes it true. It comes back ERROR_OPERATION_ABORTED, which
             * is the answer we want and not an error to report.
             */
            CancelIoEx(channel.handle, &channel.overlapped);
            DWORD ignored = 0;
            GetOverlappedResult(channel.handle, &channel.overlapped, &ignored, TRUE);
            channel.read_pending = false;
        }
        CloseHandle(channel.handle);
        channel.handle = INVALID_HANDLE_VALUE;
    }
    if (channel.read_event) {
        CloseHandle(channel.read_event);
        channel.read_event = nullptr;
    }
    channel.opened = false;
    channel.read_pending = false;
}

bool HidTransport::start_read(Channel &channel) {
    if (!channel.opened || channel.read_pending) return channel.opened;

    channel.overlapped = OVERLAPPED{};
    channel.overlapped.hEvent = channel.read_event;
    ResetEvent(channel.read_event);

    DWORD read = 0;
    if (ReadFile(channel.handle, channel.buffer.data(),
                 static_cast<DWORD>(channel.buffer.size()), &read, &channel.overlapped)) {
        /* Completed inline. The event is still signalled, so pump_reads picks
           it up on the next pass rather than this path duplicating delivery. */
        channel.read_pending = true;
        return true;
    }
    if (GetLastError() == ERROR_IO_PENDING) {
        channel.read_pending = true;
        return true;
    }

    const std::string reason = last_error("read failed");
    note(reason);
    if (events_.transport_failed) events_.transport_failed(reason);
    return false;
}

std::vector<HANDLE> HidTransport::wait_handles() const {
    std::vector<HANDLE> handles;
    for (const Channel &channel : channels_)
        if (channel.opened && channel.read_pending) handles.push_back(channel.read_event);
    return handles;
}

/*
 * Everything the driver is holding, not one report of it.
 *
 * One read per channel per pass caps this seam at one report per turn of the
 * run loop, and the loop also pumps window messages, owns the clipboard and
 * runs a tick. A pass that takes longer than a millisecond therefore falls
 * permanently behind a board emitting a report per millisecond, and the
 * driver's ring — see kInputBuffers — drops the overflow silently. Catching up
 * is the half of #143 that deepening the ring cannot do on its own.
 *
 * Bounded, because a channel delivering without pause must not hold the
 * message queue. The bound is the deepest the ring can be, so it is a ceiling
 * in every case rather than a target — the loop stops on an empty ring long
 * before it, and stops there whether or not HidD_SetNumInputBuffers took.
 */
void HidTransport::pump_reads() {
    for (Channel &channel : channels_) {
        for (ULONG taken = 0; taken < kInputBuffers; taken++) {
            if (!channel.opened || !channel.read_pending) break;
            if (WaitForSingleObject(channel.read_event, 0) != WAIT_OBJECT_0) break;

            DWORD read = 0;
            const BOOL ok = GetOverlappedResult(channel.handle, &channel.overlapped, &read, FALSE);
            channel.read_pending = false;
            if (!ok) {
                const std::string reason = last_error("read completion failed");
                note(reason);
                if (events_.transport_failed) events_.transport_failed(reason);
                break;
            }

            /*
             * Byte 0 is the report ID Windows prepends to every buffer, even on
             * a collection that declares none. It is not part of the report —
             * handing it to the reader would put a stray 0 where the
             * frame-start flag belongs, and every report would then read as
             * the orphan of a lost head.
             */
            if (read > 1 && events_.received) events_.received(channel.index, channel.buffer.data() + 1, read - 1);

            /* Re-armed inside the loop, so the next queued report is picked up
               on this pass. A refused re-arm has already reported itself. */
            if (!start_read(channel)) break;
        }
    }
}

bool HidTransport::send(const uint8_t *frame, size_t len, uint8_t count) {
    const bool striped = len > 0 && dh_msg_is_striped(frame[0]);
    const uint8_t index = striped && count > 0 ? next_striped_ % count : 0;
    if (count == 0 || count > channels_.size() ||
        !std::all_of(channels_.begin(), channels_.end(), [](const Channel &c) { return c.opened; }))
        return false;
    Channel *channel = &channels_[index];

    /*
     * The report the collection actually declares has to hold our 64 bytes
     * plus the report-ID byte Windows prepends. A device that declared less
     * would be a device this helper cannot speak to at all, and saying so is
     * better than writing past the end of the buffer.
     */
    if (channel->output_report_len < kReportSize + 1) {
        const std::string reason = "the channel declares a " +
                                   std::to_string(channel->output_report_len) +
                                   "-byte output report, too small to carry a " +
                                   std::to_string(kReportSize) + "-byte report";
        note(reason);
        if (events_.transport_failed) events_.transport_failed(reason);
        return false;
    }

    /* Behind the ID byte, the report's own byte 0 is the frame-start flag
       (ADR-0012), which lets the device's reader bridge a lost report; then
       63 bytes of stream.
       A report carries no length of its own, so the tail of the last one is
       filled with the padding byte — that is what tells the reader where the
       frame stopped (docs/protocol.md, "The report carrier"). */
    for (size_t offset = 0; offset < len; offset += DH_REPORT_STREAM_SIZE) {
        std::vector<uint8_t> report(channel->output_report_len,
                                   static_cast<uint8_t>(DH_FRAME_PAD));
        report[0] = 0; /* the report ID Windows expects in front of every write */
        report[1] = offset == 0 ? DH_REPORT_FRAME_START : DH_REPORT_FRAME_CONTINUES;
        const size_t take = std::min<size_t>(DH_REPORT_STREAM_SIZE, len - offset);
        std::memcpy(report.data() + 2, frame + offset, take);

        OVERLAPPED overlapped{};
        overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!overlapped.hEvent) {
            const std::string reason = last_error("could not create a write event");
            note(reason);
            if (events_.transport_failed) events_.transport_failed(reason);
            return false;
        }

        DWORD written = 0;
        BOOL ok = WriteFile(channel->handle, report.data(), static_cast<DWORD>(report.size()),
                            &written, &overlapped);
        if (!ok && GetLastError() == ERROR_IO_PENDING) {
            if (WaitForSingleObject(overlapped.hEvent, kWriteTimeoutMs) == WAIT_OBJECT_0) {
                ok = GetOverlappedResult(channel->handle, &overlapped, &written, FALSE);
            } else {
                /*
                 * CancelIoEx only *asks*. Returning here would leave the
                 * driver owning an OVERLAPPED on a stack frame about to be
                 * reused and a buffer about to be freed, and it would do that
                 * on exactly the path this timeout exists for — a device that
                 * has stopped draining. So the cancellation is waited out.
                 */
                CancelIoEx(channel->handle, &overlapped);
                GetOverlappedResult(channel->handle, &overlapped, &written, TRUE);
                ok = FALSE;
            }
        }
        CloseHandle(overlapped.hEvent);

        if (!ok || written != static_cast<DWORD>(report.size())) {
            /*
             * A refused write is a handle that can no longer be trusted — the
             * device has stopped draining or gone. The connection goes; this
             * is not a retryable write (dh_helper.h).
             */
            const std::string reason = last_error("report write failed");
            note(reason);
            if (events_.transport_failed) events_.transport_failed(reason);
            return false;
        }
    }
    if (striped) next_striped_ = static_cast<uint8_t>((index + 1u) % count);
    return true;
}

} // namespace deskhop
