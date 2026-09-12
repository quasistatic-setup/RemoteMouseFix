#include "rmf/MouseHook.h"

#include <atomic>

namespace rmf {
namespace {

// The callback signature is fixed by Windows, so the collaborators live at file scope.
// Only one MouseHook is ever installed.
MouseHook::Wiring g_wiring {};
bool              g_wired = false;

std::atomic<std::uint64_t> g_sequence  {0};
std::atomic<std::uint64_t> g_totalSeen {0};
std::atomic<std::uint64_t> g_skipped   {0};

// Last pointer position reported by the remote client. Hook thread only.
struct RemoteTracking {
    POINT         last       {0, 0};
    bool          valid      = false;
    std::uint32_t generation = 0;
};
RemoteTracking g_tracking;

EventKind KindFromMessage(WPARAM message) {
    switch (message) {
        case WM_MOUSEMOVE:     return EventKind::Move;
        case WM_LBUTTONDOWN:   return EventKind::LButtonDown;
        case WM_LBUTTONUP:     return EventKind::LButtonUp;
        case WM_RBUTTONDOWN:   return EventKind::RButtonDown;
        case WM_RBUTTONUP:     return EventKind::RButtonUp;
        case WM_MBUTTONDOWN:   return EventKind::MButtonDown;
        case WM_MBUTTONUP:     return EventKind::MButtonUp;
        case WM_XBUTTONDOWN:   return EventKind::XButtonDown;
        case WM_XBUTTONUP:     return EventKind::XButtonUp;
        case WM_MOUSEWHEEL:    return EventKind::Wheel;
        case WM_MOUSEHWHEEL:   return EventKind::HWheel;
        default:               return EventKind::Unknown;
    }
}

} // namespace

LRESULT CALLBACK MouseHook::HookProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode != HC_ACTION || !g_wired || lParam == 0) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }
    g_totalSeen.fetch_add(1, std::memory_order_relaxed);

    const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    Filter&                filter     = *g_wiring.filter;
    CorrectionState* const correction = g_wiring.correction;

    // Own output is counted before any scoping, so the echo watchdog stays exact even when
    // the focus changes between sending and observing.
    const bool own = info->dwExtraInfo == kOwnInputSignature;
    if (own && correction) {
        correction->ownSeen.fetch_add(1, std::memory_order_relaxed);
        correction->pendingEcho.fetch_sub(1, std::memory_order_relaxed);
    }

    // Scope first: targetPid is published by the state sampler and is non-zero only while
    // the target owns the foreground, so desktop input never enters our memory.
    const DWORD targetPid = filter.targetPid.load(std::memory_order_relaxed);
    const HWND  foreground = (targetPid != 0) ? GetForegroundWindow() : nullptr;
    DWORD foregroundPid = 0;
    if (foreground) {
        GetWindowThreadProcessId(foreground, &foregroundPid);
    }
    if (targetPid == 0 || foregroundPid != targetPid) {
        // A remote position from before leaving the target must never be diffed against
        // one from after coming back.
        g_tracking.valid = false;
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    const EventKind kind     = KindFromMessage(wParam);
    const bool      injected = (info->flags & LLMHF_INJECTED) != 0;

    // The game hides the cursor exactly while it holds the mouse for camera control
    // (findings 2026-09-12, Befund 1). That is the only window in which the remote
    // client's absolute position fights the game.
    CURSORINFO cursorInfo {};
    cursorInfo.cbSize = sizeof(cursorInfo);
    const bool cursorHidden = GetCursorInfo(&cursorInfo) &&
                              (cursorInfo.flags & CURSOR_SHOWING) == 0;

    CorrectionMode mode = CorrectionMode::Off;
    if (correction && correction->allowed) {
        mode = correction->Mode();
        const std::uint32_t generation = correction->generation.load(std::memory_order_relaxed);
        if (generation != g_tracking.generation) {
            g_tracking.generation = generation;
            g_tracking.valid      = false;
        }
    }

    bool suppress      = false;
    bool trackingReset = false;
    long remoteDx      = 0;
    long remoteDy      = 0;

    // Only injected moves from someone else carry the remote pointer position. Button
    // events are deliberately not tracked: while the game holds the cursor their reported
    // position is the anchor, not the remote pointer, and would fake a delta.
    if (kind == EventKind::Move && injected && !own) {
        if (mode != CorrectionMode::Off && cursorHidden) {
            if (g_tracking.valid) {
                remoteDx = info->pt.x - g_tracking.last.x;
                remoteDy = info->pt.y - g_tracking.last.y;
            } else {
                trackingReset = true; // no reference yet: withhold, move nothing
            }
            suppress = true;
        }
        g_tracking.last  = info->pt;
        g_tracking.valid = true;
    }

    if (suppress) {
        correction->suppressed.fetch_add(1, std::memory_order_relaxed);
        if (remoteDx != 0 || remoteDy != 0) {
            const BOOL posted = PostThreadMessageW(
                g_wiring.applyThread, g_wiring.applyMessage,
                static_cast<WPARAM>(static_cast<LONG_PTR>(remoteDx)),
                static_cast<LPARAM>(static_cast<LONG_PTR>(remoteDy)));
            if (!posted) {
                correction->outputFailures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    const bool logThis = !filter.paused.load(std::memory_order_relaxed) &&
                         (kind != EventKind::Move ||
                          filter.logMouseMoves.load(std::memory_order_relaxed));
    if (logThis) {
        RawEvent ev;
        ev.kind           = kind;
        ev.pt             = info->pt;
        ev.mouseData      = info->mouseData;
        ev.hookFlags      = info->flags;
        ev.hookTimeMs     = info->time;
        ev.extraInfo      = info->dwExtraInfo;
        ev.foreground     = foreground;
        ev.foregroundPid  = foregroundPid;
        ev.cursorHidden   = cursorHidden;
        ev.ownInput       = own;
        ev.correctionMode = mode;
        ev.suppressed     = suppress;
        ev.trackingReset  = trackingReset;
        ev.remoteDx       = remoteDx;
        ev.remoteDy       = remoteDy;
        ev.anchorValid    = filter.anchorValid.load(std::memory_order_relaxed);
        ev.anchorOffset.x = filter.anchorOffsetX.load(std::memory_order_relaxed);
        ev.anchorOffset.y = filter.anchorOffsetY.load(std::memory_order_relaxed);
        ev.seq            = g_sequence.fetch_add(1, std::memory_order_relaxed);

        LARGE_INTEGER qpc {};
        QueryPerformanceCounter(&qpc);
        ev.qpcTicks = qpc.QuadPart;

        // Two win32k calls, no messages sent to the game.
        RECT client {};
        if (GetClientRect(foreground, &client)) {
            POINT topLeft     {client.left,  client.top};
            POINT bottomRight {client.right, client.bottom};
            if (ClientToScreen(foreground, &topLeft) && ClientToScreen(foreground, &bottomRight)) {
                ev.clientScreenRect = RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
                ev.haveGeometry     = true;
            }
        }

        g_wiring.queue->Push(ev); // drops on overflow rather than blocking the input pipeline
    }

    // Non-zero stops the event here: it reaches neither later hooks nor the game.
    return suppress ? 1 : CallNextHookEx(nullptr, nCode, wParam, lParam);
}

bool MouseHook::Install(const Wiring& wiring, DWORD& outLastError) {
    if (hook_) {
        return true;
    }
    if (!wiring.queue || !wiring.filter) {
        outLastError = ERROR_INVALID_PARAMETER;
        return false;
    }
    g_wiring = wiring;
    g_wired  = true;

    // A global WH_MOUSE_LL hook needs no module handle and no injection: the callback runs
    // in this process on the installing thread, which is why no DLL is shipped.
    hook_ = SetWindowsHookExW(WH_MOUSE_LL, &MouseHook::HookProc, nullptr, 0);
    if (!hook_) {
        outLastError = GetLastError();
        g_wired  = false;
        g_wiring = Wiring{};
        return false;
    }
    outLastError = 0;
    return true;
}

void MouseHook::Uninstall() {
    if (hook_) {
        UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    g_wired  = false;
    g_wiring = Wiring{};
}

bool MouseHook::IsInstalled() const {
    return hook_ != nullptr;
}

std::uint64_t MouseHook::TotalSeen() {
    return g_totalSeen.load(std::memory_order_relaxed);
}

std::uint64_t MouseHook::FastPathSkipped() {
    return g_skipped.load(std::memory_order_relaxed);
}

} // namespace rmf
