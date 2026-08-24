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
    copy_into(data.szTip, sizeof(data.szTip) / sizeof(wchar_t),
              L"deskhopplus — " + widen(words::state_message(state_)));
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
}

} // namespace deskhop
