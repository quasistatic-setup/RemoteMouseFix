#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace rmf {

struct Config;

enum class EventKind : std::uint8_t {
    Move,
    LButtonDown, LButtonUp,
    RButtonDown, RButtonUp,
    MButtonDown, MButtonUp,
    XButtonDown, XButtonUp,
    Wheel, HWheel,
    Unknown
};

const wchar_t* EventKindName(EventKind k);
bool IsButtonEvent(EventKind k);

// One raw low-level mouse event, captured inside the hook callback.
//
// The hook callback runs on the message thread that installed the hook and is subject
// to the LowLevelHooksTimeout: if it blocks, Windows silently drops the hook. So this
// struct holds only data that is cheap to obtain (no cross-process queries, no I/O)
// and is handed to the writer thread through a lock-free ring.
struct RawEvent {
    EventKind kind = EventKind::Unknown;

    POINT  pt            {0, 0};  // physical screen pixels, straight from MSLLHOOKSTRUCT
    DWORD  mouseData     = 0;     // wheel delta in the high word / XBUTTON index
    DWORD  hookFlags     = 0;     // LLMHF_INJECTED, LLMHF_LOWER_IL_INJECTED
    DWORD  hookTimeMs    = 0;     // MSLLHOOKSTRUCT.time, the message tick count
    ULONG_PTR extraInfo  = 0;     // dwExtraInfo, the usual signature slot for synthetic input

    HWND   foreground    = nullptr;
    DWORD  foregroundPid = 0;

    // Foreground window geometry, sampled in the hook via cheap local calls. Needed to
    // express the event in client coordinates and relative to the client centre, which
    // is what a cursor recenter would target.
    RECT   clientScreenRect {0, 0, 0, 0}; // client area, mapped to screen coordinates
    bool   haveGeometry  = false;

    std::uint64_t seq    = 0;     // monotonic sequence number assigned in the hook
    std::int64_t  qpcTicks = 0;   // QueryPerformanceCounter at capture time
};

// Per-event derived values plus the resolved process identity, produced by the writer
// thread. Kept separate from RawEvent so nothing expensive happens inside the hook.
struct DecoratedEvent {
    const RawEvent* raw = nullptr;

    long dx = 0, dy = 0;          // delta to the previous logged position
    long dtMs = 0;                // hookTimeMs delta to the previous event, -1 if unknown
    POINT client {0, 0};          // pt in target client coordinates
    long centreDx = 0, centreDy = 0; // offset from client centre
    bool haveClient = false;

    bool injected      = false;
    bool lowerIlInjected = false;
    bool looksLikeRecenter = false; // injected + lands on client centre within tolerance
    bool looksLikeJump     = false; // |dx| or |dy| at/above jumpThresholdPx

    std::wstring processName;     // file name only, e.g. Wow.exe
};

// Renders one decorated event as a single fixed-column log line (no trailing newline).
std::wstring FormatEventLine(const DecoratedEvent& ev, std::int64_t qpcFreq,
                             std::int64_t qpcStart, const FILETIME& wallStart);

} // namespace rmf
