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
#include "rmf/StateSampler.h"
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

void ConsoleOut(const wchar_t* format, ...) {
    wchar_t buffer[1024];
    va_list args;
    va_start(args, format);
    const int written = vswprintf(buffer, 1024, format, args);
    va_end(args);
    if (written <= 0) {
        return;
    }

    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE || out == nullptr) {
        return;
    }
    DWORD ignored = 0;
    // WriteConsoleW keeps Unicode intact regardless of the console code page.
    if (!WriteConsoleW(out, buffer, static_cast<DWORD>(written), &ignored, nullptr)) {
        // Redirected to a file or pipe: fall back to UTF-8 bytes.
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, buffer, written, nullptr, 0, nullptr, nullptr);
        if (bytes > 0) {
            std::string utf8(static_cast<std::size_t>(bytes), '\0');
            WideCharToMultiByte(CP_UTF8, 0, buffer, written, utf8.data(), bytes, nullptr, nullptr);
            WriteFile(out, utf8.data(), static_cast<DWORD>(utf8.size()), &ignored, nullptr);
        }
    }
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
                               L"  --seconds N  stop automatically after N seconds (0 = run until stopped)\n\n"
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
    configPath = rmf::ResolveAgainstExeDir(configPath.empty() ? std::wstring(L"config.json") : configPath);

    rmf::Config cfg;
    rmf::LoadConfig(configPath, cfg);

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
        header.push_back(L"# log mouse moves: " + std::wstring(cfg.logMouseMoves ? L"yes" : L"no"));
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
    ConsoleOut(L"  dpi awareness  : %ls\n", dpiMode.c_str());
    ConsoleOut(L"  log file       : %ls\n", logger.CurrentPath().c_str());
    ConsoleOut(L"  correction     : %ls\n",
               correction.allowed ? L"permitted (Ctrl+Alt+1 / Ctrl+Alt+2, Ctrl+Alt+C = off)"
                                  : (L"LOCKED OFF - " + lockReason).c_str());
    for (const auto& failed : failedHotkeys) {
        ConsoleOut(L"  HOTKEY FAILED  : %ls\n", failed.c_str());
    }
    ConsoleOut(L"\n");

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
        ConsoleOut(L"\n  !! correction switched OFF by watchdog: %ls\n", reason.c_str());
    };

    // ---- state sampler -----------------------------------------------------------------
    rmf::ProcessNameCache samplerNames(cfg.targetProcess);
    std::atomic<bool> targetInForeground {false};

    // Sampler thread only.
    rmf::AnchorLearner anchorLearner;
    bool          holdPhaseOpen    = false;
    RECT          holdPhaseClient  {0, 0, 0, 0};
    ULONGLONG     hiddenSince      = 0;
    std::uint32_t hiddenGeneration = 0;

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

            if (holding) {
                const ULONGLONG tick = GetTickCount64();
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

                if (cfg.watchdogMaxHiddenMs > 0 &&
                    correction.Mode() != rmf::CorrectionMode::Off &&
                    tick - hiddenSince > cfg.watchdogMaxHiddenMs) {
                    tripWatchdog(Format(L"cursor hidden continuously for more than %u ms",
                                        cfg.watchdogMaxHiddenMs));
                }
                return;
            }

            if (!holdPhaseOpen) {
                return;
            }
            holdPhaseOpen = false;

            POINT anchor {};
            unsigned hits = 0, total = 0;
            if (!anchorLearner.Finish(anchor, hits, total)) {
                return;
            }
            const long offsetX = anchor.x - holdPhaseClient.left;
            const long offsetY = anchor.y - holdPhaseClient.top;
            const bool changed = !filter.anchorValid.load() ||
                                 filter.anchorOffsetX.load() != offsetX ||
                                 filter.anchorOffsetY.load() != offsetY;
            filter.anchorOffsetX.store(offsetX);
            filter.anchorOffsetY.store(offsetY);
            filter.anchorValid.store(true);
            if (changed && cfg.logStateChanges) {
                LogLine(Format(L"## STATE anchor-learned=(%ld,%ld) client_offset=(%ld,%ld) hits=%u/%u",
                               anchor.x, anchor.y, offsetX, offsetY, hits, total));
            }
        });

    // ---- mode switching (message-loop thread only) ------------------------------------
    unsigned consecutiveOutputFailures = 0;

    auto switchMode = [&](rmf::CorrectionMode target, const wchar_t* source) {
        if (target != rmf::CorrectionMode::Off && !correction.allowed) {
            LogLine(L"## CORRECTION refused mode=" + std::wstring(rmf::CorrectionModeName(target)) +
                    L": " + lockReason);
            FlushLog();
            ConsoleOut(L"\n  correction refused: %ls\n", lockReason.c_str());
            return;
        }
        const rmf::CorrectionMode previous = correction.SetMode(target);
        consecutiveOutputFailures = 0;
        LogLine(Format(L"## CORRECTION mode=%ls previous=%ls source=%ls",
                       rmf::CorrectionModeName(target), rmf::CorrectionModeName(previous), source));
        FlushLog();
        ConsoleOut(L"\n  correction: %ls\n", rmf::CorrectionModeName(target));
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
        ConsoleOut(L"  FATAL: SetWindowsHookEx(WH_MOUSE_LL) failed, error %lu\n", hookError);
        sampler.Stop();
        LogLine(L"# FATAL: SetWindowsHookEx(WH_MOUSE_LL) failed, error " + std::to_wstring(hookError));
        logger.Close();
        return 4;
    }

    ConsoleOut(L"  hook installed. Ctrl+Alt+M marker | Ctrl+Alt+P pause | Ctrl+Alt+Q quit\n");
    if (runSeconds > 0) {
        ConsoleOut(L"  auto-stop in %u seconds\n", runSeconds);
    }
    ConsoleOut(L"  waiting for %ls to come to the foreground...\n\n", cfg.targetProcess.c_str());

    // ---- writer thread -----------------------------------------------------------------
    std::atomic<std::uint64_t> writtenEvents {0};
    std::atomic<std::uint64_t> anchorCount   {0};
    std::atomic<std::uint64_t> jumpCount     {0};
    std::atomic<std::uint64_t> injectedCount {0};

    std::thread writer([&] {
        rmf::ProcessNameCache names(cfg.targetProcess);

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
            while (g_running.load(std::memory_order_relaxed)) {
                const bool inFg = targetInForeground.load();
                ConsoleOut(L"\r  [%ls] corr=%-12ls ev=%llu inj=%llu drop=%llu apply=%llu anchor=%llu jump=%llu wd=%llu   ",
                           filter.paused.load() ? L"PAUSED " : (inFg ? L"ACTIVE " : L"idle   "),
                           modeLabel(), U(writtenEvents), U(injectedCount),
                           U(correction.suppressed), U(correction.applied),
                           U(anchorCount), U(jumpCount), U(correction.watchdogTrips));
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
    unsigned markerCounter = 0;
    MSG msg {};
    while (g_running.load(std::memory_order_relaxed)) {
        const BOOL got = GetMessageW(&msg, nullptr, 0, 0);
        if (got == 0 || got == -1) {
            break;
        }

        if (msg.message == kMsgQuitRequested) {
            break;
        }

        if (msg.message == kMsgApplyDelta) {
            const rmf::CorrectionMode mode = correction.Mode();
            if (mode == rmf::CorrectionMode::Off) {
                continue; // switched off after the hook posted: drop quietly
            }
            const long dx = static_cast<long>(static_cast<LONG_PTR>(msg.wParam));
            const long dy = static_cast<long>(static_cast<LONG_PTR>(msg.lParam));

            POINT before {};
            GetCursorPos(&before);
            POINT target = before;
            const bool  ok    = rmf::ApplyRemoteDelta(mode, dx, dy, target);
            const DWORD error = ok ? 0 : GetLastError();
            POINT after {};
            GetCursorPos(&after);

            // Trace record proving whether the output landed. Pushed from this thread, which
            // is also the hook thread, so the queue keeps its single producer.
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
                correction.applied.fetch_add(1, std::memory_order_relaxed);
                consecutiveOutputFailures = 0;
                // Both modes use SendInput, so both must echo. Increment after sending: the
                // echo can only be observed once this loop returns to GetMessage, so the
                // counter never runs ahead.
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
            continue;
        }

        if (msg.message == WM_HOTKEY) {
            switch (static_cast<int>(msg.wParam)) {
                case kHotkeyMarker: {
                    ++markerCounter;
                    const rmf::StateSampler::Snapshot snap = sampler.Current();
                    LogLine(Format(L"## MARKER #%u at cursor=(%ld,%ld) cursor_visible=%ls clip=%ls correction=%ls",
                                   markerCounter, snap.cursorPos.x, snap.cursorPos.y,
                                   snap.cursorVisible ? L"yes" : L"no",
                                   snap.clipIsFullScreen ? L"released" : L"CONFINED",
                                   modeLabel()));
                    FlushLog();
                    ConsoleOut(L"\n  marker #%u written\n", markerCounter);
                    break;
                }
                case kHotkeyPause: {
                    const bool nowPaused = !filter.paused.load();
                    filter.paused.store(nowPaused);
                    LogLine(nowPaused ? L"## CAPTURE PAUSED by operator (correction unaffected)"
                                      : L"## CAPTURE RESUMED by operator");
                    FlushLog();
                    ConsoleOut(L"\n  logging %ls\n", nowPaused ? L"paused" : L"resumed");
                    break;
                }
                case kHotkeyQuit:
                    ConsoleOut(L"\n  quit requested\n");
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

    ConsoleOut(L"\n\n  stopped. %llu events logged, %llu suppressed, %llu applied, %llu watchdog trips\n",
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
