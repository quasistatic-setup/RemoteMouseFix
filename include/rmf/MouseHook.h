#pragma once

#include "rmf/EventQueue.h"

#include <windows.h>
#include <atomic>

namespace rmf {

// Installs the WH_MOUSE_LL hook and feeds raw events into a queue.
//
// Phase 1 contract: the callback returns CallNextHookEx unconditionally. It never
// swallows, rewrites, delays or re-injects an event. There is no SendInput anywhere in
// this build, so the tool cannot alter what the game receives.
class MouseHook {
public:
    // Hook state that the callback reads on every event. Kept as plain atomics because
    // the callback must not take a lock.
    struct Filter {
        std::atomic<DWORD> targetPid       {0};     // 0 = unknown, fast path disabled
        std::atomic<bool>  logMouseMoves   {true};
        std::atomic<bool>  paused          {false}; // operator toggled capture off
    };

    // Installs the hook on the calling thread. That thread must run a message loop,
    // otherwise the callback is never invoked.
    bool Install(MouseEventQueue& queue, Filter& filter, DWORD& outLastError);
    void Uninstall();

    bool IsInstalled() const;

    // Events seen by the callback, including those dropped by the move fast path.
    static std::uint64_t TotalSeen();
    // Events the fast path discarded before they ever reached the queue.
    static std::uint64_t FastPathSkipped();

private:
    static LRESULT CALLBACK HookProc(int nCode, WPARAM wParam, LPARAM lParam);

    HHOOK hook_ = nullptr;
};

} // namespace rmf
