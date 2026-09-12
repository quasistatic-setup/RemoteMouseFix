#pragma once

#include "rmf/Correction.h"
#include "rmf/EventQueue.h"

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace rmf {

// Installs the WH_MOUSE_LL hook, feeds captured events into a queue and, while a
// correction mode is active, withholds the remote client's absolute moves.
//
// The only event the hook ever withholds is an injected WM_MOUSEMOVE without this tool's
// own signature, arriving while the target window is in front and the cursor is hidden.
// Button, wheel and physical events always pass. The replacement movement is not
// synthesised here but posted to the message loop, so the callback stays far inside the
// LowLevelHooksTimeout budget.
class MouseHook {
public:
    struct Filter {
        std::atomic<DWORD> targetPid     {0};     // 0 = target not in front, nothing happens
        std::atomic<bool>  logMouseMoves {true};
        std::atomic<bool>  paused        {false}; // pauses logging only, never the correction

        // Learned warp anchor, as an offset inside the target client area so it survives
        // the window being moved.
        std::atomic<bool>  anchorValid   {false};
        std::atomic<long>  anchorOffsetX {0};
        std::atomic<long>  anchorOffsetY {0};
    };

    struct Wiring {
        MouseEventQueue* queue        = nullptr;
        Filter*          filter       = nullptr;
        CorrectionState* correction   = nullptr;
        DWORD            applyThread  = 0; // thread that runs ApplyRemoteDelta
        UINT             applyMessage = 0; // posted with wParam = dx, lParam = dy
    };

    // Installs the hook on the calling thread, which must run a message loop.
    bool Install(const Wiring& wiring, DWORD& outLastError);
    void Uninstall();
    bool IsInstalled() const;

    // Events seen by the callback, and those outside the target window.
    static std::uint64_t TotalSeen();
    static std::uint64_t FastPathSkipped();

private:
    static LRESULT CALLBACK HookProc(int nCode, WPARAM wParam, LPARAM lParam);

    HHOOK hook_ = nullptr;
};

} // namespace rmf
