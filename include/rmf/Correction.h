#pragma once

#include <windows.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>

namespace rmf {

// How a remote pointer movement is handed to the game once the original injected
// absolute event has been withheld. docs/findings-2026-09-12.md explains why the
// absolute event has to be withheld at all: it overwrites the game's own cursor warp.
//
// Both output modes go through SendInput on purpose. A SetCursorPos variant was measured
// against MouseLookProbe and lost about 40% of the movement: SetCursorPos races with the
// input thread that is still finishing the withheld event and gets reverted. SendInput is
// queued behind that event and processed in order.
enum class CorrectionMode : std::uint8_t {
    Off      = 0, // observe only, every event passes untouched
    Absolute = 1, // absolute SendInput to current + delta: exact pixels, no acceleration
    Relative = 2, // relative SendInput: also suits Raw Input readers, but is accelerated
};

const wchar_t* CorrectionModeName(CorrectionMode mode);
bool ParseCorrectionMode(const std::wstring& text, CorrectionMode& out);

// Written into dwExtraInfo of every event this tool synthesises, so the hook recognises
// its own output and never feeds it back into the correction. ASCII "RMFX".
constexpr ULONG_PTR kOwnInputSignature = 0x524D4658;

// Shared between the hook callback, the message loop, the sampler and the log writer.
// Everything the hook reads is an atomic because the hook must not take a lock.
struct CorrectionState {
    // Written once before the hook is installed, read-only afterwards. False while
    // diagnostic_mode is on, or when the emergency-off hotkey could not be registered:
    // then no mode can be enabled at all.
    bool allowed = false;

    std::atomic<std::uint8_t>  mode       {0};
    // Bumped on every mode change. The hook drops its remote-pointer tracking when it
    // sees a new generation, so a stale position never turns into a delta.
    std::atomic<std::uint32_t> generation {0};

    std::atomic<std::uint64_t> suppressed     {0}; // injected moves withheld from the game
    std::atomic<std::uint64_t> applied        {0}; // deltas successfully handed on
    std::atomic<std::uint64_t> ownSeen        {0}; // own synthetic events observed by the hook
    std::atomic<std::uint64_t> outputFailures {0}; // SendInput or post failures
    std::atomic<std::uint64_t> watchdogTrips  {0};
    // Own events sent but not yet seen by the hook. Growth without
    // bound means the input is discarded before it reaches the hook, typically because
    // the game runs at a higher integrity level and UIPI blocks it silently.
    std::atomic<std::int64_t>  pendingEcho    {0};

    CorrectionMode Mode() const {
        return static_cast<CorrectionMode>(mode.load(std::memory_order_relaxed));
    }

    // Returns the previous mode. Call from the message-loop thread.
    CorrectionMode SetMode(CorrectionMode next);

    // Switches to Off only if a mode was active; returns the mode that was switched off,
    // or Off when nothing was active. Safe from any thread, which is what the watchdogs
    // need.
    CorrectionMode ForceOff();
};

// Hands a remote pointer delta to the game. Runs on the message-loop thread, never
// inside the hook callback. `outTarget` receives the intended landing position in absolute
// mode (the current position otherwise). Returns false when the Win32 call failed.
bool ApplyRemoteDelta(CorrectionMode mode, long dx, long dy, POINT& outTarget);

// Learns the game's warp anchor from cursor positions sampled while the cursor is hidden.
// The game keeps pulling the pointer back to one point, so that point dominates the
// samples; positions caught between two warps are scattered noise.
class AnchorLearner {
public:
    void Add(POINT screenPos);

    // Evaluates and clears the collected phase. True when one position clearly dominates:
    // at least kMinSamples hits and kMinSharePercent of all samples.
    bool Finish(POINT& outPos, unsigned& outHits, unsigned& outTotal);

private:
    static constexpr unsigned    kMinSamples      = 5;
    static constexpr unsigned    kMinSharePercent = 20;
    static constexpr std::size_t kMaxDistinct     = 4096;

    std::map<std::pair<long, long>, unsigned> counts_;
    unsigned total_ = 0;
};

} // namespace rmf
