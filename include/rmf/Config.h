#pragma once

#include "rmf/Correction.h"

#include <cstdint>
#include <string>
#include <vector>

namespace rmf {

// Runtime configuration. Defaults are the safe defaults; config.json only needs to
// override what differs.
struct Config {
    // --- core keys from the project brief ---
    std::wstring targetProcess  = L"Wow.exe"; // matched case-insensitively on the file name
    // true: observe only, input is never modified and no correction mode can be enabled.
    // false: correction modes become available; the tool still starts with correction_mode.
    bool         diagnosticMode = true;
    bool         logMouseMoves  = true;       // false logs buttons/wheel/state only

    // --- correction (only effective with diagnostic_mode = false) ---
    CorrectionMode correctionMode          = CorrectionMode::Off; // mode at startup
    unsigned watchdogMaxHiddenMs           = 180000; // hidden cursor longer than this trips; 0 = off
    unsigned watchdogMaxUnechoed           = 50;     // relative mode: own events not seen by the hook
    unsigned watchdogMaxOutputFailures     = 5;      // consecutive failed SendInput calls

    // --- logging ---
    std::wstring  logDirectory  = L"logs";    // relative paths resolve next to the EXE
    std::wstring  logFilePrefix = L"remotemousefix";
    std::uint64_t maxLogBytes   = 8ull * 1024 * 1024; // rotate past this size
    unsigned      maxLogFiles   = 5;          // keep at most this many files, oldest deleted
    bool          flushOnButton = true;       // flush on button events so a crash keeps them

    // --- diagnostics tuning ---
    int      jumpThresholdPx         = 40;    // tag a move as JUMP at or above this delta
    int      anchorTolerancePx       = 3;     // real move this close to the learned anchor => ANCHOR
    unsigned statePollIntervalMs     = 10;    // cursor visibility / clip / anchor sampling period
    bool     logStateChanges         = true;
    unsigned consoleStatusIntervalMs = 2000;  // 0 disables the live console status line
    unsigned heartbeatIntervalMs     = 10000; // periodic liveness line, 0 disables it

    // Non-fatal problems found while reading config.json, reported to console and log.
    std::vector<std::wstring> warnings;
    // Path that was actually read; empty when built-in defaults were used.
    std::wstring loadedFrom;
};

// Reads `path` if it exists. A missing file is not an error: defaults are kept and a
// warning is recorded. Returns false only on a hard parse failure.
bool LoadConfig(const std::wstring& path, Config& cfg);

// Resolves a possibly relative path against the EXE directory.
std::wstring ResolveAgainstExeDir(const std::wstring& path);

} // namespace rmf
