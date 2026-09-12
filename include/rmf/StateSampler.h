#pragma once

#include <windows.h>
#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace rmf {

// Polls the pieces of mouse state that are not visible in the low-level hook stream and
// reports transitions.
//
// This is what makes a cursor recenter provable rather than guessed:
//  * ClipCursor confines the pointer to a rectangle. A game that captures the mouse for
//    camera control typically clips it, often to a 1x1 rect around the window centre.
//  * The cursor is hidden while the camera is being dragged.
//  * SetCapture ownership and the foreground window can change under us.
// None of these produce hook events, so they are sampled on a timer instead.
class StateSampler {
public:
    struct Snapshot {
        bool  cursorVisible   = true;
        RECT  clipRect        {0, 0, 0, 0};
        bool  clipIsFullScreen = true;
        HWND  foreground      = nullptr;
        DWORD foregroundPid   = 0;
        RECT  windowRect      {0, 0, 0, 0};
        RECT  clientScreenRect{0, 0, 0, 0};
        UINT  dpi             = 0;
        POINT cursorPos       {0, 0};

        bool DiffersFrom(const Snapshot& other) const;
    };

    // `onChange` is called from the sampler thread whenever the snapshot changes in a
    // way worth logging. It must be safe to call concurrently with the writer thread.
    using ChangeHandler = std::function<void(const Snapshot& prev, const Snapshot& now)>;

    // `onSample` is called on every poll, after `onChange`. Used for work that needs
    // every sample rather than transitions only: anchor learning and watchdog timing.
    using SampleHandler = std::function<void(const Snapshot& now)>;

    void Start(unsigned intervalMs, ChangeHandler onChange, SampleHandler onSample = {});
    void Stop();

    Snapshot Current() const;

private:
    void Run(unsigned intervalMs);

    std::thread       thread_;
    std::atomic<bool> running_ {false};
    ChangeHandler     onChange_;
    SampleHandler     onSample_;

    mutable CRITICAL_SECTION lock_ {};
    bool      lockInit_ = false;
    Snapshot  current_ {};
};

// Renders a state snapshot difference as a log line.
std::wstring FormatStateLine(const StateSampler::Snapshot& prev,
                             const StateSampler::Snapshot& now,
                             const std::wstring& fgProcessName);

} // namespace rmf
