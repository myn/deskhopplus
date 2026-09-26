/* SPDX-License-Identifier: GPL-3.0-only */
/* Copyright (c) 2026 Derek Reynolds */

#include "tray.h"

#include <algorithm>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>

#include "resource.h"

namespace deskhop {

namespace {

constexpr UINT kIconId = 1;
constexpr UINT kIdStatus = 1;
constexpr UINT kIdAutostart = 2;
constexpr UINT kIdQuit = 4;
constexpr UINT kIdFileSummary = 5;
constexpr UINT kIdAcceptFiles = 6;
constexpr UINT kIdDeclineFiles = 7;
constexpr UINT kIdProgress = 8;
constexpr UINT kIdAbortTransfer = 9;
constexpr UINT kIdAbortSend = 10;
constexpr UINT kIdVersion = 11;

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

/* The tray's icon size at this DPI: 16 px at 100 %, 24 at 150 %. The manifest
   makes this process DPI-aware, so the device caps report the real value. */
int small_icon_size() {
    HDC screen = GetDC(nullptr);
    const int dpi = screen ? GetDeviceCaps(screen, LOGPIXELSX) : 96;
    if (screen) ReleaseDC(nullptr, screen);
    return MulDiv(16, dpi > 0 ? dpi : 96, 96);
}

} // namespace

Tray::~Tray() {
    detach();
    for (HICON &look : looks_)
        if (look) DestroyIcon(look);
}

void Tray::attach(HWND window, Callbacks callbacks) {
    window_ = window;
    callbacks_ = std::move(callbacks);
    add_icon();
    update();
}

void Tray::detach() {
    /* Removed explicitly on the way out. An icon left behind is an orphan the
       user cannot get rid of without hovering over it. */
    remove_icon();
    if (digits_) DestroyIcon(digits_);
    digits_ = nullptr;
    if (window_) KillTimer(window_, kPromoteTimerId);
    window_ = nullptr;
}

void Tray::on_timer() { promote(); }

void Tray::add_icon() {
    if (icon_shown_ || !window_) return;
    NOTIFYICONDATAW data = base(window_);
    data.uFlags = NIF_ICON | NIF_MESSAGE;
    data.uCallbackMessage = kCallbackMessage;
    data.hIcon = icon_for(words::look(state_, have_question_));
    icon_shown_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
    /* Once per run: the record outlives an Explorer restart, which re-adds
       the icon through here. */
    if (icon_shown_ && !promoted_) promote();
}

void Tray::remove_icon() {
    if (!icon_shown_ || !window_) return;
    NOTIFYICONDATAW data = base(window_);
    Shell_NotifyIconW(NIM_DELETE, &data);
    icon_shown_ = false;
}

HICON Tray::icon_for(words::Look look) {
    /* words::Look order, which is the order of looks_. */
    static constexpr WORD ids[3] = {IDI_PAIRED, IDI_OFF, IDI_ATTENTION};
    static_assert(static_cast<int>(words::Look::Paired) == 0 &&
                      static_cast<int>(words::Look::Off) == 1 &&
                      static_cast<int>(words::Look::Attention) == 2,
                  "ids[] and looks_[] are indexed by words::Look");
    HICON &slot = looks_[static_cast<int>(look)];
    if (!slot) {
        const int size = small_icon_size();
        slot = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
                                             MAKEINTRESOURCEW(ids[static_cast<int>(look)]),
                                             IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
        /* A build without the resources still gets an icon, just not ours. */
        if (!slot) slot = LoadIconW(nullptr, IDI_APPLICATION);
    }
    return slot;
}

/*
 * The percent as two digits in place of the glyph, for as long as a file is
 * arriving — a tray icon has no text beside it, so the number goes inside.
 * White on a blue tile, like the app icon: bare blue digits vanished on a
 * dark-blue taskbar (#208's desk check), and the tile is the same colour
 * whatever the taskbar is. No "%": two digits fill 16 px on their own.
 * GDI+ rather than GDI: text drawn with GDI into a 32-bit bitmap leaves the
 * alpha at zero, and the icon comes out as a black square. The codec was
 * started by the clipboard, before this is ever called.
 */
HICON Tray::digits(unsigned percent) {
    const int size = small_icon_size();
    Gdiplus::Bitmap bitmap(size, size, PixelFormat32bppARGB);
    if (bitmap.GetLastStatus() != Gdiplus::Ok) return nullptr;
    Gdiplus::Graphics canvas(&bitmap);
    canvas.Clear(Gdiplus::Color(0, 0, 0, 0));
    canvas.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    canvas.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);

    /* The tile: a rounded square, `blue` in helpers/icon/main.swift. */
    const Gdiplus::REAL extent = static_cast<Gdiplus::REAL>(size);
    const Gdiplus::REAL corner = extent * 0.44f; /* the arc's box: twice the radius */
    const Gdiplus::REAL edge = extent - corner; /* windows.h defines `far` */
    Gdiplus::GraphicsPath tile;
    tile.AddArc(0.0f, 0.0f, corner, corner, 180.0f, 90.0f);
    tile.AddArc(edge, 0.0f, corner, corner, 270.0f, 90.0f);
    tile.AddArc(edge, edge, corner, corner, 0.0f, 90.0f);
    tile.AddArc(0.0f, edge, corner, corner, 90.0f, 90.0f);
    tile.CloseFigure();
    Gdiplus::SolidBrush fill(Gdiplus::Color(255, 46, 112, 235));
    canvas.FillPath(&fill, &tile);
    Gdiplus::FontFamily family(L"Segoe UI");
    Gdiplus::Font font(&family, static_cast<Gdiplus::REAL>(size) * 0.65f, Gdiplus::FontStyleBold,
                       Gdiplus::UnitPixel);
    if (font.GetLastStatus() != Gdiplus::Ok) return nullptr;
    /* Typographic: the generic format pads each side, and two digits at this
       size have no room to give. */
    Gdiplus::StringFormat format(Gdiplus::StringFormat::GenericTypographic());
    format.SetAlignment(Gdiplus::StringAlignmentCenter);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, 255, 255, 255));
    const std::wstring text = std::to_wstring(percent > 99u ? 99u : percent);
    canvas.DrawString(text.c_str(), -1, &font, Gdiplus::RectF(0, 0, extent, extent), &format,
                      &brush);
    HICON icon = nullptr;
    return bitmap.GetHICON(&icon) == Gdiplus::Ok ? icon : nullptr;
}

void Tray::update() {
    if (!icon_shown_ || !window_) return;
    NOTIFYICONDATAW data = base(window_);
    data.uFlags = NIF_ICON | NIF_TIP;
    /* Digits while a file arrives, unless a question is waiting — the
       question's badge outranks it, as it does on the Mac. The previous digit
       icon goes only after the shell has copied the new one: a handle per
       half second is how a helper runs out of GDI objects in an afternoon. */
    HICON fresh = nullptr;
    if (progress_total_ > 0 && !have_question_)
        fresh = digits(static_cast<unsigned>(progress_received_ * 100u / progress_total_));
    data.hIcon = fresh ? fresh : icon_for(words::look(state_, have_question_));
    copy_into(data.szTip, sizeof(data.szTip) / sizeof(wchar_t),
              widen(words::tooltip(state_, have_question_ ? summary(question_) : std::string(),
                                   progress_received_, progress_total_, sending_)));
    Shell_NotifyIconW(NIM_MODIFY, &data);
    if (digits_) DestroyIcon(digits_);
    digits_ = fresh;
}

/*
 * Windows 11 starts every new icon in the taskbar overflow, behind the
 * chevron, where a badge nobody sees is a question nobody answers (#208).
 *
 * The per-icon choice lives under HKCU\Control Panel\NotifyIconSettings,
 * one subkey per icon Explorer has met, holding the exe path it came from and
 * an IsPromoted flag. Undocumented, but per-user — no administrator, and no
 * more than the autostart Run key already writes (ADR-0006 is about installs).
 * Set on every start, so a moved exe heals itself. Where the key is absent,
 * nothing breaks, and the README's one-time Settings toggle still works.
 *
 * Explorer writes the subkey some time after the first NIM_ADD, so a try that
 * finds nothing arms a timer and tries again two seconds later, a few times,
 * then gives up for this run. A timer rather than the next update: updates
 * cluster in the first second after start and can then go quiet for hours.
 */
void Tray::promote() {
    /* A WM_TIMER already queued when detach killed the timer still arrives;
       with no window there is nothing to promote and no window to re-arm a
       timer on — SetTimer with a null window would make one nobody stops. */
    if (!window_) return;
    ++promote_attempts_;
    KillTimer(window_, kPromoteTimerId);
    wchar_t self[MAX_PATH];
    const DWORD length = GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return;

    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ,
                      &root) == ERROR_SUCCESS) {
        for (DWORD i = 0; !promoted_; ++i) {
            wchar_t name[256];
            DWORD name_length = 256;
            if (RegEnumKeyExW(root, i, name, &name_length, nullptr, nullptr, nullptr, nullptr) !=
                ERROR_SUCCESS)
                break;
            HKEY entry = nullptr;
            if (RegOpenKeyExW(root, name, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &entry) !=
                ERROR_SUCCESS)
                continue;
            /* One slot spare, so a value stored without its terminator still
               ends. */
            wchar_t path[MAX_PATH + 1]{};
            DWORD bytes = sizeof(wchar_t) * MAX_PATH;
            DWORD type = 0;
            if (RegQueryValueExW(entry, L"ExecutablePath", nullptr, &type,
                                 reinterpret_cast<BYTE *>(path), &bytes) == ERROR_SUCCESS &&
                type == REG_SZ && _wcsicmp(path, self) == 0) {
                const DWORD one = 1;
                promoted_ = RegSetValueExW(entry, L"IsPromoted", 0, REG_DWORD,
                                           reinterpret_cast<const BYTE *>(&one),
                                           sizeof one) == ERROR_SUCCESS;
            }
            RegCloseKey(entry);
        }
        RegCloseKey(root);
    }

    if (promoted_) {
        if (callbacks_.log) callbacks_.log("tray icon promoted to the taskbar");
    } else if (promote_attempts_ < kPromoteAttempts) {
        SetTimer(window_, kPromoteTimerId, kPromoteRetryMs, nullptr);
    } else if (callbacks_.log) {
        callbacks_.log("could not promote the tray icon to the taskbar; it may sit in the "
                       "overflow until it is turned on in Settings (see the README)");
    }
}

void Tray::balloon(const std::string &message) {
    if (!icon_shown_ || !window_) return;
    NOTIFYICONDATAW data = base(window_);
    data.uFlags = NIF_INFO;
    data.dwInfoFlags = NIIF_WARNING;
    copy_into(data.szInfoTitle, sizeof(data.szInfoTitle) / sizeof(wchar_t), L"DeskHopPlus");
    copy_into(data.szInfo, sizeof(data.szInfo) / sizeof(wchar_t), widen(message));
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Tray::show(dh_helper_state state) {
    const bool changed = state != state_;
    state_ = state;
    if (changed) announced_ = false;

    update();

    if (!announced_ && words::state_names_a_remedy(state_)) {
        announced_ = true;
        balloon(words::state_message(state_));
    }
}

void Tray::show_sending(bool sending) {
    if (sending == sending_) return;
    sending_ = sending;
    update();
}

void Tray::on_callback(LPARAM what) {
    const UINT event = LOWORD(what);

    /*
     * Clicking the balloon *is* the acceptance (#56).
     *
     * The shell sends NIN_BALLOONUSERCLICK when the user clicks the notification
     * body rather than dismissing it. Without this the balloon was only an
     * announcement: it had to be read, dismissed, and then the icon found and a
     * menu opened, which is three gestures for one decision and is how a
     * transfer gets forgotten between the reading and the clicking.
     *
     * A real toast with Accept and Decline buttons needs the WinRT notification
     * API and a registered AppUserModelID, which means something installed —
     * and ADR-0006 says this helper installs nothing. One click on the balloon
     * is what a shell notification can carry, so it carries the answer the user
     * is far more likely to want; Decline stays on the menu, where an answer
     * that costs nothing to delay belongs.
     */
    if (event == NIN_BALLOONUSERCLICK) {
        if (!have_question_) return;
        const uint32_t id = question_.id;
        have_question_ = false;
        update();
        if (callbacks_.accept_files) callbacks_.accept_files(id);
        return;
    }

    if (event == WM_RBUTTONUP || event == WM_LBUTTONUP || event == WM_CONTEXTMENU) show_menu();
}

void Tray::show_menu() {
    if (!window_) return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    AppendMenuW(menu, MF_STRING | MF_GRAYED, kIdVersion, widen(words::release_row()).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

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
        shown_percent_ = static_cast<int>(progress_received_ * 100u / progress_total_);
        shown_total_ = progress_total_;
        AppendMenuW(menu, MF_STRING | MF_GRAYED, kIdProgress,
                    widen(words::progress_row(progress_received_, progress_total_)).c_str());
        AppendMenuW(menu, MF_STRING, kIdAbortTransfer, L"Cancel this transfer");
    }

    if (callbacks_.is_sending && callbacks_.is_sending()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kIdAbortSend, L"Cancel what is being sent");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    const bool enabled = callbacks_.autostart_enabled && callbacks_.autostart_enabled();
    AppendMenuW(menu, MF_STRING | (enabled ? MF_CHECKED : 0u), kIdAutostart, L"Start at logon");

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kIdQuit, L"Quit DeskHopPlus Helper");

    POINT where{};
    GetCursorPos(&where);
    /* Required for the menu to dismiss when the user clicks elsewhere — a
       tray menu on a window that is not foreground otherwise stays up. */
    SetForegroundWindow(window_);
    open_menu_ = menu;
    const UINT chosen = static_cast<UINT>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON, where.x, where.y, 0, window_,
        nullptr));
    open_menu_ = nullptr;
    DestroyMenu(menu);

    if (chosen == kIdAutostart && callbacks_.toggle_autostart) callbacks_.toggle_autostart();
    else if (chosen == kIdQuit && callbacks_.quit) callbacks_.quit();
    else if (chosen == kIdAcceptFiles && have_question_) {
        const uint32_t id = question_.id;
        have_question_ = false;
        update();
        if (callbacks_.accept_files) callbacks_.accept_files(id);
    } else if (chosen == kIdDeclineFiles && have_question_) {
        const uint32_t id = question_.id;
        have_question_ = false;
        update();
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
    update();
    balloon("Files from the other computer: " + summary(offer) +
            " Click this to accept, or use the DeskHopPlus icon to decline.");
}

void Tray::withdraw_file_question(uint32_t id) {
    if (!have_question_ || question_.id != id) return;
    have_question_ = false;
    update();
}

void Tray::show_progress(uint64_t received, uint64_t total) {
    progress_received_ = received;
    progress_total_ = total;
    update();
    refresh_open_menu();
}

/*
 * Rewrites the progress row of a menu the user has open. The WM_TIMER tick
 * keeps running under TrackPopupMenu (#262), so this is reached while the menu
 * is up. The item changes, but its window does not repaint on its own, so the
 * row alone is invalidated, without erasing: repainting the whole menu, or
 * erasing first, flickers. Only when the percent moves: the size text alone
 * changes every kilobyte early on, and each repaint shows as a flicker. A
 * transfer that ends under the open menu says so and its Cancel greys out; one
 * that starts again under it turns Cancel back on.
 *
 * ponytail: the menu's width is fixed when it opens, so a row that grows
 * (a longer size or a third percent digit) can be clipped; owner-draw if seen.
 */
void Tray::refresh_open_menu() {
    if (!open_menu_) return;
    const bool ended = progress_total_ == 0;
    const int percent = ended ? -1 : static_cast<int>(progress_received_ * 100u / progress_total_);
    /* The total as well: a new transfer can start at the percent the last
       one showed, within one tick and with no end seen between them. */
    if (percent == shown_percent_ && progress_total_ == shown_total_) return;
    const std::wstring row = ended ? L"No longer receiving"
                                   : widen(words::progress_row(progress_received_, progress_total_));
    int position = -1;
    for (int i = 0, n = GetMenuItemCount(open_menu_); i < n && position < 0; ++i)
        if (GetMenuItemID(open_menu_, i) == kIdProgress) position = i;
    if (position < 0) return;
    MENUITEMINFOW info{};
    info.cbSize = sizeof info;
    info.fMask = MIIM_STRING;
    info.dwTypeData = const_cast<wchar_t *>(row.c_str());
    if (!SetMenuItemInfoW(open_menu_, kIdProgress, FALSE, &info)) return;
    shown_percent_ = percent;
    shown_total_ = progress_total_;
    /* Back on for a transfer that starts under the same open menu. */
    EnableMenuItem(open_menu_, kIdAbortTransfer, MF_BYCOMMAND | (ended ? MF_GRAYED : MF_ENABLED));
    /* "#32768" is the popup-menu window class. Only this thread's are asked,
       so a hung program's menu can never block the helper, and MN_GETHMENU
       picks ours among them. */
    for (HWND popup = FindWindowW(L"#32768", nullptr); popup;
         popup = FindWindowExW(nullptr, popup, L"#32768", nullptr)) {
        if (GetWindowThreadProcessId(popup, nullptr) != GetCurrentThreadId()) continue;
        if (reinterpret_cast<HMENU>(SendMessageW(popup, MN_GETHMENU, 0, 0)) != open_menu_) continue;
        /* The Cancel row right below as well, since it greys and ungreys. */
        RECT rows{};
        if (GetMenuItemRect(nullptr, open_menu_, static_cast<UINT>(position), &rows)) {
            RECT below{};
            if (GetMenuItemRect(nullptr, open_menu_, static_cast<UINT>(position + 1), &below))
                UnionRect(&rows, &rows, &below);
            MapWindowPoints(nullptr, popup, reinterpret_cast<POINT *>(&rows), 2);
            InvalidateRect(popup, &rows, FALSE);
        }
        break;
    }
}

std::string Tray::summary(const deskhop::FileOffer &offer) {
    const std::string what = offer.files.size() == 1
                                 ? offer.files.front().name
                                 : std::to_string(offer.files.size()) + " files";
    return what + " \xe2\x80\x94 " + words::size_text(offer.total) + ", about " +
           duration_text(offer.estimated_seconds()) + ".";
}

std::string Tray::duration_text(uint32_t seconds) {
    if (seconds < 60u) return std::to_string(seconds < 1u ? 1u : seconds) + " seconds";
    const uint32_t minutes = (seconds + 59u) / 60u;
    return minutes == 1u ? std::string("a minute") : std::to_string(minutes) + " minutes";
}

} // namespace deskhop
