// RemoteMouseFix - diagnostics with an optional, guarded correction.
//
// Observes the low-level mouse stream while a configured target process owns the
// foreground window and writes a human readable trace. With diagnostic_mode = false it
// can additionally correct the conflict measured on 2026-09-12 (docs/findings-2026-09-12.md):
// a remote client's absolute pointer position overwriting the game's cursor warp.
//
// The correction is deliberately narrow. It only ever withholds injected mouse moves from
// someone other than this tool, and only while the target is in front with its cursor
// hidden. It then hands the remote pointer's own movement on via signed SendInput. Buttons, wheel and physical
// input always pass untouched. It starts in the configured mode (off by default), can be
// switched off instantly by hotkey, and watchdogs switch it off on any sign of trouble.
//
// What this tool does not do, in any mode:
//  * no DLL injection, no driver, no code loaded into the game
//  * no ClipCursor, no change to the game or its files
//  * no keyboard hook, so no keystroke or text content can be recorded
//  * no network access of any kind

#include "rmf/Version.h"

#include "rmf/Config.h"
#include "rmf/Correction.h"
#include "rmf/EventQueue.h"
#include "rmf/EventRecord.h"
#include "rmf/Logger.h"
#include "rmf/MouseHook.h"
#include "rmf/ProcessInfo.h"
#include "rmf/RecentWarpAnchor.h"
#include "rmf/StateSampler.h"
#include "rmf/StatusView.h"
#include "rmf/WinCompat.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Hotkey ids. RegisterHotKey is used instead of a keyboard hook on purpose: it can only
// ever observe these specific combinations, so the tool remains incapable of recording
// keystrokes or text.
constexpr int kHotkeyMarker             = 1; // Ctrl+Alt+M
constexpr int kHotkeyPause              = 2; // Ctrl+Alt+P
constexpr int kHotkeyQuit               = 3; // Ctrl+Alt+Q
constexpr int kHotkeyCorrectionOff      = 4; // Ctrl+Alt+C, emergency off
constexpr int kHotkeyCorrectionAbsolute = 5; // Ctrl+Alt+1
constexpr int kHotkeyCorrectionRelative = 6; // Ctrl+Alt+2

struct HotkeySpec {
    int            id;
    UINT           vk;
    const wchar_t* label;
};

constexpr HotkeySpec kHotkeys[] = {
    {kHotkeyMarker,             'M', L"Ctrl+Alt+M marker"},
    {kHotkeyPause,              'P', L"Ctrl+Alt+P pause logging"},
    {kHotkeyQuit,               'Q', L"Ctrl+Alt+Q quit"},
    {kHotkeyCorrectionOff,      'C', L"Ctrl+Alt+C correction off"},
    {kHotkeyCorrectionAbsolute, '1', L"Ctrl+Alt+1 correction absolute"},
    {kHotkeyCorrectionRelative, '2', L"Ctrl+Alt+2 correction relative"},
};

constexpr UINT kMsgQuitRequested = WM_APP + 1;
constexpr UINT kMsgApplyDelta    = WM_APP + 2; // posted by the hook: wParam = dx, lParam = dy
constexpr UINT_PTR kSmoothTimer  = 1;
constexpr UINT kSmoothIntervalMs = 16;
constexpr long kSmoothMaxStepPx  = 20;

std::atomic<bool> g_running {true};
DWORD             g_mainThreadId = 0;

void RequestStop() {
    g_running.store(false);
    if (g_mainThreadId != 0) {
        PostThreadMessageW(g_mainThreadId, kMsgQuitRequested, 0, 0);
    }
}

BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            RequestStop();
            return TRUE;
        default:
            return FALSE;
    }
}

// Plain console output, used for the header and the shutdown summary. While the status
// panel is up, everything goes through StatusView instead, so nothing overwrites it.
void ConsoleOut(const wchar_t* format, ...) {
    wchar_t buffer[1024];
    va_list args;
    va_start(args, format);
    const int written = vswprintf(buffer, 1024, format, args);
    va_end(args);
    if (written <= 0) {
        return;
    }
    rmf::ConsoleWrite(std::wstring(buffer, static_cast<std::size_t>(written)));
}

// A name the caller meant relative to the program, not an absolute or rooted path.
bool IsRelativePath(const std::wstring& path) {
    if (path.size() >= 2 && path[1] == L':') {
        return false;
    }
    return path.empty() || (path[0] != L'\\' && path[0] != L'/');
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring Format(const wchar_t* format, ...) {
    wchar_t buffer[1024];
    va_list args;
    va_start(args, format);
    const int written = vswprintf(buffer, 1024, format, args);
    va_end(args);
    return written > 0 ? std::wstring(buffer, static_cast<std::size_t>(written)) : std::wstring();
}

std::wstring NowStampReadable() {
    SYSTEMTIME st {};
    GetLocalTime(&st);
    return Format(L"%04u-%02u-%02u %02u:%02u:%02u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

std::wstring OsVersionString() {
    // GetVersionEx lies for unmanifested apps; RtlGetVersion is not shimmed.
    using RtlGetVersionFn = LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
        if (rtlGetVersion) {
            RTL_OSVERSIONINFOW info {};
            info.dwOSVersionInfoSize = sizeof(info);
            if (rtlGetVersion(&info) == 0) {
                return Format(L"Windows %lu.%lu build %lu",
                              info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
            }
        }
    }
    return L"Windows (version unavailable)";
}

// Serialises every writer into the log: drain thread, sampler, heartbeat, message loop.
std::mutex        g_logMutex;
rmf::Logger*      g_logger = nullptr;
std::atomic<bool> g_flushPending {false};

void LogLine(const std::wstring& line) {
    std::lock_guard<std::mutex> guard(g_logMutex);
    if (g_logger) {
        g_logger->WriteLine(line);
    }
}

void LogLines(const std::vector<std::wstring>& lines) {
    std::lock_guard<std::mutex> guard(g_logMutex);
    if (!g_logger) {
        return;
    }
    for (const auto& line : lines) {
        g_logger->WriteLine(line);
    }
}

void FlushLog() {
    std::lock_guard<std::mutex> guard(g_logMutex);
    if (g_logger) {
        g_logger->Flush();
    }
}

unsigned long long U(const std::atomic<std::uint64_t>& value) {
    return static_cast<unsigned long long>(value.load(std::memory_order_relaxed));
}

} // namespace

int main() {
    g_mainThreadId = GetCurrentThreadId();

    // DPI awareness before anything reads a coordinate: the hook reports physical pixels,
    // and absolute SendInput must be computed in the same space.
    const std::wstring dpiMode = rmf::InitDpiAwareness();

    SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);
    SetConsoleTitleW(L"RemoteMouseFix " RMF_VERSION_W);

    // ---- command line --------------------------------------------------------------
    std::wstring configPath;
    unsigned runSeconds = 0; // 0 = run until the operator stops the tool
    {
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            for (int i = 1; i < argc; ++i) {
                const std::wstring arg = argv[i];
                if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
                    ConsoleOut(L"RemoteMouseFix %ls - diagnostics with optional correction\n\n"
                               L"Usage: RemoteMouseFix.exe [path\\to\\config.json] [--seconds N]\n\n"
                               L"  --seconds N  stop automatically after N seconds (0 = run until stopped)\n"
                               L"  A/B and Playing targets are read from RemoteMouseFix.ini.\n\n"
                               L"Hotkeys while running:\n"
                               L"  Ctrl+Alt+M   write a marker line into the log\n"
                               L"  Ctrl+Alt+P   pause / resume logging (correction keeps running)\n"
                               L"  Ctrl+Alt+Q   quit (Ctrl+C works too)\n"
                               L"  Ctrl+Alt+C   correction OFF (emergency off)\n"
                               L"  Ctrl+Alt+1   correction absolute\n"
                               L"  Ctrl+Alt+2   correction relative\n\n"
                               L"Correction modes need diagnostic_mode = false in the config.\n",
                               RMF_VERSION_W);
                    LocalFree(argv);
                    return 0;
                }
                if (arg == L"--seconds" && i + 1 < argc) {
                    runSeconds = static_cast<unsigned>(std::max(0L, std::wcstol(argv[++i], nullptr, 10)));
                    continue;
                }
                if (configPath.empty() && !arg.empty() && arg[0] != L'-') {
                    configPath = arg;
                }
            }
            LocalFree(argv);
        }
    }
    // Profiles ship in config\ next to the EXE. A relative path that is not there is
    // looked up in that folder as well, so a folder from an older release, or a launcher
    // that still names config-play.json directly, keeps working instead of silently
    // falling back to the built-in defaults.
    {
        const std::wstring requested = configPath.empty() ? std::wstring(L"config.json") : configPath;
        configPath                   = rmf::ResolveAgainstExeDir(requested);
        if (!FileExists(configPath) && IsRelativePath(requested)) {
            const std::wstring inConfigFolder = rmf::ResolveAgainstExeDir(L"config\\" + requested);
            if (FileExists(inConfigFolder)) {
                configPath = inConfigFolder;
            }
        }
    }

    rmf::Config cfg;
    rmf::LoadConfig(configPath, cfg);

    // Each gameplay profile opts into its own section of the common INI. A full
    // path is matched against the running process image, not used to launch it.
    // Missing/invalid INI fails closed before installing a mouse hook.
    if (!cfg.targetIniSection.empty()) {
        const std::wstring iniPath = rmf::ResolveAgainstExeDir(L"RemoteMouseFix.ini");
        if (!FileExists(iniPath)) {
            ConsoleOut(L"ERROR: RemoteMouseFix.ini is missing beside RemoteMouseFix.exe.\n"
                       L"       Restore it from the package; no correction started.\n");
            return 2;
        }
        wchar_t selected[32768] {};
        const DWORD len = GetPrivateProfileStringW(cfg.targetIniSection.c_str(),
            L"Executable", L"", selected, 32768, iniPath.c_str());
        const std::wstring target(selected, static_cast<std::size_t>(len));
        if (target.empty() || len >= 32767) {
            ConsoleOut(L"ERROR: [%ls] Executable in RemoteMouseFix.ini is missing or too long.\n",
                       cfg.targetIniSection.c_str());
            return 2;
        }
        const bool invalidCharacters = std::any_of(target.begin(), target.end(), [](wchar_t ch) {
            return ch < 32 || ch == L'<' || ch == L'>' || ch == L'"' ||
                   ch == L'|' || ch == L'?' || ch == L'*';
        });
        const bool hasPathSeparator = target.find_first_of(L"\\/") != std::wstring::npos;
        const bool absoluteDrivePath =
            target.size() >= 6 && ((target[0] >= L'A' && target[0] <= L'Z') ||
                                     (target[0] >= L'a' && target[0] <= L'z')) &&
            target[1] == L':' && (target[2] == L'\\' || target[2] == L'/');
        const bool uncPath = target.size() >= 7 &&
            (target[0] == L'\\' || target[0] == L'/') &&
            (target[1] == L'\\' || target[1] == L'/');
        const bool hasPath = hasPathSeparator || target.find(L':') != std::wstring::npos;
        if (invalidCharacters || target.front() == L' ' || target.back() == L' ' ||
            (hasPath && !(absoluteDrivePath || uncPath)) || target.back() == L'\\' ||
            target.back() == L'/') {
            ConsoleOut(L"ERROR: invalid [%ls] Executable in RemoteMouseFix.ini.\n"
                       L"       Use Name.exe or a full absolute path such as C:\\Games\\MyGame.exe.\n",
                       cfg.targetIniSection.c_str());
            return 2;
        }
        std::wstring filename = target;
        if (hasPath) {
            // Normalize relative components such as ".." in a configured absolute path.
            std::wstring expanded(32768, L'\0');
            const DWORD size = GetFullPathNameW(target.c_str(),
                static_cast<DWORD>(expanded.size()), expanded.data(), nullptr);
            if (size == 0 || size >= expanded.size()) {
                ConsoleOut(L"ERROR: cannot resolve target path in RemoteMouseFix.ini.\n");
                return 2;
            }
            expanded.resize(size);
            std::replace(expanded.begin(), expanded.end(), L'/', L'\\');
            cfg.targetPath = std::move(expanded);
            filename = cfg.targetPath.substr(cfg.targetPath.find_last_of(L"\\/") + 1);
        }
        if (filename.size() < 5 ||
            CompareStringOrdinal(filename.c_str() + filename.size() - 4, 4,
                                 L".exe", 4, TRUE) != CSTR_EQUAL ||
            filename.find_first_of(L":\\/") != std::wstring::npos) {
            ConsoleOut(L"ERROR: [%ls] target must end with an .exe filename.\n",
                       cfg.targetIniSection.c_str());
            return 2;
        }
        cfg.targetProcess = filename;
        ConsoleOut(L"  target from [%ls]: %ls\n", cfg.targetIniSection.c_str(), target.c_str());
    }

    SetConsoleTitleW(cfg.diagnosticMode
                         ? L"RemoteMouseFix " RMF_VERSION_W L" - DIAGNOSTIC ONLY"
                         : L"RemoteMouseFix " RMF_VERSION_W L" - A/B CORRECTION TEST");

    ConsoleOut(L"RemoteMouseFix %ls - diagnostics with optional correction\n", RMF_VERSION_W);
    ConsoleOut(L"----------------------------------------------------------\n");
    for (const auto& warning : cfg.warnings) {
        ConsoleOut(L"  config warning: %ls\n", warning.c_str());
    }

    const std::wstring logDir = rmf::ResolveAgainstExeDir(cfg.logDirectory);

    rmf::Logger logger;
    std::wstring logError;
    if (!logger.Open(logDir, cfg.logFilePrefix, cfg.maxLogBytes, cfg.maxLogFiles, logError)) {
        ConsoleOut(L"  FATAL: %ls\n", logError.c_str());
        return 3;
    }
    {
        std::lock_guard<std::mutex> guard(g_logMutex);
        g_logger = &logger;
    }

    LARGE_INTEGER qpcFreqLi {};
    QueryPerformanceFrequency(&qpcFreqLi);
    const std::int64_t qpcFreq = qpcFreqLi.QuadPart;
    LARGE_INTEGER qpcStartLi {};
    QueryPerformanceCounter(&qpcStartLi);
    const std::int64_t qpcStart = qpcStartLi.QuadPart;
    FILETIME wallStart {};
    GetSystemTimeAsFileTime(&wallStart);

    // ---- hotkeys and correction permission ------------------------------------------
    // Registered before anything else runs, because whether the emergency-off key exists
    // decides whether a correction mode may be enabled at all.
    std::vector<std::wstring> failedHotkeys;
    bool offHotkeyRegistered = false;
    for (const auto& hotkey : kHotkeys) {
        if (RegisterHotKey(nullptr, hotkey.id, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, hotkey.vk)) {
            if (hotkey.id == kHotkeyCorrectionOff) {
                offHotkeyRegistered = true;
            }
        } else {
            failedHotkeys.push_back(Format(L"%ls (error %lu, probably used by another program)",
                                           hotkey.label, GetLastError()));
        }
    }

    rmf::CorrectionState correction;
    std::wstring lockReason;
    if (cfg.diagnosticMode) {
        lockReason = L"diagnostic_mode = true";
    } else if (!offHotkeyRegistered) {
        lockReason = L"emergency-off hotkey Ctrl+Alt+C could not be registered";
    }
    correction.allowed = lockReason.empty(); // written before the hook exists, read-only after

    // The same fact in two vocabularies: lockReason goes into the log in the config's own
    // terms, lockHint onto the screen for someone who has never read the config.
    std::wstring lockHint;
    if (cfg.diagnosticMode) {
        lockHint = L"this profile never touches the mouse";
    } else if (!offHotkeyRegistered) {
        lockHint = L"another program holds the emergency-off hotkey Ctrl+Alt+C";
    }

    rmf::StatusView panel;
    // Written by whichever thread trips a watchdog, read by the status thread.
    std::mutex   safetyStopMutex;
    std::wstring safetyStopReason;

    const bool elevated = rmf::IsProcessElevated();

    // ---- session header --------------------------------------------------------------
    {
        std::vector<std::wstring> header;
        header.push_back(L"# =====================================================================");
        header.push_back(L"# RemoteMouseFix " RMF_VERSION_W L" - diagnostic log");
        header.push_back(L"# started        : " + NowStampReadable());
        header.push_back(L"# os             : " + OsVersionString());
        header.push_back(L"# dpi awareness  : " + dpiMode);
        header.push_back(L"# elevated       : " + std::wstring(elevated ? L"yes" : L"no"));
        header.push_back(L"# config file    : " + (cfg.loadedFrom.empty() ? L"<defaults>" : cfg.loadedFrom));
        header.push_back(L"# target process : " + cfg.targetProcess);
        if (!cfg.targetPath.empty()) header.push_back(L"# target image   : " + cfg.targetPath);
        header.push_back(L"# log mouse moves: " + std::wstring(cfg.logMouseMoves ? L"yes" : L"no"));
        header.push_back(L"# A/B panel      : " + std::wstring(cfg.showAbTestUi ? L"on" : L"off"));
        header.push_back(L"# diagnostic mode: " + std::wstring(cfg.diagnosticMode ? L"yes" : L"no"));
        header.push_back(L"# correction     : " +
                         (correction.allowed
                              ? L"permitted, start=" + std::wstring(rmf::CorrectionModeName(cfg.correctionMode))
                              : L"LOCKED OFF (" + lockReason + L")"));
        header.push_back(Format(L"# watchdog       : max_hidden=%ums max_unechoed=%u max_output_failures=%u",
                                cfg.watchdogMaxHiddenMs, cfg.watchdogMaxUnechoed,
                                cfg.watchdogMaxOutputFailures));
        header.push_back(Format(L"# thresholds     : jump>=%dpx  anchor<=%dpx  state_poll=%ums",
                                cfg.jumpThresholdPx, cfg.anchorTolerancePx, cfg.statePollIntervalMs));
        header.push_back(Format(L"# smoothing      : absolute only, interval=%ums max_step=%ldpx",
                                kSmoothIntervalMs, kSmoothMaxStepPx));
        header.push_back(L"# timing trace   : " +
                         std::wstring(cfg.traceCorrectionTiming ? L"on (250ms windows)" : L"off"));
        header.push_back(Format(L"# heartbeat      : every %u ms (0 = off)", cfg.heartbeatIntervalMs));
        header.push_back(Format(L"# rotation       : %llu bytes, keep %u files",
                                static_cast<unsigned long long>(cfg.maxLogBytes), cfg.maxLogFiles));
        header.push_back(Format(L"# virtual screen : %dx%d at (%d,%d), monitors=%d",
                                GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                                GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                                GetSystemMetrics(SM_CMONITORS)));

        // Pointer speed and acceleration change how a relative delta becomes cursor motion,
        // which matters directly for the relative correction mode.
        int mouseParams[3] = {0, 0, 0};
        int mouseSpeed = 0;
        SystemParametersInfoW(SPI_GETMOUSE, 0, mouseParams, 0);
        SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &mouseSpeed, 0);
        header.push_back(Format(L"# MOUSECFG      : speed=%d threshold1=%d threshold2=%d acceleration=%d",
                                mouseSpeed, mouseParams[0], mouseParams[1], mouseParams[2]));

        for (const auto& failed : failedHotkeys) {
            header.push_back(L"# HOTKEY FAILED  : " + failed);
        }
        for (const auto& warning : cfg.warnings) {
            header.push_back(L"# CONFIG WARNING : " + warning);
        }

        header.push_back(L"#");
        if (correction.allowed) {
            header.push_back(L"# guarantees: input is modified only while a correction mode is on, and then only");
            header.push_back(L"#   injected moves from another program while the target holds a hidden cursor.");
            header.push_back(L"#   Buttons, wheel and physical input always pass. No keyboard is recorded, no network is used.");
        } else {
            header.push_back(L"# guarantees: no input is modified, no keyboard is recorded, no network is used.");
        }
        header.push_back(L"# column reference: docs/diagnostics.md");
        header.push_back(L"# =====================================================================");
        LogLines(header);
        logger.Flush();
    }

    ConsoleOut(L"  target process : %ls\n", cfg.targetProcess.c_str());
    if (!cfg.targetPath.empty()) ConsoleOut(L"  target image   : %ls\n", cfg.targetPath.c_str());
    ConsoleOut(L"  config file    : %ls\n",
               cfg.loadedFrom.empty() ? L"<defaults>" : cfg.loadedFrom.c_str());
    ConsoleOut(L"  run profile    : %ls\n",
               cfg.diagnosticMode ? L"DIAGNOSTIC ONLY (correction cannot be enabled)"
                                  : L"A/B CORRECTION TEST");
    ConsoleOut(L"  dpi awareness  : %ls\n", dpiMode.c_str());
    ConsoleOut(L"  log file       : %ls\n", logger.CurrentPath().c_str());
    ConsoleOut(L"  correction     : %ls\n",
               correction.allowed ? L"permitted (Ctrl+Alt+1 / Ctrl+Alt+2, Ctrl+Alt+C = off)"
                                  : (L"LOCKED OFF - " + lockReason).c_str());
    for (const auto& failed : failedHotkeys) {
        ConsoleOut(L"  HOTKEY FAILED  : %ls\n", failed.c_str());
    }
    ConsoleOut(L"\n");

    // From here on the panel owns the bottom of the window. Everything else has to go
    // through panel.Notice, or it would be painted over on the next refresh.
    {
        rmf::StatusSnapshot initial;
        initial.targetProcess     = cfg.targetProcess;
        initial.correctionAllowed = correction.allowed;
        initial.abTestUi          = cfg.showAbTestUi;
        initial.lockReason        = lockHint;
        initial.game              = rmf::IsProcessRunning(cfg.targetProcess, cfg.targetPath)
                                        ? rmf::GameState::Background
                                        : rmf::GameState::NotRunning;
        panel.Start(cfg.consoleStatusIntervalMs > 0, initial);
    }

    // The ring holds 16384 events of well over 100 bytes each, several megabytes: far too
    // large for the default 1 MB (MSVC) or 2 MB (MinGW) main-thread stack. On the stack it
    // overflows at entry to main as soon as RawEvent grows, so it lives on the heap.
    const auto queueStorage = std::make_unique<rmf::MouseEventQueue>();
    rmf::MouseEventQueue&  queue = *queueStorage;
    rmf::MouseHook::Filter filter;
    filter.logMouseMoves.store(cfg.logMouseMoves);

    // Any thread may trip a watchdog. Only a real transition logs, so repeated trips of an
    // already inactive correction stay silent.
    auto tripWatchdog = [&](const std::wstring& reason) {
        const rmf::CorrectionMode was = correction.ForceOff();
        if (was == rmf::CorrectionMode::Off) {
            return;
        }
        correction.watchdogTrips.fetch_add(1);
        LogLine(L"## CORRECTION DISABLED by watchdog: " + reason + L" (was " +
                rmf::CorrectionModeName(was) + L")");
        FlushLog();
        {
            std::lock_guard<std::mutex> guard(safetyStopMutex);
            safetyStopReason = reason;
        }
        panel.Notice(rmf::NoticeLevel::Error, L"the fix switched itself off: " + reason);
    };

    // ---- state sampler -----------------------------------------------------------------
    rmf::ProcessNameCache samplerNames(cfg.targetProcess, cfg.targetPath);
    std::atomic<bool> targetInForeground {false};
    std::atomic<bool> cursorHeldByTarget {false};

    // Sampler thread only.
    rmf::AnchorLearner anchorLearner;
    rmf::RecentWarpAnchor recentAnchor;
    // Relearning changes the reference for outstanding smoothing steps: main thread
    // discards the stale backlog before emitting another corrected move.
    std::atomic<std::uint32_t> anchorRevision {0};
    // Published only after successful absolute SendInput. Used solely by the
    // sampler to exclude our own cursor destinations from warp-anchor learning.
    std::atomic<long> lastOwnTargetX {0};
    std::atomic<long> lastOwnTargetY {0};
    std::atomic<std::uint64_t> lastOwnOutputMs {0};
    std::uint64_t ownCountAtHoldStart = 0; // sampler-thread only
    bool          haveLastShownCursor = false;
    POINT         lastShownCursor {0, 0};
    bool          sawWarpAtHoldStart = false;
    bool          holdPhaseOpen    = false;
    RECT          holdPhaseClient  {0, 0, 0, 0};
    ULONGLONG     hiddenSince      = 0;
    std::uint32_t hiddenGeneration = 0;

    auto publishAnchor = [&](POINT anchor, unsigned hits, unsigned total, RECT client) {
        const long offsetX = anchor.x - client.left;
        const long offsetY = anchor.y - client.top;
        const bool hadAnchor = filter.anchorValid.load(std::memory_order_relaxed);
        const long previousX = filter.anchorOffsetX.load(std::memory_order_relaxed);
        const long previousY = filter.anchorOffsetY.load(std::memory_order_relaxed);
        const bool changed = !hadAnchor || previousX != offsetX || previousY != offsetY;
        const bool relocated = hadAnchor &&
            (std::labs(previousX - offsetX) > cfg.anchorTolerancePx ||
             std::labs(previousY - offsetY) > cfg.anchorTolerancePx);
        // Make the two coordinate stores appear as one valid anchor to the main loop.
        filter.anchorValid.store(false, std::memory_order_release);
        filter.anchorOffsetX.store(offsetX, std::memory_order_relaxed);
        filter.anchorOffsetY.store(offsetY, std::memory_order_relaxed);
        filter.anchorValid.store(true, std::memory_order_release);
        if (relocated) anchorRevision.fetch_add(1, std::memory_order_release);
        if (changed && cfg.logStateChanges) {
            LogLine(Format(L"## STATE anchor-learned=(%ld,%ld) client_offset=(%ld,%ld) hits=%u/%u",
                           anchor.x, anchor.y, offsetX, offsetY, hits, total));
        }
    };

    rmf::StateSampler sampler;
    sampler.Start(cfg.statePollIntervalMs,
        [&](const rmf::StateSampler::Snapshot& prev, const rmf::StateSampler::Snapshot& now) {
            // The sampler owns target discovery: it publishes the PID to the hook only while
            // the target actually holds the foreground window.
            const bool isTarget = samplerNames.IsTargetPid(now.foregroundPid);
            filter.targetPid.store(isTarget ? now.foregroundPid : 0, std::memory_order_relaxed);
            targetInForeground.store(isTarget);

            if (!cfg.logStateChanges) {
                return;
            }
            // Focus transitions are logged in both directions: "something else took the
            // foreground mid-drag" is itself a suspect. No foreign input is recorded.
            const bool focusChanged = (now.foreground != prev.foreground) ||
                                      (now.foregroundPid != prev.foregroundPid);
            if (focusChanged && !isTarget) {
                LogLine(L"## STATE focus-left-target -> pid=" +
                        std::to_wstring(now.foregroundPid) + L" " +
                        samplerNames.NameForPid(now.foregroundPid));
                return;
            }
            if (!isTarget) {
                return;
            }
            LogLine(rmf::FormatStateLine(prev, now, samplerNames.NameForPid(now.foregroundPid)));
        },
        [&](const rmf::StateSampler::Snapshot& now) {
            const bool holding = targetInForeground.load() && !now.cursorVisible;
            cursorHeldByTarget.store(holding, std::memory_order_relaxed);

            if (holding) {
                // A warp-like discontinuity when the cursor becomes hidden is
                // independent evidence of recentering. Stable positions alone are
                // insufficient (Probe mode B can keep the cursor perfectly still).
                if (!holdPhaseOpen) {
                    recentAnchor.Reset();
                    ownCountAtHoldStart = correction.applied.load(std::memory_order_acquire);
                    sawWarpAtHoldStart = haveLastShownCursor &&
                        (std::labs(now.cursorPos.x - lastShownCursor.x) >= 30 ||
                         std::labs(now.cursorPos.y - lastShownCursor.y) >= 30);
                }
                const ULONGLONG tick = GetTickCount64();
                const auto ownCount = correction.applied.load(std::memory_order_acquire);
                const POINT lastOwnTarget {
                    lastOwnTargetX.load(std::memory_order_relaxed),
                    lastOwnTargetY.load(std::memory_order_relaxed)};
                recentAnchor.Observe(now.cursorPos, ownCount, lastOwnTarget, tick,
                                     lastOwnOutputMs.load(std::memory_order_acquire));
                const std::uint32_t generation = correction.generation.load();
                if (!holdPhaseOpen || generation != hiddenGeneration) {
                    // The watchdog clock starts with the hold, or with the latest mode
                    // change, so re-enabling during a long hold does not trip at once.
                    hiddenSince      = tick;
                    hiddenGeneration = generation;
                }
                holdPhaseOpen   = true;
                holdPhaseClient = now.clientScreenRect;
                anchorLearner.Add(now.cursorPos);

                POINT liveAnchor {};
                unsigned liveHits = 0, liveTotal = 0;
                if (!filter.anchorValid.load(std::memory_order_relaxed) &&
                    anchorLearner.Estimate(liveAnchor, liveHits, liveTotal)) {
                    publishAnchor(liveAnchor, liveHits, liveTotal, now.clientScreenRect);
                }

                // A previous game anchor can become stale after moving the window or
                // changing the game's camera mode. Accept a NEW location only after
                // both an onset warp and a strong, recent 12-sample consensus.
                // No output is issued from this sampler thread.
                POINT candidate {};
                unsigned candidateHits = 0, candidateSamples = 0;
                if (sawWarpAtHoldStart &&
                    correction.Mode() == rmf::CorrectionMode::Absolute &&
                    filter.anchorValid.load(std::memory_order_relaxed) &&
                    recentAnchor.Stable(candidate, candidateHits, candidateSamples)) {
                    const long expectedX = now.clientScreenRect.left +
                        filter.anchorOffsetX.load(std::memory_order_relaxed);
                    const long expectedY = now.clientScreenRect.top +
                        filter.anchorOffsetY.load(std::memory_order_relaxed);
                    if (std::labs(candidate.x - expectedX) > cfg.anchorTolerancePx ||
                        std::labs(candidate.y - expectedY) > cfg.anchorTolerancePx) {
                        if (cfg.logStateChanges) {
                            LogLine(Format(
                                L"## STATE anchor-relearned during hold: previous=(%ld,%ld) "
                                L"candidate=(%ld,%ld) hits=%u/%u (warp transition observed)",
                                expectedX, expectedY, candidate.x, candidate.y,
                                candidateHits, candidateSamples));
                        }
                        publishAnchor(candidate, candidateHits, candidateSamples, now.clientScreenRect);
                    }
                }

                if (cfg.watchdogMaxHiddenMs > 0 &&
                    correction.Mode() != rmf::CorrectionMode::Off &&
                    tick - hiddenSince > cfg.watchdogMaxHiddenMs) {
                    tripWatchdog(Format(L"cursor hidden continuously for more than %u ms",
                                        cfg.watchdogMaxHiddenMs));
                }
                return;
            }

            if (targetInForeground.load(std::memory_order_relaxed) && now.cursorVisible) {
                lastShownCursor = now.cursorPos;
                haveLastShownCursor = true;
            } else {
                haveLastShownCursor = false;
            }
            if (!holdPhaseOpen) {
                return;
            }
            holdPhaseOpen = false;
            sawWarpAtHoldStart = false;
            recentAnchor.Reset();

            POINT anchor {};
            unsigned hits = 0, total = 0;
            if (!anchorLearner.Finish(anchor, hits, total)) return;

            // Whole-hold estimates include our own SendInput positions. When output
            // happened in this hold, they MUST NOT overwrite a learned game anchor
            // on button release. Mid-hold re-learning uses the stricter, own-filtered
            // recent consensus instead.
            if (filter.anchorValid.load(std::memory_order_acquire) &&
                correction.applied.load(std::memory_order_acquire) != ownCountAtHoldStart) {
                if (cfg.logStateChanges) {
                    LogLine(L"## STATE anchor-release-estimate ignored: own output during hold");
                }
                return;
            }
            publishAnchor(anchor, hits, total, holdPhaseClient);
        });

    // ---- mode switching (message-loop thread only) ------------------------------------
    unsigned consecutiveOutputFailures = 0;

    auto switchMode = [&](rmf::CorrectionMode target, const wchar_t* source) {
        if (target != rmf::CorrectionMode::Off && !correction.allowed) {
            LogLine(L"## CORRECTION refused mode=" + std::wstring(rmf::CorrectionModeName(target)) +
                    L": " + lockReason);
            FlushLog();
            panel.Notice(rmf::NoticeLevel::Warn, L"the fix cannot be switched on: " + lockHint);
            return;
        }
        const rmf::CorrectionMode previous = correction.SetMode(target);
        consecutiveOutputFailures = 0;
        if (target != rmf::CorrectionMode::Off) {
            // Switching a mode back on clears the last stop, so the panel stops showing a
            // red line for something the player has already dealt with.
            std::lock_guard<std::mutex> guard(safetyStopMutex);
            safetyStopReason.clear();
        }
        LogLine(Format(L"## CORRECTION mode=%ls previous=%ls source=%ls",
                       rmf::CorrectionModeName(target), rmf::CorrectionModeName(previous), source));
        FlushLog();
        panel.Notice(target == rmf::CorrectionMode::Off ? rmf::NoticeLevel::Warn
                                                        : rmf::NoticeLevel::Good,
                     target == rmf::CorrectionMode::Off
                         ? std::wstring(L"the fix is now off")
                         : L"the fix is now on (" + std::wstring(rmf::CorrectionModeName(target)) + L")");
    };

    if (cfg.correctionMode != rmf::CorrectionMode::Off) {
        if (correction.allowed) {
            switchMode(cfg.correctionMode, L"config");
        } else {
            LogLine(L"## CORRECTION config correction_mode=" +
                    std::wstring(rmf::CorrectionModeName(cfg.correctionMode)) +
                    L" ignored: " + lockReason);
        }
    }

    // ---- hook ---------------------------------------------------------------------------
    rmf::MouseHook hook;
    rmf::MouseHook::Wiring wiring;
    wiring.queue        = &queue;
    wiring.filter       = &filter;
    wiring.correction   = &correction;
    wiring.applyThread  = g_mainThreadId;
    wiring.applyMessage = kMsgApplyDelta;

    DWORD hookError = 0;
    if (!hook.Install(wiring, hookError)) {
        panel.Notice(rmf::NoticeLevel::Error,
                     Format(L"cannot watch the mouse: SetWindowsHookEx failed with error %lu", hookError));
        panel.Stop();
        sampler.Stop();
        LogLine(L"# FATAL: SetWindowsHookEx(WH_MOUSE_LL) failed, error " + std::to_wstring(hookError));
        logger.Close();
        return 4;
    }

    panel.Notice(rmf::NoticeLevel::Good, L"watching the mouse now");
    if (runSeconds > 0) {
        panel.Notice(rmf::NoticeLevel::Info, Format(L"this run stops by itself in %u seconds", runSeconds));
    }

    // ---- writer thread -----------------------------------------------------------------
    std::atomic<std::uint64_t> writtenEvents {0};
    std::atomic<std::uint64_t> anchorCount   {0};
    std::atomic<std::uint64_t> jumpCount     {0};
    std::atomic<std::uint64_t> injectedCount {0};
    std::atomic<unsigned> markerCount {0};

    std::thread writer([&] {
        rmf::ProcessNameCache names(cfg.targetProcess, cfg.targetPath);

        POINT previousPt {0, 0};
        bool  havePrevious = false;
        DWORD previousTime = 0;

        auto decorate = [&](const rmf::RawEvent& raw, rmf::DecoratedEvent& ev) {
            ev.raw             = &raw;
            ev.injected        = (raw.hookFlags & LLMHF_INJECTED) != 0;
            ev.lowerIlInjected = (raw.hookFlags & LLMHF_LOWER_IL_INJECTED) != 0;
            ev.processName     = names.NameForPid(raw.foregroundPid);

            if (havePrevious) {
                ev.dx   = raw.pt.x - previousPt.x;
                ev.dy   = raw.pt.y - previousPt.y;
                ev.dtMs = static_cast<long>(raw.hookTimeMs - previousTime);
            } else {
                ev.dtMs = -1;
            }
            const bool hadPrevious = havePrevious;
            previousPt   = raw.pt;
            previousTime = raw.hookTimeMs;
            havePrevious = true;

            if (raw.haveGeometry) {
                ev.client.x   = raw.pt.x - raw.clientScreenRect.left;
                ev.client.y   = raw.pt.y - raw.clientScreenRect.top;
                ev.centreDx   = raw.pt.x - (raw.clientScreenRect.left + raw.clientScreenRect.right) / 2;
                ev.centreDy   = raw.pt.y - (raw.clientScreenRect.top + raw.clientScreenRect.bottom) / 2;
                ev.haveClient = true;

                if (raw.anchorValid && raw.cursorHidden) {
                    ev.anchorDx   = raw.pt.x - (raw.clientScreenRect.left + raw.anchorOffset.x);
                    ev.anchorDy   = raw.pt.y - (raw.clientScreenRect.top + raw.anchorOffset.y);
                    ev.haveAnchor = true;
                }
            }

            // A withheld move never reached the game, so its position says nothing about
            // whether the game's warp held.
            if (raw.kind == rmf::EventKind::Move && !raw.suppressed && ev.haveAnchor &&
                std::abs(ev.anchorDx) <= cfg.anchorTolerancePx &&
                std::abs(ev.anchorDy) <= cfg.anchorTolerancePx) {
                ev.atAnchor = true;
                anchorCount.fetch_add(1, std::memory_order_relaxed);
            }

            if (hadPrevious &&
                (std::abs(ev.dx) >= cfg.jumpThresholdPx || std::abs(ev.dy) >= cfg.jumpThresholdPx)) {
                ev.looksLikeJump = true;
                jumpCount.fetch_add(1, std::memory_order_relaxed);
            }
            if (ev.injected) {
                injectedCount.fetch_add(1, std::memory_order_relaxed);
            }
        };

        rmf::RawEvent raw;
        while (g_running.load(std::memory_order_relaxed)) {
            bool didWork = false;
            while (queue.Pop(raw)) {
                didWork = true;
                if (raw.kind == rmf::EventKind::Apply) {
                    LogLine(rmf::FormatApplyLine(raw, qpcFreq, qpcStart, wallStart));
                    continue;
                }
                rmf::DecoratedEvent ev;
                decorate(raw, ev);
                LogLine(rmf::FormatEventLine(ev, qpcFreq, qpcStart, wallStart));
                writtenEvents.fetch_add(1, std::memory_order_relaxed);

                if (cfg.flushOnButton && rmf::IsButtonEvent(raw.kind)) {
                    g_flushPending.store(true);
                }
            }
            if (g_flushPending.exchange(false)) {
                FlushLog();
            }
            if (!didWork) {
                Sleep(4);
            }
        }

        while (queue.Pop(raw)) {
            if (raw.kind == rmf::EventKind::Apply) {
                LogLine(rmf::FormatApplyLine(raw, qpcFreq, qpcStart, wallStart));
                continue;
            }
            rmf::DecoratedEvent ev;
            decorate(raw, ev);
            LogLine(rmf::FormatEventLine(ev, qpcFreq, qpcStart, wallStart));
            writtenEvents.fetch_add(1, std::memory_order_relaxed);
        }
        FlushLog();
    });

    auto modeLabel = [&]() -> const wchar_t* {
        return correction.allowed ? rmf::CorrectionModeName(correction.Mode()) : L"locked";
    };

    // ---- console status thread ---------------------------------------------------------
    std::thread status;
    if (cfg.consoleStatusIntervalMs > 0) {
        status = std::thread([&] {
            bool      gameRunning     = false;
            ULONGLONG lastProcessScan = 0;
            while (g_running.load(std::memory_order_relaxed)) {
                rmf::StatusSnapshot snap;
                snap.targetProcess     = cfg.targetProcess;
                snap.loggingPaused     = filter.paused.load();
                snap.correctionAllowed = correction.allowed;
                snap.abTestUi          = cfg.showAbTestUi;
                snap.markers           = markerCount.load(std::memory_order_relaxed);
                snap.mode              = correction.Mode();
                snap.lockReason        = lockHint;
                {
                    std::lock_guard<std::mutex> guard(safetyStopMutex);
                    snap.safetyStopReason = safetyStopReason;
                }
                if (targetInForeground.load()) {
                    snap.game   = rmf::GameState::Foreground;
                    gameRunning = true;
                } else {
                    // Walking the process list is too expensive for every refresh, and
                    // "has the game been started at all" changes slowly.
                    const ULONGLONG tick = GetTickCount64();
                    if (lastProcessScan == 0 || tick - lastProcessScan >= 2000) {
                        gameRunning     = rmf::IsProcessRunning(cfg.targetProcess, cfg.targetPath);
                        lastProcessScan = tick;
                    }
                    snap.game = gameRunning ? rmf::GameState::Background : rmf::GameState::NotRunning;
                }
                snap.events      = writtenEvents.load();
                snap.jumps       = jumpCount.load();
                snap.corrections = correction.applied.load();
                snap.problems    = correction.outputFailures.load() +
                                   static_cast<unsigned long long>(queue.Dropped());
                panel.Update(snap);
                for (unsigned slept = 0;
                     slept < cfg.consoleStatusIntervalMs && g_running.load(std::memory_order_relaxed);
                     slept += 50) {
                    Sleep(50);
                }
            }
        });
    }

    // ---- heartbeat -----------------------------------------------------------------------
    // Separates "nothing happened" from "the target never matched" in an empty log.
    std::thread heartbeat;
    if (cfg.heartbeatIntervalMs > 0) {
        heartbeat = std::thread([&] {
            while (g_running.load(std::memory_order_relaxed)) {
                for (unsigned slept = 0;
                     slept < cfg.heartbeatIntervalMs && g_running.load(std::memory_order_relaxed);
                     slept += 100) {
                    Sleep(100);
                }
                if (!g_running.load(std::memory_order_relaxed)) {
                    break;
                }
                const rmf::StateSampler::Snapshot snap = sampler.Current();
                LogLine(Format(
                    L"## HEARTBEAT target_foreground=%ls fg_pid=%lu correction=%ls events=%llu "
                    L"injected=%llu suppressed=%llu applied=%llu own=%llu anchor=%llu jump=%llu "
                    L"output_failures=%llu watchdog=%llu dropped=%llu cursor=%ls clip=%ls",
                    targetInForeground.load() ? L"yes" : L"no", snap.foregroundPid, modeLabel(),
                    U(writtenEvents), U(injectedCount), U(correction.suppressed),
                    U(correction.applied), U(correction.ownSeen), U(anchorCount), U(jumpCount),
                    U(correction.outputFailures), U(correction.watchdogTrips),
                    static_cast<unsigned long long>(queue.Dropped()),
                    snap.cursorVisible ? L"shown" : L"HIDDEN",
                    snap.clipIsFullScreen ? L"released" : L"CONFINED"));
                FlushLog();
            }
        });
    }

    // ---- auto-stop timer -----------------------------------------------------------------
    std::thread timer;
    if (runSeconds > 0) {
        LogLine(L"# auto-stop after " + std::to_wstring(runSeconds) + L" seconds");
        timer = std::thread([runSeconds] {
            for (unsigned slept = 0;
                 slept < runSeconds * 1000 && g_running.load(std::memory_order_relaxed);
                 slept += 50) {
                Sleep(50);
            }
            if (g_running.load(std::memory_order_relaxed)) {
                RequestStop();
            }
        });
    }

    // ---- message loop --------------------------------------------------------------------
    // WH_MOUSE_LL callbacks are delivered on this thread, so this loop drives the hook and
    // must stay responsive. It also executes the correction output posted by the hook.
    long long smoothPendingX = 0;
    long long smoothPendingY = 0;
    std::uint32_t lastAnchorRevision = anchorRevision.load(std::memory_order_acquire);

    // Opt-in A/B diagnostics. These counters belong to the message-loop thread; the
    // mouse hook does no additional work and logging is limited to one summary per 250 ms.
    struct TimingWindow {
        unsigned timerTicks       = 0;
        unsigned incoming         = 0;
        unsigned outputAttempts   = 0;
        unsigned missingAnchor    = 0;
        unsigned cursorReadFailed = 0;
        unsigned offAnchor        = 0;
        unsigned emptyPending     = 0;
        std::uint64_t maxTimerGapMs = 0;
        long lastAnchorDx = 0;
        long lastAnchorDy = 0;
        POINT lastCursor {0, 0};
        POINT lastAnchor {0, 0};
    } timing;
    std::uint64_t timingLastTickMs = 0;
    std::uint64_t timingLastReportMs = 0;
    std::uint64_t timingLastInputMs = 0;
    std::uint64_t timingPendingSinceMs = 0;

    auto qpcNowMs = [&]() -> std::uint64_t {
        LARGE_INTEGER now {};
        QueryPerformanceCounter(&now);
        // Split before multiplying to avoid overflow during long system uptimes.
        return static_cast<std::uint64_t>(now.QuadPart / qpcFreq) * 1000 +
               static_cast<std::uint64_t>((now.QuadPart % qpcFreq) * 1000 / qpcFreq);
    };

    auto applyDelta = [&](rmf::CorrectionMode mode, long dx, long dy) {
        POINT before {};
        GetCursorPos(&before);
        POINT target = before;
        const bool  ok    = rmf::ApplyRemoteDelta(mode, dx, dy, target);
        const DWORD error = ok ? 0 : GetLastError();
        POINT after {};
        GetCursorPos(&after);

        if (cfg.logMouseMoves && !filter.paused.load(std::memory_order_relaxed)) {
            rmf::RawEvent record;
            record.kind           = rmf::EventKind::Apply;
            record.correctionMode = mode;
            record.remoteDx       = dx;
            record.remoteDy       = dy;
            record.applyBefore    = before;
            record.applyTarget    = target;
            record.pt             = after;
            record.applyOk        = ok;
            record.applyError     = error;
            LARGE_INTEGER qpc {};
            QueryPerformanceCounter(&qpc);
            record.qpcTicks = qpc.QuadPart;
            queue.Push(record);
        }

        if (ok) {
            if (mode == rmf::CorrectionMode::Absolute) {
                lastOwnTargetX.store(target.x, std::memory_order_relaxed);
                lastOwnTargetY.store(target.y, std::memory_order_relaxed);
                lastOwnOutputMs.store(GetTickCount64(), std::memory_order_release);
            }
            correction.applied.fetch_add(1, std::memory_order_release);
            consecutiveOutputFailures = 0;
            const std::int64_t pending = correction.pendingEcho.fetch_add(1) + 1;
            if (pending > static_cast<std::int64_t>(cfg.watchdogMaxUnechoed)) {
                tripWatchdog(Format(L"%lld own moves never reached the hook "
                                    L"(input blocked, for example by UIPI)",
                                    static_cast<long long>(pending)));
            }
        } else {
            correction.outputFailures.fetch_add(1, std::memory_order_relaxed);
            if (++consecutiveOutputFailures >= cfg.watchdogMaxOutputFailures) {
                tripWatchdog(Format(L"%u consecutive SendInput failures, last error %lu",
                                    consecutiveOutputFailures, error));
                consecutiveOutputFailures = 0;
            }
        }
    };

    const UINT_PTR smoothTimerId = SetTimer(nullptr, kSmoothTimer, kSmoothIntervalMs, nullptr);
    if (smoothTimerId == 0) {
        LogLine(L"# CONFIG WARNING : smoothing timer unavailable; absolute mode uses immediate output");
    }
    MSG msg {};
    while (g_running.load(std::memory_order_relaxed)) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0 || got == -1) {
            break;
        }

        if (msg.message == kMsgQuitRequested) {
            break;
        }

        const auto observedRevision = anchorRevision.load(std::memory_order_acquire);
        if (observedRevision != lastAnchorRevision) {
            lastAnchorRevision = observedRevision;
            if (smoothPendingX || smoothPendingY) {
                LogLine(Format(L"## SMOOTH RESET reason=anchor-relearned discarded=(%+lld,%+lld)",
                               smoothPendingX, smoothPendingY));
            }
            smoothPendingX = 0;
            smoothPendingY = 0;
            timingPendingSinceMs = 0;
        }

        if (msg.message == kMsgApplyDelta) {
            const rmf::CorrectionMode mode = correction.Mode();
            if (mode == rmf::CorrectionMode::Off) {
                smoothPendingX = 0;
                smoothPendingY = 0;
                continue; // switched off after the hook posted: drop quietly
            }
            const long dx = static_cast<long>(static_cast<LONG_PTR>(msg.wParam));
            const long dy = static_cast<long>(static_cast<LONG_PTR>(msg.lParam));
            if (cfg.traceCorrectionTiming && mode == rmf::CorrectionMode::Absolute) {
                ++timing.incoming;
                timingLastInputMs = qpcNowMs();
            }
            if (mode == rmf::CorrectionMode::Absolute && smoothTimerId != 0) {
                if (cfg.traceCorrectionTiming && smoothPendingX == 0 && smoothPendingY == 0) {
                    timingPendingSinceMs = qpcNowMs();
                }
                auto retargetAxis = [&](long long& pending, long incoming, wchar_t axis) {
                    constexpr long kRetargetMinPx = 5;
                    const bool reversed =
                        (pending > kSmoothMaxStepPx && incoming <= -kRetargetMinPx) ||
                        (pending < -kSmoothMaxStepPx && incoming >= kRetargetMinPx);
                    if (!reversed) {
                        return;
                    }
                    LogLine(Format(L"## SMOOTH RETARGET axis=%lc discarded=%+lld incoming=%+ld",
                                   axis, pending, incoming));
                    pending = 0;
                };
                retargetAxis(smoothPendingX, dx, L'x');
                retargetAxis(smoothPendingY, dy, L'y');
                smoothPendingX += dx;
                smoothPendingY += dy;
                if (cfg.traceCorrectionTiming && smoothPendingX == 0 && smoothPendingY == 0) {
                    timingPendingSinceMs = 0;
                }
            } else {
                applyDelta(mode, dx, dy);
            }
            continue;
        }

        if (msg.message == WM_TIMER && msg.wParam == smoothTimerId) {
            if (cfg.traceCorrectionTiming) {
                const bool active = correction.Mode() == rmf::CorrectionMode::Absolute &&
                                    cursorHeldByTarget.load(std::memory_order_relaxed);
                if (!active) {
                    timing = TimingWindow{};
                    timingLastTickMs = 0;
                    timingLastReportMs = 0;
                    timingLastInputMs = 0;
                    timingPendingSinceMs = 0;
                } else {
                    const std::uint64_t nowMs = qpcNowMs();
                    if (timingLastReportMs == 0) timingLastReportMs = nowMs;
                    if (timingLastTickMs != 0) {
                        timing.maxTimerGapMs = std::max(timing.maxTimerGapMs, nowMs - timingLastTickMs);
                    }
                    timingLastTickMs = nowMs;
                    ++timing.timerTicks;
                    if (nowMs - timingLastReportMs >= 250) {
                        const auto inputAge = timingLastInputMs == 0 ? -1LL :
                            static_cast<long long>(nowMs - timingLastInputMs);
                        const auto pendingAge = timingPendingSinceMs == 0 ? 0ULL :
                            static_cast<unsigned long long>(nowMs - timingPendingSinceMs);
                        LogLine(Format(
                            L"## TIMING qpc_ms=%llu window_ms=%llu ticks=%u max_timer_gap_ms=%llu "
                            L"input=%u apply=%u skip=no_anchor:%u,cursor:%u,off_anchor:%u,empty:%u "
                            L"pending=(%lld,%lld) pending_age_ms=%llu input_age_ms=%lld "
                            L"anchor_delta=(%ld,%ld) anchor=(%ld,%ld) cursor=(%ld,%ld)",
                            static_cast<unsigned long long>(nowMs),
                            static_cast<unsigned long long>(nowMs - timingLastReportMs),
                            timing.timerTicks,
                            static_cast<unsigned long long>(timing.maxTimerGapMs),
                            timing.incoming, timing.outputAttempts, timing.missingAnchor,
                            timing.cursorReadFailed, timing.offAnchor, timing.emptyPending,
                            smoothPendingX, smoothPendingY, pendingAge, inputAge,
                            timing.lastAnchorDx, timing.lastAnchorDy,
                            timing.lastAnchor.x, timing.lastAnchor.y,
                            timing.lastCursor.x, timing.lastCursor.y));
                        timing = TimingWindow{};
                        timingLastReportMs = nowMs;
                    }
                }
            }
            if (correction.Mode() != rmf::CorrectionMode::Absolute ||
                !cursorHeldByTarget.load(std::memory_order_relaxed)) {
                smoothPendingX = 0;
                smoothPendingY = 0;
                continue;
            }
            // Never stack a new step on top of one the game has not consumed yet. Doing
            // so makes current+delta accumulate before the next game frame and amplifies
            // camera motion. The learned anchor is the proof that the previous step was
            // read and the game warped back.
            if (!filter.anchorValid.load(std::memory_order_relaxed)) {
                if (cfg.traceCorrectionTiming && (smoothPendingX || smoothPendingY)) ++timing.missingAnchor;
                continue;
            }
            const rmf::StateSampler::Snapshot snap = sampler.Current();
            POINT current {};
            if (!GetCursorPos(&current)) {
                if (cfg.traceCorrectionTiming && (smoothPendingX || smoothPendingY)) ++timing.cursorReadFailed;
                continue;
            }
            const long anchorX = snap.clientScreenRect.left +
                                 filter.anchorOffsetX.load(std::memory_order_relaxed);
            const long anchorY = snap.clientScreenRect.top +
                                 filter.anchorOffsetY.load(std::memory_order_relaxed);
            if (cfg.traceCorrectionTiming) {
                timing.lastCursor = current;
                timing.lastAnchor = POINT{anchorX, anchorY};
                timing.lastAnchorDx = current.x - anchorX;
                timing.lastAnchorDy = current.y - anchorY;
            }
            if (std::abs(current.x - anchorX) > cfg.anchorTolerancePx ||
                std::abs(current.y - anchorY) > cfg.anchorTolerancePx) {
                if (cfg.traceCorrectionTiming && (smoothPendingX || smoothPendingY)) ++timing.offAnchor;
                continue;
            }
            const long dx = static_cast<long>(std::clamp(
                smoothPendingX, -static_cast<long long>(kSmoothMaxStepPx),
                static_cast<long long>(kSmoothMaxStepPx)));
            const long dy = static_cast<long>(std::clamp(
                smoothPendingY, -static_cast<long long>(kSmoothMaxStepPx),
                static_cast<long long>(kSmoothMaxStepPx)));
            if (dx != 0 || dy != 0) {
                smoothPendingX -= dx;
                smoothPendingY -= dy;
                if (cfg.traceCorrectionTiming) {
                    ++timing.outputAttempts;
                    if (smoothPendingX == 0 && smoothPendingY == 0) timingPendingSinceMs = 0;
                }
                applyDelta(rmf::CorrectionMode::Absolute, dx, dy);
            } else if (cfg.traceCorrectionTiming) {
                ++timing.emptyPending;
            }
            continue;
        }

        if (msg.message == WM_HOTKEY) {
            switch (static_cast<int>(msg.wParam)) {
                case kHotkeyMarker: {
                    const unsigned markerNumber = markerCount.fetch_add(1, std::memory_order_relaxed) + 1;
                    const rmf::StateSampler::Snapshot snap = sampler.Current();
                    LogLine(Format(L"## MARKER #%u at cursor=(%ld,%ld) cursor_visible=%ls clip=%ls correction=%ls",
                                   markerNumber, snap.cursorPos.x, snap.cursorPos.y,
                                   snap.cursorVisible ? L"yes" : L"no",
                                   snap.clipIsFullScreen ? L"released" : L"CONFINED",
                                   modeLabel()));
                    FlushLog();
                    panel.Notice(rmf::NoticeLevel::Info,
                                 Format(L"marker #%u written to the log", markerNumber));
                    break;
                }
                case kHotkeyPause: {
                    const bool nowPaused = !filter.paused.load();
                    filter.paused.store(nowPaused);
                    LogLine(nowPaused ? L"## CAPTURE PAUSED by operator (correction unaffected)"
                                      : L"## CAPTURE RESUMED by operator");
                    FlushLog();
                    panel.Notice(nowPaused ? rmf::NoticeLevel::Warn : rmf::NoticeLevel::Info,
                                 nowPaused ? L"logging paused, the fix keeps working"
                                           : L"logging resumed");
                    break;
                }
                case kHotkeyQuit:
                    panel.Notice(rmf::NoticeLevel::Info, L"closing down");
                    g_running.store(false);
                    break;
                case kHotkeyCorrectionOff:
                    switchMode(rmf::CorrectionMode::Off, L"hotkey");
                    break;
                case kHotkeyCorrectionAbsolute:
                    switchMode(rmf::CorrectionMode::Absolute, L"hotkey");
                    break;
                case kHotkeyCorrectionRelative:
                    switchMode(rmf::CorrectionMode::Relative, L"hotkey");
                    break;
                default:
                    break;
            }
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ---- shutdown ------------------------------------------------------------------------
    if (smoothTimerId != 0) {
        KillTimer(nullptr, smoothTimerId);
    }
    g_running.store(false);
    correction.ForceOff();

    for (const auto& hotkey : kHotkeys) {
        UnregisterHotKey(nullptr, hotkey.id);
    }

    hook.Uninstall();
    sampler.Stop();

    for (std::thread* worker : {&writer, &status, &timer, &heartbeat}) {
        if (worker->joinable()) {
            worker->join();
        }
    }
    panel.Stop();

    {
        std::vector<std::wstring> footer;
        footer.push_back(L"#");
        footer.push_back(L"# --- session end " + NowStampReadable() + L" ---");
        footer.push_back(Format(L"# events logged   : %llu", U(writtenEvents)));
        footer.push_back(Format(L"# injected        : %llu", U(injectedCount)));
        footer.push_back(Format(L"# at anchor       : %llu", U(anchorCount)));
        footer.push_back(Format(L"# jump tagged     : %llu", U(jumpCount)));
        footer.push_back(Format(L"# suppressed      : %llu", U(correction.suppressed)));
        footer.push_back(Format(L"# applied         : %llu", U(correction.applied)));
        footer.push_back(Format(L"# own seen        : %llu", U(correction.ownSeen)));
        footer.push_back(Format(L"# output failures : %llu", U(correction.outputFailures)));
        footer.push_back(Format(L"# watchdog trips  : %llu", U(correction.watchdogTrips)));
        footer.push_back(Format(L"# hook events seen: %llu (outside target %llu, queue drops %llu)",
                                static_cast<unsigned long long>(rmf::MouseHook::TotalSeen()),
                                static_cast<unsigned long long>(rmf::MouseHook::FastPathSkipped()),
                                static_cast<unsigned long long>(queue.Dropped())));
        footer.push_back(Format(L"# log rotations   : %u", logger.RotationCount()));
        LogLines(footer);
    }

    ConsoleOut(L"  stopped. %llu events logged, %llu suppressed, %llu applied, %llu watchdog trips\n",
               U(writtenEvents), U(correction.suppressed), U(correction.applied),
               U(correction.watchdogTrips));
    if (queue.Dropped() > 0) {
        ConsoleOut(L"  note: %llu events were dropped because the queue was full.\n",
                   static_cast<unsigned long long>(queue.Dropped()));
    }
    ConsoleOut(L"  log: %ls\n", logger.CurrentPath().c_str());

    {
        std::lock_guard<std::mutex> guard(g_logMutex);
        g_logger = nullptr;
    }
    logger.Close();
    return 0;
}
