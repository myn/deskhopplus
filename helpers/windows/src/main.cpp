/*
 * The Windows helper: a single portable exe that finds the device, seizes
 * every channel, introduces itself, keeps the session alive (#49), and carries
 * the clipboard across it (#52).
 *
 * Clipboard text and images — files are #56, and cursor
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
 * messages and owns the clock. What each output *means* is output_dispatch.cpp,
 * so that every arm of it is reachable by a test (#152); this file is the Win32
 * half of that seam.
 * ---------------------------------------------------------------------------
 */

#include <windows.h>

#include <dbt.h>
#include <shlobj.h>

#include <share.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "autostart.h"
#include "channel_identity.h"
#include "clip_service.h"
#include "clipboard.h"
#include "cursor_placement.h"
#include "dh_p256.h"
#include "helper_session.h"
#include "hid_transport.h"
#include "output_dispatch.h"
#include "seal_aead.h"
#include "secret_store.h"
#include "tray.h"

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

/* How often to re-sweep while no channel has been found (#157). Long enough
   that an absent device costs one SetupAPI enumeration a second and nothing
   else — the sweep logs only when its answer changes — and short enough that
   a sweep taken a few milliseconds into a reboot's device rebuild is
   corrected before the user reaches for the tray. Nothing sweeps once a
   channel is found. */
constexpr uint32_t kRescanMs = 1000;

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

/*
 * The shim. What each output *means* is output_dispatch.cpp's, which is where
 * a test can watch it (#152); this class is the Win32 half of that seam — the
 * tray, the transport, this computer's clipboard, the secret store and the
 * clock — plus the run loop that carries messages between them.
 */
class Helper : public HelperEffects {
  public:
    bool start(HINSTANCE instance);
    int run();
    void stop() { PostQuitMessage(0); }

    /* HelperEffects: one line each over the real object. Public because the
       interface is, and for no other reason — nothing calls them but the
       dispatch. */
    bool store_board_key(const std::vector<uint8_t> &key) override {
        return secrets_.save_board_key(key);
    }
    void acquire_channels() override { transport_.acquire(); }
    void release_channels() override { transport_.release(); }
    bool send(const std::vector<uint8_t> &frame) override {
        return transport_.send(frame.data(), frame.size());
    }
    bool build_frame(uint8_t type, const std::vector<uint8_t> &body,
                     std::vector<uint8_t> &out) override {
        return session_->emit(type, body, out);
    }
    void note_sent() override { session_->note_sent(now_ms()); }
    void note_send_refused() override { session_->note_send_refused(); }
    void show_state(dh_helper_state state) override { tray_.show(state); }
    void deliver_text(const std::vector<uint8_t> &utf8) override {
        clipboard_.deliver_text(utf8);
    }
    void deliver_image(const std::vector<uint8_t> &png) override {
        if (waiting_for_image_) awaited_image_ = png;
        else clipboard_.deliver_image(png);
    }
    void lazy_image(uint32_t id, uint64_t total) override { clipboard_.lazy_image(id, total); }
    void cancel_lazy_image(uint32_t id) override { clipboard_.cancel_lazy_image(id); }
    void schedule_retry(uint32_t after_ms) override {
        /* Compared as an unsigned difference in run(), never as `now >= then`:
           the clock is 32-bit milliseconds and wraps, and a plain comparison
           would fire every retry at once for the 24 days after it does. */
        retry_pending_ = true;
        retry_at_ = now_ms() + after_ms;
    }
    std::vector<ClipOutput> clip_policy_changed(uint8_t flags) override {
        return clipboard_service_->policy_changed(flags);
    }
    void log(const std::string &message) override;

  private:
    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM w, LPARAM l);
    LRESULT handle(UINT message, WPARAM w, LPARAM l);

    void feed(const std::vector<Output> &outputs);
    std::optional<std::vector<uint8_t>> request_lazy_image(uint32_t id, uint64_t total);

    /*
     * Monotonic, deliberately. A wall clock going backwards — routine on a
     * laptop coming out of sleep — would stall the heartbeat past the device's
     * absence deadline and kill a healthy session. The core compares its
     * milliseconds as unsigned differences, so truncating to 32 bits here is
     * arithmetic rather than a session dropped every 49 days.
     */
    uint32_t now_ms() const { return static_cast<uint32_t>(GetTickCount64()); }

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
    std::unique_ptr<CursorPlacement> cursor_placement_;
    HidTransport transport_;
    Tray tray_;
    Clipboard clipboard_;
    OutputDispatch dispatch_{*this};

    uint32_t last_tick_{0};
    uint32_t last_rescan_{0};
    bool retry_pending_{false};
    uint32_t retry_at_{0};
    /*
     * Whether the last thing the session said was that bulk may cross. The
     * clipboard has to be told when a session *ends* — its seal and any
     * transfer go with it — and the session reports a state rather than an
     * event, so the transition is worked out here.
     */
    bool bulk_was_allowed_{false};
    bool waiting_for_image_{false};
    std::optional<std::vector<uint8_t>> awaited_image_;
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
    cursor_placement_ = std::make_unique<CursorPlacement>(
        [this](const std::string &message) { log(message); },
        [this](uint8_t type, const std::vector<uint8_t> &body) {
            std::vector<uint8_t> frame;
            if (!session_->emit(type, body, frame)) {
                log("a cursor-position response could not be built; there is no session");
                return;
            }
            if (transport_.send(frame.data(), frame.size())) {
                session_->note_sent(now_ms());
                if (type == DH_MSG_POS_RESPONSE && !body.empty())
                    log("cursor response id=" + std::to_string(body[0]) + " sent");
            } else {
                session_->note_send_refused();
                log("a cursor-position response was not taken by the transport and is lost");
            }
        });

    /* Verified bulk frames, straight from the core. Nothing here re-reads the
       stream: decode, tag and replay counter are all upstream of this. */
    session_->set_payload_sink([this](uint8_t type, const uint8_t *body, size_t len) {
        if (cursor_placement_->received(type, body, len, now_ms())) return;
        dispatch_.emit(clipboard_service_->received(type, body, len));
    });

    Clipboard::Callbacks clipboard_callbacks;
    clipboard_callbacks.log = [this](const std::string &m) { log(m); };
    clipboard_callbacks.local_copy = [this](std::vector<uint8_t> utf8) {
        /* Nothing is offered without a session to carry it. The state the user
           is shown and this answer come from the same core, so a helper that
           says "connected" and refuses a copy is not a state this can reach. */
        if (!session_->can_send_bulk()) return;
        dispatch_.emit(clipboard_service_->local_copy(ClipKind::Text, utf8));
    };
    clipboard_callbacks.local_image = [this](std::vector<uint8_t> png) {
        if (!session_->can_send_bulk()) return;
        dispatch_.emit(clipboard_service_->local_copy(ClipKind::Png, png));
    };
    clipboard_callbacks.request_image = [this](uint32_t id, uint64_t total) {
        return request_lazy_image(id, total);
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

    last_tick_ = last_rescan_ = now_ms();
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
                    /* Destroying the clipboard-owner window gives Windows its
                       WM_RENDERALLFORMATS chance before the channel stops. */
                    if (window_ != nullptr) {
                        DestroyWindow(window_);
                        window_ = nullptr;
                    }
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
        /* Unsigned difference, the same shape as the tick below and for the
           same reason: GetTickCount64 is truncated to 32 bits here and wraps.
           Guarded on has_device() — nothing found — rather than on
           holding_channels(), so this can never race the retry above, which
           runs only when there *is* something to acquire. */
        if (!transport_.has_device() && now - last_rescan_ >= kRescanMs) {
            last_rescan_ = now;
            transport_.rescan();
            /* Said out loud, because the sweep's own "channel(s) found" line
               cannot say which caller asked for it, and that is exactly what
               validating #157 needs to see: a recovery no device event could
               have delivered. The guard above was false a line ago, so this
               is only ever the rescan's own doing. */
            if (transport_.has_device())
                log("found by the idle rescan, with no device event to prompt it");
        }
        if (now - last_tick_ >= kTickMs) {
            last_tick_ = now;
            feed(session_->tick(now));
            /* A chance to push the next credit-gated batch. On the tick as well
               as on arriving frames, so a transfer whose last credit grant was
               lost still finishes rather than sitting still. */
            if (session_->can_send_bulk()) dispatch_.emit(clipboard_service_->pump());
            /* And a chance to give up on one that has stopped moving — the far
               helper having crashed leaves this end's session perfectly
               healthy, so nothing else here would ever notice. */
            /* The board's drop totals go with the tick so that an
               abandonment can quote them (#133). Read here rather than held
               there: the board restates them whenever they move, and nothing
               tells the clipboard when that was. */
            dh_device_drops drops{};
            const bool stated = session_->device_drops(&drops);
            dispatch_.emit(clipboard_service_->tick(now, stated ? &drops : nullptr));
        }
    }
}

void Helper::feed(const std::vector<Output> &outputs) {
    dispatch_.apply(outputs);

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
    if (bulk_was_allowed_ && !live) dispatch_.emit(clipboard_service_->session_ended());
    bulk_was_allowed_ = live;
}

std::optional<std::vector<uint8_t>> Helper::request_lazy_image(uint32_t id, uint64_t total) {
    awaited_image_.reset();
    waiting_for_image_ = true;
    dispatch_.emit(clipboard_service_->request_lazy_image(id));
    const uint32_t started = now_ms();
    const uint64_t estimated_ms = total * 1000u / (49u * 1024u);
    const uint32_t timeout_ms = static_cast<uint32_t>(
        std::min<uint64_t>(estimated_ms + 30000u, UINT32_MAX / 2u));
    while (!awaited_image_ && session_->can_send_bulk() &&
           now_ms() - started < timeout_ms) {
        transport_.pump_reads();
        feed(session_->tick(now_ms()));
        if (session_->can_send_bulk()) dispatch_.emit(clipboard_service_->pump());
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                waiting_for_image_ = false;
                PostQuitMessage(static_cast<int>(message.wParam));
                return std::nullopt;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        Sleep(1);
    }
    waiting_for_image_ = false;
    return std::exchange(awaited_image_, std::nullopt);
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
        clipboard_.handle(message, w);
        return 0;

    case WM_RENDERFORMAT:
    case WM_RENDERALLFORMATS:
    case WM_DESTROYCLIPBOARD:
        /* Delayed image rendering and ownership loss are directed to the
           owner window, not delivered as clipboard-update notifications. */
        clipboard_.handle(message, w);
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
