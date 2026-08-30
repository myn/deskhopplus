#include "cursor_placement.h"

#include <algorithm>
#include <vector>

#include "dh_frame.h"

namespace deskhop {

CursorPlacement *CursorPlacement::instance_ = nullptr;

CursorPlacement::CursorPlacement(
    std::function<void(const std::string &)> log,
    std::function<void(uint8_t, const std::vector<uint8_t> &)> respond)
    : log_(std::move(log)), respond_(std::move(respond)) {
    instance_ = this;
    foreground_hook_ = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
                                        &CursorPlacement::foreground_changed, 0, 0,
                                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

CursorPlacement::~CursorPlacement() {
    if (foreground_hook_) UnhookWinEvent(foreground_hook_);
    if (instance_ == this) instance_ = nullptr;
}

bool CursorPlacement::received(uint8_t type, const uint8_t *body, size_t len, uint32_t now_ms) {
    if (type == DH_MSG_POS_QUERY) {
        if (len != 0) {
            log_("ignored a malformed cursor position query");
            return true;
        }
        if (pending_) {
            query_pending_ = true;
        } else {
            std::vector<uint8_t> response;
            if (position_body(response)) respond_(DH_MSG_POS_RESPONSE, response);
        }
        return true;
    }
    if (type != DH_MSG_PLACE) return false;
    dh_place request{};
    if (!dh_place_decode(body, len, &request)) {
        log_("ignored a malformed cursor placement");
        return true;
    }
    last_chain_direction_ = request.chain_direction;
    (void)place(request, now_ms, true);
    return true;
}

void CALLBACK CursorPlacement::foreground_changed(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD,
                                                   DWORD) {
    CursorPlacement *self = instance_;
    if (!self || !self->pending_) return;
    const uint32_t now = static_cast<uint32_t>(GetTickCount64());
    if (static_cast<int32_t>(now - self->pending_->expires_at) >= 0) {
        self->log_("deferred cursor placement expired");
        self->pending_.reset();
        self->query_pending_ = false;
        return;
    }
    const dh_place request = self->pending_->place;
    self->pending_.reset();
    if (self->place(request, now, false)) self->answer_pending_query();
}

bool CursorPlacement::place(const dh_place &request, uint32_t now_ms, bool may_defer) {
    if (secure_desktop_active()) {
        log_("cursor placement abandoned while the secure desktop is active");
        return false;
    }
    if (foreground_is_higher_integrity()) {
        if (may_defer) {
            pending_ = Pending{request, now_ms + 500u};
            log_("cursor placement deferred while a higher-integrity window has focus");
        }
        return false;
    }

    POINT wanted{};
    if (!target(request, wanted)) {
        log_("cursor placement named a monitor outside the configured display chain");
        return false;
    }
    SetLastError(0);
    if (!SetCursorPos(wanted.x, wanted.y)) {
        const DWORD error = GetLastError();
        log_("cursor placement was refused (error " + std::to_string(error) + ")");
        if (may_defer) pending_ = Pending{request, now_ms + 500u};
        return false;
    }
    POINT actual{};
    if (!GetCursorPos(&actual) || actual.x != wanted.x || actual.y != wanted.y) {
        log_("cursor placement was clamped or did not reach the requested position");
        return false;
    }
    pending_.reset();
    return true;
}

void CursorPlacement::answer_pending_query() {
    if (!query_pending_) return;
    query_pending_ = false;
    std::vector<uint8_t> response;
    if (position_body(response)) respond_(DH_MSG_POS_RESPONSE, response);
}

namespace {
struct DisplayList {
    std::vector<dh_display_rect> rects;
    std::vector<HMONITOR> handles;
};

BOOL CALLBACK collect_monitor(HMONITOR monitor, HDC, LPRECT rect, LPARAM context) {
    auto *list = reinterpret_cast<DisplayList *>(context);
    list->handles.push_back(monitor);
    list->rects.push_back({rect->left, rect->top, rect->right - rect->left,
                           rect->bottom - rect->top});
    return TRUE;
}

bool display_snapshot(DisplayList &displays, size_t &primary_index) {
    if (!EnumDisplayMonitors(nullptr, nullptr, collect_monitor,
                             reinterpret_cast<LPARAM>(&displays)) || displays.rects.empty())
        return false;
    const POINT origin{0, 0};
    const HMONITOR primary = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    const auto it = std::find(displays.handles.begin(), displays.handles.end(), primary);
    primary_index = it == displays.handles.end()
                        ? 0u
                        : static_cast<size_t>(it - displays.handles.begin());
    return true;
}
} // namespace

bool CursorPlacement::target(const dh_place &request, POINT &point) {
    DisplayList displays;
    size_t primary_index = 0;
    if (!display_snapshot(displays, primary_index)) return false;
    dh_place_point target_point{};
    if (!dh_place_target(&request, displays.rects.data(), displays.rects.size(), primary_index,
                         &target_point))
        return false;
    point.x = target_point.x;
    point.y = target_point.y;
    return true;
}

bool CursorPlacement::position_body(std::vector<uint8_t> &body) {
    if (!last_chain_direction_) return false;
    DisplayList displays;
    size_t primary_index = 0;
    if (!display_snapshot(displays, primary_index)) return false;
    POINT cursor{};
    if (!GetCursorPos(&cursor)) return false;
    const HMONITOR current = MonitorFromPoint(cursor, MONITOR_DEFAULTTONULL);
    const auto current_it = std::find(displays.handles.begin(), displays.handles.end(), current);
    if (current_it == displays.handles.end()) return false;
    const size_t current_index = static_cast<size_t>(current_it - displays.handles.begin());
    uint8_t chain_index = 0;
    for (size_t index = 1; index <= std::min<size_t>(displays.rects.size(), 255); index++) {
        dh_place request{static_cast<uint8_t>(index), *last_chain_direction_, 4, 0};
        dh_place_point point{};
        if (dh_place_target(&request, displays.rects.data(), displays.rects.size(), primary_index,
                            &point) && point.display_index == current_index) {
            chain_index = static_cast<uint8_t>(index);
            break;
        }
    }
    if (chain_index == 0) return false;
    const dh_display_rect &rect = displays.rects[current_index];
    const auto normalized = [](LONG value, int32_t start, int32_t size) -> uint16_t {
        if (size <= 1) return 0;
        const int64_t offset = std::clamp<int64_t>((int64_t)value - start, 0, size - 1);
        return static_cast<uint16_t>((offset * 65535 + (size - 1) / 2) / (size - 1));
    };
    const dh_position position{chain_index, normalized(cursor.x, rect.x, rect.width),
                               normalized(cursor.y, rect.y, rect.height)};
    body.resize(DH_POSITION_BODY_SIZE);
    return dh_position_encode(&position, body.data(), body.size());
}

bool CursorPlacement::secure_desktop_active() const {
    HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    if (!desktop) return true;
    CloseDesktop(desktop);
    return false;
}

DWORD CursorPlacement::process_integrity(HANDLE process) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return 0;
    DWORD size = 0;
    GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    std::vector<uint8_t> storage(size);
    DWORD rid = 0;
    if (size && GetTokenInformation(token, TokenIntegrityLevel, storage.data(), size, &size)) {
        const auto *label = reinterpret_cast<const TOKEN_MANDATORY_LABEL *>(storage.data());
        PSID sid = label->Label.Sid;
        rid = *GetSidSubAuthority(sid, static_cast<DWORD>(*GetSidSubAuthorityCount(sid) - 1));
    }
    CloseHandle(token);
    return rid;
}

bool CursorPlacement::foreground_is_higher_integrity() const {
    HWND foreground = GetForegroundWindow();
    if (!foreground) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(foreground, &pid);
    HANDLE foreground_process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!foreground_process) return true;
    const DWORD foreground_integrity = process_integrity(foreground_process);
    CloseHandle(foreground_process);
    const DWORD own_integrity = process_integrity(GetCurrentProcess());
    return foreground_integrity == 0 || own_integrity == 0 || foreground_integrity > own_integrity;
}

} // namespace deskhop
