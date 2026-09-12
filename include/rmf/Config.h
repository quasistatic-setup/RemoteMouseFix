#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace rmf {

// Runtime configuration. Defaults are the Phase 1 diagnostic defaults; config.json
// only needs to override what differs.
struct Config {
    // --- required keys from the project brief ---
    std::wstring targetProcess     = L"Wow.exe"; // matched case-insensitively against the file name
    bool         diagnosticMode    = true;       // Phase 1 must stay true: observe only, never modify input
    bool         logMouseMoves     = true;       // false logs buttons/wheel/state only

    // --- logging ---
    std::wstring  logDirectory     = L"logs";    // relative paths resolve next to the EXE
    std::wstring  logFilePrefix    = L"remotemousefix";
    std::uint64_t maxLogBytes      = 8ull * 1024 * 1024; // rotate past this size
    unsigned      maxLogFiles      = 5;          // keep at most this many files, oldest deleted
    bool          flushOnButton    = true;       // flush on button events so a crash keeps them

    // --- diagnostics tuning ---
    int      jumpThresholdPx          = 40;   // tag a move as JUMP at or above this delta
    int      recenterTolerancePx      = 3;    // injected move landing this close to client centre => RECENTER
    unsigned statePollIntervalMs      = 10;   // cursor clip/visibility sampler period
    bool     logStateChanges          = true;
    unsigned consoleStatusIntervalMs  = 2000;  // 0 disables the live console status line
    unsigned heartbeatIntervalMs      = 10000; // periodic liveness line, 0 disables it

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
