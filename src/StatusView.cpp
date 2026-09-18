#include "rmf/StatusView.h"

#include <string>

#ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#endif

namespace rmf {
namespace {

// Kept as narrow constants so a build without colour simply substitutes an empty string.
constexpr const wchar_t* kGreen  = L"\x1b[32m";
constexpr const wchar_t* kYellow = L"\x1b[33m";
constexpr const wchar_t* kRed    = L"\x1b[31m";
constexpr const wchar_t* kGrey   = L"\x1b[90m";
constexpr const wchar_t* kReset  = L"\x1b[0m";
constexpr const wchar_t* kNone   = L"";

std::wstring Count(unsigned long long value, const wchar_t* singular, const wchar_t* plural) {
    return std::to_wstring(value) + L" " + (value == 1 ? singular : plural);
}

bool NoColorRequested() {
    // https://no-color.org: any non-empty value disables colour.
    return GetEnvironmentVariableW(L"NO_COLOR", nullptr, 0) > 0;
}

} // namespace

void ConsoleWrite(const std::wstring& text) {
    if (text.empty()) {
        return;
    }
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE || out == nullptr) {
        return;
    }
    DWORD ignored = 0;
    // WriteConsoleW keeps Unicode intact regardless of the console code page.
    if (WriteConsoleW(out, text.c_str(), static_cast<DWORD>(text.size()), &ignored, nullptr)) {
        return;
    }
    // Redirected to a file or pipe: fall back to UTF-8 bytes.
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                          nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) {
        return;
    }
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), utf8.data(), bytes,
                        nullptr, nullptr);
    WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &ignored, nullptr);
}

void StatusView::Start(bool enabled, const StatusSnapshot& initial) {
    std::lock_guard<std::mutex> guard(mutex_);
    snapshot_ = initial;

    HANDLE out  = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode = 0;
    bool   vt   = false;
    if (out != INVALID_HANDLE_VALUE && out != nullptr && GetConsoleMode(out, &mode) != 0) {
        // Repainting in place needs the cursor sequences. Without them the panel would
        // scroll a new copy of itself into the log every two seconds.
        vt = SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }

    enabled_ = enabled;
    repaint_ = enabled && vt;
    colour_  = vt && !NoColorRequested();
    started_ = true;

    Draw();
}

void StatusView::Update(const StatusSnapshot& snapshot) {
    std::lock_guard<std::mutex> guard(mutex_);
    snapshot_ = snapshot;
    Draw();
}

void StatusView::Draw() {
    if (!enabled_ || !started_) {
        return;
    }
    if (repaint_) {
        Repaint();
        return;
    }
    PrintIfChanged();
}

void StatusView::PrintIfChanged() {
    // No cursor control, so every draw would scroll another copy into a redirected log.
    // Print the panel only when the situation itself changed; growing counters alone are
    // not a change worth a new block.
    const std::vector<Line> lines   = Render();
    std::wstring            summary = lines.empty() ? std::wstring() : lines.front().text;
    if (lines.size() > 1) {
        summary += L" | " + lines[1].text;
    }
    if (summary == lastPlainSummary_) {
        return;
    }
    lastPlainSummary_ = summary;
    for (const Line& line : lines) {
        ConsoleWrite(line.text + L"\r\n");
    }
}

void StatusView::Notice(NoticeLevel level, const std::wstring& text) {
    std::lock_guard<std::mutex> guard(mutex_);

    ErasePanel();

    const wchar_t* colour = kNone;
    const wchar_t* prefix = L"   ";
    if (level == NoticeLevel::Good) {
        colour = colour_ ? kGreen : kNone;
        prefix = L"   ";
    } else if (level == NoticeLevel::Warn) {
        colour = colour_ ? kYellow : kNone;
        prefix = L"!  ";
    } else if (level == NoticeLevel::Error) {
        colour = colour_ ? kRed : kNone;
        prefix = L"!! ";
    }

    ConsoleWrite(std::wstring(colour) + L"  " + prefix + text + (colour_ ? kReset : kNone) + L"\r\n");

    Draw();
}

void StatusView::Stop() {
    std::lock_guard<std::mutex> guard(mutex_);
    Draw();
    started_    = false;
    enabled_    = false;
    linesDrawn_ = 0;
}

void StatusView::ErasePanel() {
    if (!repaint_ || linesDrawn_ == 0) {
        return;
    }
    // Back to the top of the panel, then clear everything below it.
    ConsoleWrite(L"\x1b[" + std::to_wstring(linesDrawn_) + L"F\x1b[0J");
    linesDrawn_ = 0;
}

void StatusView::Repaint() {
    const std::vector<Line> lines = Render();

    std::wstring out;
    if (repaint_ && linesDrawn_ > 0) {
        out += L"\x1b[" + std::to_wstring(linesDrawn_) + L"F";
    }
    for (const Line& line : lines) {
        if (repaint_) {
            out += L"\x1b[2K"; // the previous, possibly longer, text must go
        }
        if (colour_) {
            out += line.colour;
        }
        out += line.text;
        if (colour_) {
            out += kReset;
        }
        out += L"\r\n";
    }
    linesDrawn_ = static_cast<unsigned>(lines.size());
    ConsoleWrite(out);
}

std::vector<StatusView::Line> StatusView::Render() const {
    const wchar_t* green  = colour_ ? kGreen : kNone;
    const wchar_t* yellow = colour_ ? kYellow : kNone;
    const wchar_t* red    = colour_ ? kRed : kNone;
    const wchar_t* grey   = colour_ ? kGrey : kNone;

    const std::wstring game = snapshot_.targetProcess.empty() ? L"the game" : snapshot_.targetProcess;
    const bool inFront      = snapshot_.game == GameState::Foreground;
    const bool stopped      = !snapshot_.safetyStopReason.empty();
    const bool locked       = !snapshot_.lockReason.empty();
    const bool correcting   = snapshot_.mode != CorrectionMode::Off;

    std::vector<Line> lines;

    // ---- the game window ----------------------------------------------------------
    switch (snapshot_.game) {
        case GameState::NotRunning:
            lines.push_back({yellow, L"  GAME  o  " + game + L" is not running yet"});
            break;
        case GameState::Background:
            lines.push_back({yellow, L"  GAME  o  " + game + L" is running, but another window is in front"});
            break;
        case GameState::Foreground:
            lines.push_back({green, L"  GAME  *  " + game + L" is in front"});
            break;
    }

    // ---- the fix itself -----------------------------------------------------------
    const std::wstring modeName = CorrectionModeName(snapshot_.mode);
    if (stopped) {
        lines.push_back({red, L"  FIX   !  switched itself off: " + snapshot_.safetyStopReason});
    } else if (locked) {
        lines.push_back({grey, L"  FIX   -  off, recording only: " + snapshot_.lockReason});
    } else if (!correcting) {
        lines.push_back({yellow, L"  FIX   o  standby - press Ctrl+Alt+1 to switch it on"});
    } else if (!inFront) {
        lines.push_back({yellow, L"  FIX   o  ready (" + modeName + L") - it works while the game is in front"});
    } else {
        lines.push_back({green, L"  FIX   *  on (" + modeName + L") - correcting the pointer"});
    }

    // ---- what it has done so far ---------------------------------------------------
    std::wstring work = L"  WORK     ";
    work += locked ? Count(snapshot_.events, L"event", L"events") + L" recorded, " +
                         std::to_wstring(snapshot_.jumps) + L" of them jumpy"
                   : Count(snapshot_.jumps, L"jump", L"jumps") + L" seen, " +
                         Count(snapshot_.corrections, L"correction", L"corrections") + L" applied";
    if (snapshot_.problems > 0) {
        work += L", " + Count(snapshot_.problems, L"problem", L"problems");
    }
    if (snapshot_.loggingPaused) {
        work += L"  (logging paused)";
    }
    lines.push_back({snapshot_.problems > 0 ? yellow : grey, work});

    // ---- the one line that says what to do next -------------------------------------
    std::wstring    hint;
    const wchar_t*  hintColour = grey;
    if (stopped) {
        hint       = L"Close this window and start the fix again. If it keeps happening, send the log.";
        hintColour = red;
    } else if (locked) {
        hint = L"Nothing is being changed. Start-AB-Test.cmd is the profile that corrects.";
    } else if (snapshot_.game == GameState::NotRunning) {
        hint = L"Start the game now. This panel follows along.";
    } else if (!inFront) {
        hint = L"Click into the game window; the top line turns green.";
    } else if (!correcting) {
        hint = L"Press Ctrl+Alt+1 to switch the fix on.";
    } else if (snapshot_.corrections == 0) {
        hint = L"Hold the right mouse button and look around. Corrections start counting.";
    } else {
        hint = L"Everything is working. Ctrl+Alt+C switches the fix off at any time.";
    }
    lines.push_back({hintColour, L"  NEXT  >  " + hint});

    lines.push_back({grey, snapshot_.correctionAllowed
                               ? L"  KEYS     Ctrl+Alt+1 fix on | Ctrl+Alt+C fix off | "
                                 L"Ctrl+Alt+P pause log | Ctrl+Alt+Q quit"
                               : L"  KEYS     Ctrl+Alt+M mark the log | Ctrl+Alt+P pause log | "
                                 L"Ctrl+Alt+Q quit"});

    return lines;
}

} // namespace rmf
