// The console panel the player actually looks at while playing.
//
// The old single status line listed raw counters: whoever ran the tool had to know what
// "corr=absolute ev=27 inj=27 apply=51 wd=1" means before they could tell working from
// broken. This renders the same state as three traffic-light lines plus one line that
// says what to do next, and it repaints in place so the panel stays at the bottom of the
// window instead of scrolling away.
//
// Colour and repainting need a real console with ANSI support; when output is redirected
// to a file, or when NO_COLOR is set, the view degrades to plain lines that are printed
// only when the state actually changes.
#pragma once

#include "rmf/Correction.h"

#include <windows.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace rmf {

// Writes text to the console, Unicode intact, with a UTF-8 fallback when the handle is a
// file or a pipe. Every console write in the program goes through this.
void ConsoleWrite(const std::wstring& text);

// Where the game window is, from the player's point of view.
enum class GameState : std::uint8_t {
    NotRunning, // the target process is not in the process list at all
    Background, // running, but another window owns the foreground
    Foreground, // the target holds the foreground window: the fix can do its work
};

enum class NoticeLevel : std::uint8_t { Info, Good, Warn, Error };

// Everything the panel shows. Assembled by the status thread from the shared atomics.
struct StatusSnapshot {
    std::wstring   targetProcess;
    GameState      game              = GameState::NotRunning;
    bool           loggingPaused     = false;
    bool           correctionAllowed = false;
    CorrectionMode mode              = CorrectionMode::Off;

    // Plain-language reason why no correction can run at all this session, e.g. the
    // diagnostic profile. Empty when a mode may be enabled.
    std::wstring lockReason;
    // Set once a watchdog switched the correction off. This is the state the player used
    // to miss entirely: the tool kept running and kept looking busy while it had already
    // stopped correcting.
    std::wstring safetyStopReason;

    unsigned long long events      = 0;
    unsigned long long jumps       = 0;
    unsigned long long corrections = 0;
    unsigned long long problems    = 0;
};

class StatusView {
public:
    // Detects console capabilities and prints the panel for the first time. `enabled`
    // false keeps the object usable for notices without ever drawing a panel.
    void Start(bool enabled, const StatusSnapshot& initial);

    // Repaints with fresh numbers. Safe to call from the status thread only; notices from
    // other threads are serialised against it.
    void Update(const StatusSnapshot& snapshot);

    // A one-off message that scrolls above the panel and stays there, e.g. a mode change
    // or a watchdog trip.
    void Notice(NoticeLevel level, const std::wstring& text);

    // Leaves the last panel on screen and puts the cursor below it, so shutdown output
    // does not overwrite it.
    void Stop();

private:
    struct Line {
        const wchar_t* colour; // ANSI sequence or an empty string when colour is off
        std::wstring   text;
    };

    // All three assume the caller holds mutex_.
    std::vector<Line> Render() const;
    void              Draw();       // repaint, or print once if the state changed
    void              Repaint();    // in place, needs cursor control
    void              PrintIfChanged(); // fallback for a redirected or plain console
    void              ErasePanel();

    mutable std::mutex mutex_;
    StatusSnapshot     snapshot_;
    std::wstring       lastPlainSummary_;
    unsigned           linesDrawn_ = 0;
    bool               enabled_    = false;
    bool               repaint_    = false; // console can move the cursor back up
    bool               colour_     = false;
    bool               started_    = false;
};

} // namespace rmf
