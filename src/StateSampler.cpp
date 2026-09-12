#include "rmf/StateSampler.h"
#include "rmf/WinCompat.h"

#include <cstdarg>
#include <cwchar>

namespace rmf {
namespace {

bool RectsEqual(const RECT& a, const RECT& b) {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

void AppendFormatted(std::wstring& out, const wchar_t* format, ...) {
    wchar_t buffer[512];
    va_list args;
    va_start(args, format);
    const int written = vswprintf(buffer, 512, format, args);
    va_end(args);
    if (written > 0) {
        out.append(buffer, static_cast<std::size_t>(written));
    }
}

std::wstring RectToString(const RECT& r) {
    std::wstring out;
    AppendFormatted(out, L"(%ld,%ld)-(%ld,%ld) %ldx%ld",
                    r.left, r.top, r.right, r.bottom, r.right - r.left, r.bottom - r.top);
    return out;
}

} // namespace

bool StateSampler::Snapshot::DiffersFrom(const Snapshot& other) const {
    // Deliberately excludes cursorPos: the pointer moves constantly and that stream is
    // already covered by the hook. Only state transitions belong in the state log.
    return cursorVisible    != other.cursorVisible ||
           clipIsFullScreen != other.clipIsFullScreen ||
           !RectsEqual(clipRect, other.clipRect) ||
           foreground       != other.foreground ||
           foregroundPid    != other.foregroundPid ||
           !RectsEqual(windowRect, other.windowRect) ||
           !RectsEqual(clientScreenRect, other.clientScreenRect) ||
           dpi              != other.dpi;
}

void StateSampler::Start(unsigned intervalMs, ChangeHandler onChange, SampleHandler onSample) {
    if (running_.load()) {
        return;
    }
    if (!lockInit_) {
        InitializeCriticalSection(&lock_);
        lockInit_ = true;
    }
    onChange_ = std::move(onChange);
    onSample_ = std::move(onSample);
    running_.store(true);
    thread_ = std::thread(&StateSampler::Run, this, intervalMs == 0 ? 10u : intervalMs);
}

void StateSampler::Stop() {
    running_.store(false);
    if (thread_.joinable()) {
        thread_.join();
    }
    if (lockInit_) {
        DeleteCriticalSection(&lock_);
        lockInit_ = false;
    }
}

StateSampler::Snapshot StateSampler::Current() const {
    if (!lockInit_) {
        return Snapshot{};
    }
    EnterCriticalSection(&lock_);
    const Snapshot copy = current_;
    LeaveCriticalSection(&lock_);
    return copy;
}

void StateSampler::Run(unsigned intervalMs) {
    // Virtual screen bounds, used to tell "clipped to a region" from "not clipped".
    const int vsX = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vsY = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vsW = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vsH = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    Snapshot previous {};
    bool havePrevious = false;

    while (running_.load(std::memory_order_relaxed)) {
        Snapshot now {};

        // Cursor visibility and position. CURSORINFO is the only way to observe the
        // hide that a game performs while it holds the mouse for camera control, and
        // that hide never shows up as a hook event.
        CURSORINFO ci {};
        ci.cbSize = sizeof(ci);
        if (GetCursorInfo(&ci)) {
            now.cursorVisible = (ci.flags & CURSOR_SHOWING) != 0;
            now.cursorPos     = ci.ptScreenPos;
        } else {
            GetCursorPos(&now.cursorPos);
        }

        // ClipCursor is the other half of a capture: a confined pointer is the direct
        // fingerprint of a game recentring the cursor inside a small rectangle.
        if (GetClipCursor(&now.clipRect)) {
            now.clipIsFullScreen = (now.clipRect.left   <= vsX &&
                                    now.clipRect.top    <= vsY &&
                                    now.clipRect.right  >= vsX + vsW &&
                                    now.clipRect.bottom >= vsY + vsH);
        }

        now.foreground = GetForegroundWindow();
        if (now.foreground) {
            GetWindowThreadProcessId(now.foreground, &now.foregroundPid);
            GetWindowRect(now.foreground, &now.windowRect);

            RECT client {};
            if (GetClientRect(now.foreground, &client)) {
                POINT topLeft     {client.left,  client.top};
                POINT bottomRight {client.right, client.bottom};
                if (ClientToScreen(now.foreground, &topLeft) &&
                    ClientToScreen(now.foreground, &bottomRight)) {
                    now.clientScreenRect = RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y};
                }
            }
            now.dpi = GetWindowDpi(now.foreground);
        }

        if (lockInit_) {
            EnterCriticalSection(&lock_);
            current_ = now;
            LeaveCriticalSection(&lock_);
        }

        if (onChange_ && (!havePrevious || now.DiffersFrom(previous))) {
            onChange_(havePrevious ? previous : now, now);
        }
        if (onSample_) {
            onSample_(now);
        }
        previous     = now;
        havePrevious = true;

        Sleep(intervalMs);
    }
}

std::wstring FormatStateLine(const StateSampler::Snapshot& prev,
                             const StateSampler::Snapshot& now,
                             const std::wstring& fgProcessName) {
    std::wstring line = L"## STATE ";

    if (now.foreground != prev.foreground || now.foregroundPid != prev.foregroundPid) {
        AppendFormatted(line, L"fg=0x%p pid=%lu %ls | ",
                        static_cast<void*>(now.foreground), now.foregroundPid,
                        fgProcessName.c_str());
    }

    if (now.cursorVisible != prev.cursorVisible) {
        AppendFormatted(line, L"cursor=%ls | ", now.cursorVisible ? L"SHOWN" : L"HIDDEN");
    }

    if (now.clipIsFullScreen != prev.clipIsFullScreen || !RectsEqual(now.clipRect, prev.clipRect)) {
        if (now.clipIsFullScreen) {
            line += L"clip=RELEASED | ";
        } else {
            AppendFormatted(line, L"clip=CONFINED %ls | ", RectToString(now.clipRect).c_str());
        }
    }

    if (!RectsEqual(now.clientScreenRect, prev.clientScreenRect)) {
        AppendFormatted(line, L"client=%ls | ", RectToString(now.clientScreenRect).c_str());
    }

    if (now.dpi != prev.dpi) {
        AppendFormatted(line, L"dpi=%u | ", now.dpi);
    }

    // Always carry the current pointer position: it is the anchor for reading the
    // surrounding hook lines.
    AppendFormatted(line, L"cursor_at=(%ld,%ld)", now.cursorPos.x, now.cursorPos.y);
    return line;
}

} // namespace rmf
