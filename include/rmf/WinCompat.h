// Thin wrappers around Windows APIs that are version dependent or that we want to
// resolve dynamically, so the EXE keeps loading on plain Windows 10 builds.
#pragma once

#include <windows.h>
#include <string>

namespace rmf {

// Opts the process into Per-Monitor-V2 DPI awareness when available, falling back to
// Per-Monitor V1 and then System DPI awareness. Must be called before any window or
// cursor coordinate is read.
//
// Why this matters here: WH_MOUSE_LL reports *physical* screen pixels. If the process
// were DPI-virtualised, every window/client rectangle we compare against would be
// scaled differently from the hook coordinates and the logged offsets would be wrong.
//
// Returns a human readable description of the mode that was actually established.
std::wstring InitDpiAwareness();

// DPI of the monitor hosting `hwnd`, or the system DPI if GetDpiForWindow is missing.
// Returns 0 when no DPI could be determined.
UINT GetWindowDpi(HWND hwnd);

// Full image path of `pid` using QueryFullProcessImageNameW with
// PROCESS_QUERY_LIMITED_INFORMATION, which works for same-user processes without
// elevation. `outAccessDenied` is set when the process exists but cannot be opened,
// which almost always means it runs at a higher integrity level than this tool.
bool QueryProcessImagePath(DWORD pid, std::wstring& outPath, bool& outAccessDenied);

// True when the current process runs elevated.
bool IsProcessElevated();

// Directory containing the running EXE, without a trailing separator.
std::wstring GetExecutableDirectory();

} // namespace rmf
