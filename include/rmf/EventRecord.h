#pragma once

#include "rmf/Correction.h"

#include <windows.h>

#include <cstdint>
#include <string>

namespace rmf {

enum class EventKind : std::uint8_t {
    Move,
    LButtonDown, LButtonUp,
    RButtonDown, RButtonUp,
    MButtonDown, MButtonUp,
    XButtonDown, XButtonUp,
    Wheel, HWheel,
    Apply,   // not a hook event: one correction output, recorded by the message loop
    Unknown
};

const wchar_t* EventKindName(EventKind k);
bool IsButtonEvent(EventKind k);

// One raw low-level mouse event, captured inside the hook callback.
//
// The hook callback is subject to the LowLevelHooksTimeout: if it blocks, Windows
// silently drops the hook. So this struct holds only data that is cheap to obtain and is
// handed to the writer thread through a lock-free ring.
struct RawEvent {
    EventKind kind = EventKind::Unknown;

    POINT     pt         {0, 0}; // physical screen pixels, straight from MSLLHOOKSTRUCT
    DWORD     mouseData  = 0;    // wheel delta in the high word / XBUTTON index
    DWORD     hookFlags  = 0;    // LLMHF_INJECTED, LLMHF_LOWER_IL_INJECTED
    DWORD     hookTimeMs = 0;    // MSLLHOOKSTRUCT.time, the message tick count
    ULONG_PTR extraInfo  = 0;    // dwExtraInfo, the signature slot for synthetic input

    HWND  foreground    = nullptr;
    DWORD foregroundPid = 0;

    RECT clientScreenRect {0, 0, 0, 0}; // target client area in screen coordinates
    bool haveGeometry     = false;

    bool cursorHidden = false; // CURSOR_SHOWING clear at capture: the game holds the mouse
    bool ownInput     = false; // carries kOwnInputSignature, produced by this tool

    // Correction outcome, decided inside the hook.
    CorrectionMode correctionMode = CorrectionMode::Off; // mode in effect at capture
    bool suppressed    = false; // withheld from the game
    bool trackingReset = false; // first remote move after a reset, delta forced to zero
    long remoteDx      = 0;     // remote pointer delta handed to the correction
    long remoteDy      = 0;

    bool  anchorValid  = false; // a learned warp anchor was known at capture
    POINT anchorOffset {0, 0};  // that anchor inside the client area

    // Apply records only: cursor position read right before the output call; pt holds the
    // position read right after it. Shows whether the output actually landed.
    POINT applyBefore {0, 0};
    POINT applyTarget {0, 0}; // intended landing position (absolute mode)
    bool  applyOk     = false;
    DWORD applyError  = 0;

    std::uint64_t seq      = 0; // monotonic sequence number assigned in the hook
    std::int64_t  qpcTicks = 0; // QueryPerformanceCounter at capture time
};

// Per-event derived values plus the resolved process identity, produced by the writer
// thread so nothing expensive happens inside the hook.
struct DecoratedEvent {
    const RawEvent* raw = nullptr;

    long  dx = 0, dy = 0;             // delta to the previous logged position
    long  dtMs = 0;                   // hookTimeMs delta to the previous event, -1 if unknown
    POINT client {0, 0};              // pt in target client coordinates
    long  centreDx = 0, centreDy = 0; // offset from the client centre
    bool  haveClient = false;

    // Offset from the learned anchor while the cursor is hidden. This is the quantity the
    // game reads as camera movement.
    long anchorDx = 0, anchorDy = 0;
    bool haveAnchor = false;

    bool injected        = false;
    bool lowerIlInjected = false;
    bool atAnchor        = false; // a real, not withheld, move within tolerance of the anchor
    bool looksLikeJump   = false; // |dx| or |dy| at/above jumpThresholdPx

    std::wstring processName; // file name only, e.g. Wow.exe
};

// Renders one Apply record (kind == Apply) as a log line.
std::wstring FormatApplyLine(const RawEvent& raw, std::int64_t qpcFreq,
                             std::int64_t qpcStart, const FILETIME& wallStart);

// Renders one decorated event as a single fixed-column log line (no trailing newline).
std::wstring FormatEventLine(const DecoratedEvent& ev, std::int64_t qpcFreq,
                             std::int64_t qpcStart, const FILETIME& wallStart);

} // namespace rmf
