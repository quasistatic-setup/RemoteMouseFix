#include "rmf/MouseHook.h"

#include <atomic>

namespace rmf {
namespace {

// The callback signature is fixed by Windows, so the hook's collaborators live at file
// scope. Only one MouseHook instance is ever installed.
MouseEventQueue*     g_queue  = nullptr;
MouseHook::Filter*   g_filter = nullptr;

std::atomic<std::uint64_t> g_sequence  {0};
std::atomic<std::uint64_t> g_totalSeen {0};
std::atomic<std::uint64_t> g_skipped   {0};

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
    // Phase 1 contract: whatever happens below, the event is passed on untouched.
    // There is no code path in this build that returns a non-zero value or that calls
    // SendInput / mouse_event / SetCursorPos.
    if (nCode != HC_ACTION || !g_queue || !g_filter) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    g_totalSeen.fetch_add(1, std::memory_order_relaxed);

    if (g_filter->paused.load(std::memory_order_relaxed)) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    // Fast path first, so desktop input outside the target window never enters our
    // memory at all. targetPid is published by the state sampler and is non-zero only
    // while the target process owns the foreground window.
    const DWORD targetPid = g_filter->targetPid.load(std::memory_order_relaxed);
    if (targetPid == 0) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    const EventKind kind = KindFromMessage(wParam);
    if (kind == EventKind::Move && !g_filter->logMouseMoves.load(std::memory_order_relaxed)) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    const HWND foreground = GetForegroundWindow();
    DWORD foregroundPid = 0;
    if (foreground) {
        GetWindowThreadProcessId(foreground, &foregroundPid);
    }
    if (foregroundPid != targetPid) {
        g_skipped.fetch_add(1, std::memory_order_relaxed);
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
    if (!info) {
        return CallNextHookEx(nullptr, nCode, wParam, lParam);
    }

    RawEvent ev;
    ev.kind          = kind;
    ev.pt            = info->pt;
    ev.mouseData     = info->mouseData;
    ev.hookFlags     = info->flags;
    ev.hookTimeMs    = info->time;
    ev.extraInfo     = info->dwExtraInfo;
    ev.foreground    = foreground;
    ev.foregroundPid = foregroundPid;
    ev.seq           = g_sequence.fetch_add(1, std::memory_order_relaxed);

    LARGE_INTEGER qpc {};
    QueryPerformanceCounter(&qpc);
    ev.qpcTicks = qpc.QuadPart;

    // Client geometry of the target window. Two win32k calls, no messages sent to the
    // game, so this stays well inside the LowLevelHooksTimeout budget.
    RECT client {};
    if (GetClientRect(foreground, &client)) {
        POINT topLeft     {client.left,  client.top};
        POINT bottomRight {client.right, client.bottom};
        if (ClientToScreen(foreground, &topLeft) && ClientToScreen(foreground, &bottomRight)) {
            ev.clientScreenRect = RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
            ev.haveGeometry     = true;
        }
    }

    g_queue->Push(ev); // drops on overflow rather than blocking the input pipeline

    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

bool MouseHook::Install(MouseEventQueue& queue, Filter& filter, DWORD& outLastError) {
    if (hook_) {
        return true;
    }
    g_queue  = &queue;
    g_filter = &filter;

    // A global WH_MOUSE_LL hook needs no module handle and no injection: the callback
    // runs in this process on the installing thread, which is why no DLL is shipped.
    hook_ = SetWindowsHookExW(WH_MOUSE_LL, &MouseHook::HookProc, nullptr, 0);
    if (!hook_) {
        outLastError = GetLastError();
        g_queue  = nullptr;
        g_filter = nullptr;
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
    g_queue  = nullptr;
    g_filter = nullptr;
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
