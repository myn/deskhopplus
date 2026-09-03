#include "tray.h"

#include <shellapi.h>

#include "words.h"

namespace deskhop {

namespace {

constexpr UINT kIconId = 1;
constexpr UINT kIdStatus = 1;
constexpr UINT kIdAutostart = 2;
constexpr UINT kIdAutostartDetail = 3;
constexpr UINT kIdQuit = 4;
constexpr UINT kIdFileSummary = 5;
constexpr UINT kIdAcceptFiles = 6;
constexpr UINT kIdDeclineFiles = 7;
constexpr UINT kIdProgress = 8;
constexpr UINT kIdAbortTransfer = 9;
constexpr UINT kIdAbortSend = 10;

std::wstring widen(const std::string &text) {
    if (text.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    if (needed <= 1) return {};
    std::wstring out(static_cast<size_t>(needed - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out.data(), needed);
    return out;
}

/* Copy into one of NOTIFYICONDATAW's fixed fields, truncated rather than
   overrun. A tooltip is 128 characters and one of these messages is longer
   than that; losing its tail is better than losing the icon. */
void copy_into(wchar_t *field, size_t capacity, const std::wstring &text) {
    const size_t take = text.size() < capacity - 1 ? text.size() : capacity - 1;
    for (size_t i = 0; i < take; ++i) field[i] = text[i];
    field[take] = L'\0';
}

NOTIFYICONDATAW base(HWND window) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window;
    data.uID = kIconId;
    return data;
}

} // namespace

Tray::~Tray() { detach(); }

void Tray::attach(HWND window, Callbacks callbacks) {
    window_ = window;
    callbacks_ = std::move(callbacks);
}

void Tray::detach() {
    /* Removed explicitly on the way out. An icon left behind is an orphan the
       user cannot get rid of without hovering over it. */
    remove_icon();
    window_ = nullptr;
}

void Tray::add_icon() {
    if (icon_shown_ || !window_) return;
    NOTIFYICONDATAW data = base(window_);
    data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    data.uCallbackMessage = kCallbackMessage;
    data.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    copy_into(data.szTip, sizeof(data.szTip) / sizeof(wchar_t), L"deskhopplus helper");
    icon_shown_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
}

void Tray::remove_icon() {
    if (!icon_shown_ || !window_) return;
    NOTIFYICONDATAW data = base(window_);
    Shell_NotifyIconW(NIM_DELETE, &data);
    icon_shown_ = false;
}

void Tray::update_tooltip() {
    if (!icon_shown_ || !window_) return;
    NOTIFYICONDATAW data = base(window_);
    data.uFlags = NIF_TIP;
    /* What is happening now takes precedence over what the device is doing:
       a question waiting for an answer and a transfer under way are both
       things the user is meant to act on, and the state is one hover away. */
    std::string tip = words::state_message(state_);
    if (have_question_) {
        tip = "Files offered: " + summary(question_);
    } else if (progress_total_ > 0) {
        tip = "Receiving " + size_text(progress_received_) + " of " + size_text(progress_total_);
    }
    copy_into(data.szTip, sizeof(data.szTip) / sizeof(wchar_t),
              L"deskhopplus — " + widen(tip));
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Tray::balloon(const std::string &message) {
    if (!icon_shown_ || !window_) return;
    NOTIFYICONDATAW data = base(window_);
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_WARNING;
    copy_into(data.szInfoTitle, sizeof(data.szInfoTitle) / sizeof(wchar_t), L"deskhopplus");
    copy_into(data.szInfo, sizeof(data.szInfo) / sizeof(wchar_t), widen(message));
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Tray::show(dh_helper_state state) {
    const bool changed = state != state_;
    state_ = state;
    if (changed) announced_ = false;

    /*
     * The quiet state shows nothing at all — no icon, so nothing to hover
     * over. Looking for the device, and a device briefly away, are the helper
     * doing its job rather than something to report.
     */
    if (state_ == DH_HELPER_QUIET) {
        remove_icon();
        return;
    }

    add_icon();
    update_tooltip();

    if (!announced_ && words::state_names_a_remedy(state_)) {
        announced_ = true;
        balloon(words::state_message(state_));
    }
}

void Tray::on_callback(LPARAM what) {
    const UINT event = LOWORD(what);
    if (event == WM_RBUTTONUP || event == WM_LBUTTONUP || event == WM_CONTEXTMENU) show_menu();
}

void Tray::show_menu() {
    if (!window_) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const std::string status = words::state_message(state_);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, kIdStatus,
                widen(status.empty() ? "Looking for the device" : status).c_str());

    if (have_question_) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, kIdFileSummary,
                    widen(summary(question_)).c_str());
        AppendMenuW(menu, MF_STRING, kIdAcceptFiles, L"Accept and start the transfer");
        AppendMenuW(menu, MF_STRING, kIdDeclineFiles, L"Decline");
    }

    if (progress_total_ > 0) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const unsigned percent =
            static_cast<unsigned>(progress_received_ * 100u / progress_total_);
        AppendMenuW(menu, MF_STRING | MF_GRAYED, kIdProgress,
                    widen("Receiving " + size_text(progress_received_) + " of " +
                          size_text(progress_total_) + " \xe2\x80\x94 " +
                          std::to_string(percent) + "%").c_str());
        AppendMenuW(menu, MF_STRING, kIdAbortTransfer, L"Cancel this transfer");
    }

    if (callbacks_.is_sending && callbacks_.is_sending()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kIdAbortSend, L"Cancel what is being sent");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    const bool enabled = callbacks_.autostart_enabled && callbacks_.autostart_enabled();
    AppendMenuW(menu, MF_STRING | (enabled ? MF_CHECKED : 0u), kIdAutostart, L"Start at logon");
    if (callbacks_.autostart_detail) {
        const std::string detail = callbacks_.autostart_detail();
        if (!detail.empty())
            AppendMenuW(menu, MF_STRING | MF_GRAYED, kIdAutostartDetail, widen(detail).c_str());
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdQuit, L"Quit deskhopplus helper");

    POINT where{};
    GetCursorPos(&where);
    /* Required for the menu to dismiss when the user clicks elsewhere — a
       tray menu on a window that is not foreground otherwise stays up. */
    SetForegroundWindow(window_);
    const UINT chosen = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, where.x, where.y, 0, window_,
        nullptr));
    DestroyMenu(menu);

    if (chosen == kIdAutostart && callbacks_.toggle_autostart) callbacks_.toggle_autostart();
    else if (chosen == kIdQuit && callbacks_.quit) callbacks_.quit();
    else if (chosen == kIdAcceptFiles && have_question_) {
        const uint32_t id = question_.id;
        have_question_ = false;
        update_tooltip();
        if (callbacks_.accept_files) callbacks_.accept_files(id);
    } else if (chosen == kIdDeclineFiles && have_question_) {
        const uint32_t id = question_.id;
        have_question_ = false;
        update_tooltip();
        if (callbacks_.decline_files) callbacks_.decline_files(id);
    } else if (chosen == kIdAbortTransfer && callbacks_.abort_transfer) {
        callbacks_.abort_transfer();
    } else if (chosen == kIdAbortSend && callbacks_.abort_send) {
        callbacks_.abort_send();
    }
}

void Tray::ask_about_files(const deskhop::FileOffer &offer) {
    question_ = offer;
    have_question_ = true;
    /* The icon may not be showing: the quiet state hides it, and a question
       the user cannot see is a transfer that never happens. */
    add_icon();
    update_tooltip();
    balloon("Files from the other computer: " + summary(offer) +
            " Use the deskhopplus icon to accept or decline.");
}

void Tray::withdraw_file_question(uint32_t id) {
    if (!have_question_ || question_.id != id) return;
    have_question_ = false;
    update_tooltip();
}

void Tray::show_progress(uint64_t received, uint64_t total) {
    progress_received_ = received;
    progress_total_ = total;
    update_tooltip();
}

std::string Tray::summary(const deskhop::FileOffer &offer) {
    const std::string what = offer.files.size() == 1
                                 ? offer.files.front().name
                                 : std::to_string(offer.files.size()) + " files";
    return what + " \xe2\x80\x94 " + size_text(offer.total) + ", about " +
           duration_text(offer.estimated_seconds()) + ".";
}

/* Integer arithmetic, and truncating rather than rounding — the same spelling
   as `MenuBar.size` on the other computer, so the two ends quote one transfer
   at one size. */
std::string Tray::size_text(uint64_t bytes) {
    if (bytes >= 1024u * 1024u) {
        const uint64_t tenths = (bytes * 10u) / (1024u * 1024u);
        return std::to_string(tenths / 10u) + "." + std::to_string(tenths % 10u) + " MB";
    }
    if (bytes >= 1024u) return std::to_string(bytes / 1024u) + " KB";
    return std::to_string(bytes) + " bytes";
}

std::string Tray::duration_text(uint32_t seconds) {
    if (seconds < 60u) return std::to_string(seconds < 1u ? 1u : seconds) + " seconds";
    const uint32_t minutes = (seconds + 59u) / 60u;
    return minutes == 1u ? std::string("a minute") : std::to_string(minutes) + " minutes";
}

} // namespace deskhop
