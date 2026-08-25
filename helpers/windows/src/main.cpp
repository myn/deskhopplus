/*
 * The Windows helper: a single portable exe that finds the device, seizes
 * every channel, introduces itself, keeps the session alive (#49), and carries
 * the clipboard across it (#52).
 *
 * Clipboard text only so far — images are #55, files are #56, and cursor
 * placement is #51. Nothing needs installing: see helpers/windows/README.md
 * and ADR-0006.
 *
 * ---------------------------------------------------------------------------
 * THE LOOP, AND WHY IT IS ONE THREAD
 *
 * Device events arrive as WM_DEVICECHANGE on this window. Reports arrive as
 * overlapped read completions. MsgWaitForMultipleObjectsEx waits on both at
 * once, so they are handled in arrival order on the thread that owns the
 * window — the same thread the tray and (later) clipboard ownership must run
 * on anyway. The shared core assumes a single-threaded caller. There is
 * nothing here to lock, and that is a decision rather than an accident.
 *
 * Nothing in this file decides anything about the session. Every decision is
 * src/core/dh_helper.c, reached through HelperSession. This file carries
 * messages, owns the clock, and turns an output into a log line or a tooltip.
 * ---------------------------------------------------------------------------
 */

#include <windows.h>

#include <dbt.h>
#include <shlobj.h>

#include <share.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "autostart.h"
#include "channel_identity.h"
#include "clip_service.h"
#include "clipboard.h"
#include "dh_p256.h"
#include "helper_session.h"
#include "hid_transport.h"
#include "seal_aead.h"
#include "secret_store.h"
#include "tray.h"
#include "words.h"

namespace deskhop {

namespace {

constexpr wchar_t kWindowClass[] = L"deskhopplus_helper";
/* One instance per user session. Two helpers would both try to seize the same
   channel and the second would sit in "every channel refused" for ever, which
   is a true report of a self-inflicted problem and a confusing one — the
   likeliest way to reach it is autostart having already started one. */
constexpr wchar_t kInstanceMutex[] = L"Local\\deskhopplus-helper";

/* Fine enough that a heartbeat is never late by much, and the same figure the
   macOS helper ticks at. ADR-0004's interval is a second; a quarter of it
   leaves room for the wait to be woken by something else first. */
constexpr uint32_t kTickMs = 250;

std::string hex(const std::vector<uint8_t> &bytes) {
    std::string out;
    char pair[3];
    for (uint8_t byte : bytes) {
        std::snprintf(pair, sizeof pair, "%02x", byte);
        out += pair;
    }
    return out;
}

} // namespace

class Helper {
  public:
    bool start(HINSTANCE instance);
    int run();
    void stop() { PostQuitMessage(0); }

  private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l);
    LRESULT handle(UINT message, WPARAM w, LPARAM l);

    void feed(const std::vector<Output> &outputs);
    void apply(const Output &output);
    void emit(const std::vector<ClipOutput> &outputs);

    /*
     * Monotonic, deliberately. A wall clock going backwards — routine on a
     * laptop coming out of sleep — would stall the heartbeat past the device's
     * absence deadline and kill a healthy session. The core compares its
     * milliseconds as unsigned differences, so truncating to 32 bits here is
     * arithmetic rather than a session dropped every 49 days.
     */
    uint32_t now_ms() const { return static_cast<uint32_t>(GetTickCount64()); }

    void log(const std::string &message);
    std::string autostart_detail() const;
    Tray::Callbacks tray_callbacks();

    static Helper *instance_;

    HWND window_{nullptr};
    UINT taskbar_created_{0};
    HANDLE single_instance_{nullptr};
    FILE *log_file_{nullptr};

    SecretStore secrets_;
    std::unique_ptr<HelperSession> session_;
    std::unique_ptr<Autostart> autostart_;
    std::unique_ptr<ClipService> clipboard_service_;
    HidTransport transport_;
    Tray tray_;
    Clipboard clipboard_;

    uint32_t last_tick_{0};
    bool retry_pending_{false};
    uint32_t retry_at_{0};
    /*
     * Whether the last thing the session said was that bulk may cross. The
     * clipboard has to be told when a session *ends* — its seal and any
     * transfer go with it — and the session reports a state rather than an
     * event, so the transition is worked out here.
     */
    bool bulk_was_allowed_{false};
};

Helper *Helper::instance_ = nullptr;

void Helper::log(const std::string &message) {
    /* A WIN32-subsystem process has no console, so the log is a file beside
       the helper's other state plus the debugger's stream — both readable on
       a machine where nothing may be installed to read them. */
    const std::string line = "[" + std::to_string(now_ms()) + "ms] " + message + "\n";
    if (log_file_) {
        std::fputs(line.c_str(), log_file_);
        std::fflush(log_file_);
    }
    OutputDebugStringA(line.c_str());
}

bool Helper::start(HINSTANCE instance) {
    instance_ = this;

    single_instance_ = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (!single_instance_ || GetLastError() == ERROR_ALREADY_EXISTS) return false;

    const std::wstring log_path = secrets_.directory() + L"\\helper.log";
    SHCreateDirectoryExW(nullptr, secrets_.directory().c_str(), nullptr);
    /*
     * _wfsopen and not _wfopen_s: the secure variant opens a file
     * non-shareable, which locked this log against every reader while the
     * helper ran. Reading it is the documented way to tell a refused channel
     * from a disconnected device (#114), so a log nobody can open until the
     * helper is quit answers the question only after destroying its subject.
     * _SH_DENYWR keeps this process the only writer and lets anything read.
     */
    log_file_ = _wfsopen(log_path.c_str(), L"a", _SH_DENYWR);

    /*
     * Apartment-threaded, for the shell. The startup-folder rung of the
     * autostart ladder writes its shortcut through IShellLink, and the tray
     * lives on this thread — both want an STA, and initialising it once here
     * beats each of them doing it and undoing it around every call.
     */
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    WNDCLASSW window_class{};
    window_class.lpfnWndProc = &Helper::window_proc;
    window_class.hInstance = instance;
    window_class.lpszClassName = kWindowClass;
    if (!RegisterClassW(&window_class)) return false;

    /*
     * Message-only. There is no window worth showing: the helper's whole face
     * is a notification-area icon, and a hidden top-level window would still
     * appear in Alt-Tab and the taskbar on some shells.
     *
     * A message-only window does not receive broadcast WM_DEVICECHANGE, but it
     * does receive the ones a RegisterDeviceNotification asks for — which is
     * all this helper wants, and narrower than a broadcast besides.
     */
    window_ = CreateWindowExW(0, kWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr,
                              instance, nullptr);
    if (!window_) return false;

    /* Explorer restarting takes every tray icon with it and then asks for them
       back with this message. Without it the helper is invisible until its
       next state change. */
    taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");

    SecretStore::Identity identity;
    if (!secrets_.load_identity(identity)) {
        log("could not load or create this helper's key; the system RNG refused");
        return false;
    }
    /* The key id every hello carries, and the value the board's config page
       shows as *Paired helper* (#114) — same spelling, so the two can be
       compared by eye. */
    log("helper key id: " + hex(identity.key_id));

    /*
     * Both halves of "am I paired?", said at startup rather than left to be
     * inferred from what happens next. Without them a helper that pairs and is
     * a stranger again after a restart looks like the board forgetting, and
     * the sitting goes looking at the firmware.
     */
    if (!identity.persisted)
        log("this key could not be written and exists only in memory; pairing will work now and "
            "be gone after a restart, because the next start draws a different key");

    std::vector<uint8_t> board_key = secrets_.load_board_key();
    log(board_key.empty() ? "no stored board key: this helper must pair before it has a session"
                          : "stored board key found: this helper has paired with a board before");

    Identity core_identity;
    core_identity.public_key = identity.public_key;
    core_identity.key_id = identity.key_id;
    /* The one ECDH the core asks a platform for. The HKDF over the result
       stays in the core: a helper deriving its own session keys would be a
       second implementation of the rule both ends must agree on. */
    core_identity.ecdh = [private_key = identity.private_key](const uint8_t *peer,
                                                             uint8_t *shared) {
        return dh_p256_ecdh(private_key.data(), peer, shared);
    };

    session_ = std::make_unique<HelperSession>(
        std::move(core_identity), std::move(board_key),
        [](uint8_t *out, size_t len) {
            /* A short draw would leave the core keying on bytes nobody chose.
               There is nothing to fall back to, so this stops. */
            if (!fill_random(out, len)) std::abort();
        });

    /*
     * The clipboard. `seal_aead()` is null on a machine where CNG will not give
     * up an AES-GCM provider — the service refuses every copy in that case
     * rather than falling back to sending a payload in clear (ADR-0008).
     */
    if (seal_aead() == nullptr)
        log("this machine has no AES-GCM provider; the clipboard cannot be sealed and so will "
            "not be carried");
    clipboard_service_ = std::make_unique<ClipService>(seal_aead(), [](uint8_t *out, size_t len) {
        /* A short draw would key a seal on bytes nobody chose. There is
           nothing to fall back to, so this stops. */
        if (!fill_random(out, len)) std::abort();
    });

    /* Verified bulk frames, straight from the core. Nothing here re-reads the
       stream: decode, tag and replay counter are all upstream of this. */
    session_->set_payload_sink([this](uint8_t type, const uint8_t *body, size_t len) {
        emit(clipboard_service_->received(type, body, len));
    });

    Clipboard::Callbacks clipboard_callbacks;
    clipboard_callbacks.log = [this](const std::string &m) { log(m); };
    clipboard_callbacks.local_copy = [this](std::vector<uint8_t> utf8) {
        /* Nothing is offered without a session to carry it. The state the user
           is shown and this answer come from the same core, so a helper that
           says "connected" and refuses a copy is not a state this can reach. */
        if (!session_->can_send_bulk()) return;
        emit(clipboard_service_->local_copy(ClipKind::Text, utf8));
    };
    clipboard_.attach(window_, std::move(clipboard_callbacks));

    autostart_ = std::make_unique<Autostart>(secrets_.directory(),
                                             [this](const std::string &m) { log(m); });
    autostart_->start(Autostart::launched_by_autostart());

    tray_.attach(window_, tray_callbacks());

    HidTransport::Events events;
    events.log = [this](const std::string &m) { log(m); };
    events.device_appeared = [this](dh_device_identity which) {
        feed(session_->device_appeared(which, now_ms()));
    };
    events.device_disappeared = [this] { feed(session_->device_disappeared(now_ms())); };
    events.channels_acquired = [this](uint8_t count) {
        feed(session_->channels_acquired(count, now_ms()));
    };
    events.acquisition_refused = [this](uint8_t acquired, uint8_t of) {
        feed(session_->acquisition_refused(acquired, of, now_ms()));
    };
    events.received = [this](const uint8_t *data, size_t len) {
        feed(session_->received(data, len, now_ms()));
    };
    events.transport_failed = [this](const std::string &reason) {
        feed(session_->transport_failed(reason, now_ms()));
    };

    last_tick_ = now_ms();
    log("deskhop helper started; waiting for the channel");
    transport_.start(window_, std::move(events));
    return true;
}

int Helper::run() {
    for (;;) {
        std::vector<HANDLE> handles = transport_.wait_handles();
        const DWORD count = static_cast<DWORD>(handles.size());

        const DWORD result = MsgWaitForMultipleObjectsEx(
            count, count ? handles.data() : nullptr, kTickMs, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        if (result == WAIT_OBJECT_0 + count) {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                if (message.message == WM_QUIT) {
                    transport_.stop();
                    clipboard_.detach();
                    tray_.detach();
                    return static_cast<int>(message.wParam);
                }
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
        }

        /* Unconditional: a completion that arrived while the loop was in a
           message handler is still sitting there, and its event was consumed
           by whatever woke the wait. */
        transport_.pump_reads();

        const uint32_t now = now_ms();
        if (retry_pending_ && now - retry_at_ < 0x80000000u) {
            retry_pending_ = false;
            /* Only when there is something to acquire and it is not already
               held — the device may have come back on its own in the meantime. */
            if (transport_.has_device() && !transport_.holding_channels()) transport_.acquire();
        }
        if (now - last_tick_ >= kTickMs) {
            last_tick_ = now;
            feed(session_->tick(now));
            /* A chance to push the next credit-gated batch. On the tick as well
               as on arriving frames, so a transfer whose last credit grant was
               lost still finishes rather than sitting still. */
            if (session_->can_send_bulk()) emit(clipboard_service_->pump());
            /* And a chance to give up on one that has stopped moving — the far
               helper having crashed leaves this end's session perfectly
               healthy, so nothing else here would ever notice. */
            emit(clipboard_service_->tick(now));
        }
    }
}

void Helper::feed(const std::vector<Output> &outputs) {
    for (const Output &output : outputs) apply(output);

    /*
     * A session that has gone takes the seal and any transfer with it.
     *
     * Asked of `can_send_bulk` — the *session's* answer — and after every batch
     * of outputs, not of the state the user is shown and not only when that
     * state changes. `dh_helper_allows_bulk` counts RECONNECTING_REPEATEDLY as
     * allowing bulk, and that is precisely the state a teardown lands in once
     * the flap rate has tripped: the session is gone, its keys are cleared, and
     * the state reads true before and after. Worse, the core reports that state
     * only on the transition, so the second and subsequent drops of a burst
     * produce no state output at all. #107 measured 586 teardowns in sixteen
     * hours — the exact condition that trips the rate — so the edge that
     * matters is the one this misses.
     */
    const bool live = session_->can_send_bulk();
    if (bulk_was_allowed_ && !live) emit(clipboard_service_->session_ended());
    bulk_was_allowed_ = live;
}

void Helper::apply(const Output &output) {
    switch (output.kind) {
    case Output::Kind::StoreBoardKey:
        if (!secrets_.save_board_key(output.bytes))
            log("paired, but the board key could not be stored — pairing will not survive a "
                "restart");
        break;

    case Output::Kind::OpenChannels:
        transport_.acquire();
        break;

    case Output::Kind::CloseChannels:
        transport_.release();
        break;

    case Output::Kind::Send:
        transport_.send(output.bytes.data(), output.bytes.size());
        break;

    case Output::Kind::State:
        if (!words::state_is_known(output.state))
            log("the core reported state " + std::to_string(static_cast<int>(output.state)) +
                ", which this helper has no words for");
        log("state: " + (words::state_message(output.state).empty()
                             ? std::string("(nothing to report)")
                             : words::state_message(output.state)));
        tray_.show(output.state);
        break;

    case Output::Kind::ClipPolicy:
        emit(clipboard_service_->policy_changed(output.clip_flags));
        break;

    case Output::Kind::Retry:
        /* Compared as an unsigned difference below, never as `now >= then`:
           the clock is 32-bit milliseconds and wraps, and a plain comparison
           would fire every retry at once for the 24 days after it does. */
        retry_pending_ = true;
        retry_at_ = now_ms() + output.retry_after_ms;
        break;

    case Output::Kind::Note:
        log(output.note);
        break;
    }
}

/*
 * The clipboard's outputs: frames to authenticate and send, payloads to write,
 * and diagnostics.
 *
 * Every frame goes out through `session_->emit`, never with a counter of this
 * file's own — the counter space belongs to the session key and the heartbeat
 * is already writing into it. `note_sent` is what keeps ADR-0004's beat out of
 * a direction that is far from idle.
 */
void Helper::emit(const std::vector<ClipOutput> &outputs) {
    for (const ClipOutput &output : outputs) {
        switch (output.kind) {
        case ClipOutput::Kind::Send: {
            std::vector<uint8_t> frame;
            if (!session_->emit(output.type, output.bytes, frame)) {
                log("a clipboard frame could not be built; there is no session");
                break;
            }
            /* The idle timer is charged only for a frame the transport
               actually took. Charging for one it refused would suppress a beat
               that ADR-0004 owed the board — which is exactly what
               HelperSession::emit says not to do. */
            if (transport_.send(frame.data(), frame.size())) session_->note_sent(now_ms());
            break;
        }

        case ClipOutput::Kind::Deliver:
            if (output.payload_kind != static_cast<uint8_t>(ClipKind::Text)) {
                log("a payload of kind " + std::to_string(output.payload_kind) +
                    " arrived, which this slice does not write — images are #55 and files "
                    "are #56");
                break;
            }
            clipboard_.deliver_text(output.bytes);
            break;

        case ClipOutput::Kind::Note:
            log(output.note);
            break;
        }
    }
}

Tray::Callbacks Helper::tray_callbacks() {
    return Tray::Callbacks{
        [this] { return autostart_->record().enabled; },
        [this] { return autostart_detail(); },
        /* Opt-in, and opt back out. Never touched on first run: a portable exe
           that silently writes a logon task is a surprise nobody asked for. */
        [this] {
            if (autostart_->record().enabled) autostart_->disable();
            else autostart_->enable();
        },
        [this] { stop(); },
    };
}

std::string Helper::autostart_detail() const {
    switch (autostart_->status()) {
    case autostart::Verification::NotEnabled:
        return {};
    case autostart::Verification::NotRegistered:
        /* Logged, not shouted about. The helper is an enhancement, never a
           dependency, and a manually-launched one is fully functional. */
        return "  (this machine refused every method)";
    case autostart::Verification::RegisteredNotYetProven:
        return std::string("  (") + autostart::name(autostart_->record().mechanism) +
               ", not yet seen to fire)";
    case autostart::Verification::Confirmed:
        return std::string("  (") + autostart::name(autostart_->record().mechanism) +
               ", confirmed)";
    }
    return {};
}

LRESULT Helper::handle(UINT message, WPARAM w, LPARAM l) {
    if (message == taskbar_created_ && taskbar_created_ != 0) {
        /* The shell forgot every icon. Re-assert whatever the current state
           says should be showing — including nothing, for the quiet state. */
        tray_.detach();
        tray_.attach(window_, tray_callbacks());
        tray_.show(session_->state());
        return 0;
    }

    switch (message) {
    case WM_DEVICECHANGE:
        /*
         * Any device event re-sweeps, and re-sweeping is also how a refusal
         * recovers. The program that holds the channel *releasing* its handle
         * produces no device notification at all, which is why the capped
         * backoff runs beside this rather than instead of it: passive waiting
         * would be waiting for ever.
         */
        transport_.on_device_change(w, l);
        return TRUE;

    case WM_CLIPBOARDUPDATE:
        /* Something was copied on this computer. Handled on this thread
           because setting clipboard data requires owning a window, and this is
           the window (clipboard.h). */
        clipboard_.handle(message);
        return 0;

    case Tray::kCallbackMessage:
        tray_.on_callback(l);
        return 0;

    case WM_CLOSE:
    case WM_ENDSESSION:
        stop();
        return 0;

    case WM_DESTROY:
        stop();
        return 0;

    default:
        break;
    }
    return DefWindowProcW(window_, message, w, l);
}

LRESULT CALLBACK Helper::window_proc(HWND window, UINT message, WPARAM w, LPARAM l) {
    if (instance_ && instance_->window_ == window) return instance_->handle(message, w, l);
    return DefWindowProcW(window, message, w, l);
}

} // namespace deskhop

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    deskhop::Helper helper;
    if (!helper.start(instance)) return 1;
    return helper.run();
}
