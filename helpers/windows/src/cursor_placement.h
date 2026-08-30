#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "dh_place.h"

namespace deskhop {

class CursorPlacement {
  public:
    CursorPlacement(std::function<void(const std::string &)> log,
                    std::function<void(uint8_t, const std::vector<uint8_t> &)> respond);
    ~CursorPlacement();

    bool received(uint8_t type, const uint8_t *body, size_t len, uint32_t now_ms);

  private:
    struct Pending {
        dh_place place{};
        uint32_t expires_at{0};
    };

    static void CALLBACK foreground_changed(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
    bool place(const dh_place &request, uint32_t now_ms, bool may_defer);
    void answer_pending_query();
    bool target(const dh_place &request, POINT &point);
    bool position_body(std::vector<uint8_t> &body);
    bool secure_desktop_active() const;
    bool foreground_is_higher_integrity() const;
    static DWORD process_integrity(HANDLE process);

    static CursorPlacement *instance_;
    std::function<void(const std::string &)> log_;
    std::function<void(uint8_t, const std::vector<uint8_t> &)> respond_;
    HWINEVENTHOOK foreground_hook_{nullptr};
    std::optional<Pending> pending_;
    bool query_pending_{false};
    std::optional<uint8_t> last_chain_direction_;
};

} // namespace deskhop
