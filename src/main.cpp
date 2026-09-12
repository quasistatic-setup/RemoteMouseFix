// RemoteMouseFix - Phase 1: diagnostics only.
//
// Observes the low-level mouse stream while a configured target process owns the
// foreground window, and writes a human readable trace. It installs exactly one
// WH_MOUSE_LL hook and always forwards every event unchanged.
//
// What this build deliberately does NOT do:
//  * no DLL injection, no driver, no code loaded into the game
//  * no SendInput / mouse_event / SetCursorPos / ClipCursor - it never writes input
//  * no filtering, correction or suppression of any event
//  * no keyboard hook, so no keystroke or text content can be recorded
//  * no network access of any kind
// Phase 2 can add correction; Phase 1 exists to produce the evidence first.

#include "rmf/Version.h"

#include "rmf/Config.h"
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
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

// Hotkey ids. RegisterHotKey is used instead of a keyboard hook on purpose: it can only
// ever observe these two specific combinations, so the tool remains incapable of
// recording keystrokes or text.
constexpr int kHotkeyMarker = 1; // Ctrl+Alt+M: drop a marker line into the log
constexpr int kHotkeyPause  = 2; // Ctrl+Alt+P: pause / resume capture
constexpr int kHotkeyQuit   = 3; // Ctrl+Alt+Q: clean shutdown

constexpr UINT kMsgQuitRequested = WM_APP + 1;

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

std::wstring NowStampReadable() {
    SYSTEMTIME st {};
    GetLocalTime(&st);
    wchar_t buf[64];
    swprintf(buf, 64, L"%04u-%02u-%02u %02u:%02u:%02u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring OsVersionString() {
    // GetVersionEx lies for unmanifested apps, so read the real build from ntoskrnl's
    // sibling: RtlGetVersion is not subject to compatibility shimming.
    using RtlGetVersionFn = LONG (WINAPI*)(PRTL_OSVERSIONINFOW);
    if (HMODULE ntdll = GetModuleHandleW(L"ntdll.dll")) {
        auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
        if (rtlGetVersion) {
            RTL_OSVERSIONINFOW info {};
            info.dwOSVersionInfoSize = sizeof(info);
            if (rtlGetVersion(&info) == 0) {
                wchar_t buf[64];
                swprintf(buf, 64, L"Windows %lu.%lu build %lu",
                         info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
                return buf;
            }
        }
    }
    return L"Windows (version unavailable)";
}

// Serialises the two writers into the log: the drain thread and the sampler callback.
std::mutex           g_logMutex;
rmf::Logger*         g_logger = nullptr;
std::atomic<bool>    g_flushPending {false};

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

} // namespace

int main() {
    g_mainThreadId = GetCurrentThreadId();

    // DPI awareness before anything reads a coordinate: the hook reports physical
    // pixels, and every rectangle we compare against must be in the same space.
    const std::wstring dpiMode = rmf::InitDpiAwareness();

    SetConsoleCtrlHandler(&ConsoleCtrlHandler, TRUE);
    SetConsoleTitleW(L"RemoteMouseFix " RMF_VERSION_W L" - diagnostics");

    // Command line: an alternative config path and an optional auto-stop timeout.
    std::wstring configPath;
    unsigned runSeconds = 0; // 0 = run until the operator stops the tool
    {
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            for (int i = 1; i < argc; ++i) {
                const std::wstring arg = argv[i];
                if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
                    ConsoleOut(L"RemoteMouseFix %ls - Phase 1 diagnostics\n\n"
                               L"Usage: RemoteMouseFix.exe [path\\to\\config.json] [--seconds N]\n\n"
                               L"  --seconds N  stop automatically after N seconds (0 = run until stopped)\n\n"
                               L"Hotkeys while running:\n"
                               L"  Ctrl+Alt+M   write a marker line into the log\n"
                               L"  Ctrl+Alt+P   pause / resume capture\n"
                               L"  Ctrl+Alt+Q   quit (Ctrl+C works too)\n",
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
    if (configPath.empty()) {
        configPath = rmf::ResolveAgainstExeDir(L"config.json");
    } else {
        configPath = rmf::ResolveAgainstExeDir(configPath);
    }

    rmf::Config cfg;
    rmf::LoadConfig(configPath, cfg);

    ConsoleOut(L"RemoteMouseFix %ls - Phase 1 diagnostics (observe only)\n", RMF_VERSION_W);
    ConsoleOut(L"----------------------------------------------------------\n");

    // Phase 1 has no correction path at all, so running with diagnostic_mode off would
    // silently do nothing. Refuse instead of pretending.
    if (!cfg.diagnosticMode) {
        ConsoleOut(L"config has diagnostic_mode = false, but this build contains no\n"
                   L"filtering and no input synthesis. Set diagnostic_mode = true.\n");
        return 2;
    }

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

    const bool elevated = rmf::IsProcessElevated();

    // Session header. Everything needed to interpret the trace later lives here, so a
    // log file is self-describing when it comes back for analysis.
    {
        std::vector<std::wstring> header;
        header.push_back(L"# =====================================================================");
        header.push_back(L"# RemoteMouseFix " RMF_VERSION_W L" - Phase 1 diagnostic log");
        header.push_back(L"# started        : " + NowStampReadable());
        header.push_back(L"# os             : " + OsVersionString());
        header.push_back(L"# dpi awareness  : " + dpiMode);
        header.push_back(L"# elevated       : " + std::wstring(elevated ? L"yes" : L"no"));
        header.push_back(L"# config file    : " + (cfg.loadedFrom.empty() ? L"<defaults>" : cfg.loadedFrom));
        header.push_back(L"# target process : " + cfg.targetProcess);
        header.push_back(L"# log mouse moves: " + std::wstring(cfg.logMouseMoves ? L"yes" : L"no"));
        {
            wchar_t buf[256];
            swprintf(buf, 256, L"# thresholds     : jump>=%dpx  recenter<=%dpx  state_poll=%ums",
                     cfg.jumpThresholdPx, cfg.recenterTolerancePx, cfg.statePollIntervalMs);
            header.push_back(buf);
            swprintf(buf, 256, L"# heartbeat      : every %u ms (0 = off)", cfg.heartbeatIntervalMs);
            header.push_back(buf);
            swprintf(buf, 256, L"# rotation       : %llu bytes, keep %u files",
                     static_cast<unsigned long long>(cfg.maxLogBytes), cfg.maxLogFiles);
            header.push_back(buf);
            swprintf(buf, 256, L"# virtual screen : %dx%d at (%d,%d), monitors=%d",
                     GetSystemMetrics(SM_CXVIRTUALSCREEN), GetSystemMetrics(SM_CYVIRTUALSCREEN),
                     GetSystemMetrics(SM_XVIRTUALSCREEN), GetSystemMetrics(SM_YVIRTUALSCREEN),
                     GetSystemMetrics(SM_CMONITORS));
            header.push_back(buf);
        }

        // Pointer speed and the "enhance pointer precision" acceleration curve both
        // change how a relative delta becomes a cursor movement, so they belong in the
        // record of any mouse investigation.
        int mouseParams[3] = {0, 0, 0};
        int mouseSpeed = 0;
        SystemParametersInfoW(SPI_GETMOUSE, 0, mouseParams, 0);
        SystemParametersInfoW(SPI_GETMOUSESPEED, 0, &mouseSpeed, 0);
        {
            wchar_t buf[256];
            swprintf(buf, 256, L"# MOUSECFG      : speed=%d threshold1=%d threshold2=%d acceleration=%d",
                     mouseSpeed, mouseParams[0], mouseParams[1], mouseParams[2]);
            header.push_back(buf);
        }

        header.push_back(L"#");
        header.push_back(L"# guarantees: no input is modified, no keyboard is recorded, no network is used.");
        header.push_back(L"# column reference: docs/diagnostics.md");
        header.push_back(L"# =====================================================================");
        LogLines(header);
        logger.Flush();
    }

    ConsoleOut(L"  target process : %ls\n", cfg.targetProcess.c_str());
    ConsoleOut(L"  dpi awareness  : %ls\n", dpiMode.c_str());
    ConsoleOut(L"  log file       : %ls\n", logger.CurrentPath().c_str());
    ConsoleOut(L"  log mouse moves: %ls\n", cfg.logMouseMoves ? L"yes" : L"no");
    ConsoleOut(L"\n");

    rmf::MouseEventQueue   queue;
    rmf::MouseHook::Filter filter;
    filter.logMouseMoves.store(cfg.logMouseMoves);

    rmf::ProcessNameCache samplerNames(cfg.targetProcess);

    // The sampler owns target discovery: it publishes the PID to the hook only while
    // the target process actually holds the foreground window. That single atomic is
    // what makes the hook's fast path both cheap and exactly scoped.
    std::atomic<bool> targetInForeground {false};
    std::atomic<DWORD> lastTargetPid {0};

    rmf::StateSampler sampler;
    sampler.Start(cfg.statePollIntervalMs,
        [&](const rmf::StateSampler::Snapshot& prev, const rmf::StateSampler::Snapshot& now) {
            const bool isTarget = samplerNames.IsTargetPid(now.foregroundPid);
            filter.targetPid.store(isTarget ? now.foregroundPid : 0, std::memory_order_relaxed);
            targetInForeground.store(isTarget);
            if (isTarget) {
                lastTargetPid.store(now.foregroundPid);
            }

            if (!cfg.logStateChanges) {
                return;
            }

            // Focus transitions are logged in both directions, because "something else
            // took the foreground mid-drag" is itself a prime suspect. These lines carry
            // a window handle and a process name, never any input data, so the rule that
            // no foreign input reaches the file still holds.
            const bool focusChanged = (now.foreground != prev.foreground) ||
                                      (now.foregroundPid != prev.foregroundPid);
            if (focusChanged && !isTarget) {
                LogLine(L"## STATE focus-left-target -> pid=" +
                        std::to_wstring(now.foregroundPid) + L" " +
                        samplerNames.NameForPid(now.foregroundPid));
                return;
            }

            // Detailed state (cursor visibility, ClipCursor, geometry, DPI) only while
            // the target actually owns the foreground.
            if (!isTarget) {
                return;
            }
            LogLine(rmf::FormatStateLine(prev, now, samplerNames.NameForPid(now.foregroundPid)));
        });

    rmf::MouseHook hook;
    DWORD hookError = 0;
    if (!hook.Install(queue, filter, hookError)) {
        ConsoleOut(L"  FATAL: SetWindowsHookEx(WH_MOUSE_LL) failed, error %lu\n", hookError);
        sampler.Stop();
        LogLine(L"# FATAL: SetWindowsHookEx(WH_MOUSE_LL) failed, error " + std::to_wstring(hookError));
        logger.Close();
        return 4;
    }

    RegisterHotKey(nullptr, kHotkeyMarker, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'M');
    RegisterHotKey(nullptr, kHotkeyPause,  MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'P');
    RegisterHotKey(nullptr, kHotkeyQuit,   MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, 'Q');

    ConsoleOut(L"  hook installed. Hotkeys: Ctrl+Alt+M marker | Ctrl+Alt+P pause | Ctrl+Alt+Q quit\n");
    if (runSeconds > 0) {
        ConsoleOut(L"  auto-stop in %u seconds\n", runSeconds);
    }
    ConsoleOut(L"  waiting for %ls to come to the foreground...\n\n", cfg.targetProcess.c_str());

    // ---- writer thread -------------------------------------------------------------
    // Drains the ring, resolves process names, derives deltas and writes the lines.
    // Kept off the hook thread so no disk I/O can ever delay an input event.
    std::atomic<std::uint64_t> writtenEvents {0};
    std::atomic<std::uint64_t> recenterCount {0};
    std::atomic<std::uint64_t> jumpCount     {0};
    std::atomic<std::uint64_t> injectedCount {0};

    std::thread writer([&] {
        rmf::ProcessNameCache names(cfg.targetProcess);

        POINT previousPt {0, 0};
        bool  havePrevious = false;
        DWORD previousTime = 0;

        rmf::RawEvent raw;
        while (g_running.load(std::memory_order_relaxed)) {
            bool didWork = false;
            while (queue.Pop(raw)) {
                didWork = true;

                rmf::DecoratedEvent ev;
                ev.raw = &raw;
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
                previousPt   = raw.pt;
                previousTime = raw.hookTimeMs;
                havePrevious = true;

                if (raw.haveGeometry) {
                    ev.client.x = raw.pt.x - raw.clientScreenRect.left;
                    ev.client.y = raw.pt.y - raw.clientScreenRect.top;
                    const long centreX = (raw.clientScreenRect.left + raw.clientScreenRect.right) / 2;
                    const long centreY = (raw.clientScreenRect.top + raw.clientScreenRect.bottom) / 2;
                    ev.centreDx  = raw.pt.x - centreX;
                    ev.centreDy  = raw.pt.y - centreY;
                    ev.haveClient = true;
                }

                // Recenter signature: the game warps the pointer with SetCursorPos,
                // which surfaces here as an injected move landing on the client centre.
                if (raw.kind == rmf::EventKind::Move && ev.injected && ev.haveClient &&
                    std::abs(ev.centreDx) <= cfg.recenterTolerancePx &&
                    std::abs(ev.centreDy) <= cfg.recenterTolerancePx) {
                    ev.looksLikeRecenter = true;
                    recenterCount.fetch_add(1, std::memory_order_relaxed);
                }

                if (havePrevious &&
                    (std::abs(ev.dx) >= cfg.jumpThresholdPx ||
                     std::abs(ev.dy) >= cfg.jumpThresholdPx)) {
                    ev.looksLikeJump = true;
                    jumpCount.fetch_add(1, std::memory_order_relaxed);
                }

                if (ev.injected) {
                    injectedCount.fetch_add(1, std::memory_order_relaxed);
                }

                LogLine(rmf::FormatEventLine(ev, qpcFreq, qpcStart, wallStart));
                writtenEvents.fetch_add(1, std::memory_order_relaxed);

                // A button event is the anchor of the whole investigation, so make sure
                // it survives a crash or a hard kill of the session.
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

        // Drain whatever is still queued at shutdown.
        while (queue.Pop(raw)) {
            rmf::DecoratedEvent ev;
            ev.raw = &raw;
            ev.injected        = (raw.hookFlags & LLMHF_INJECTED) != 0;
            ev.lowerIlInjected = (raw.hookFlags & LLMHF_LOWER_IL_INJECTED) != 0;
            ev.processName     = names.NameForPid(raw.foregroundPid);
            ev.dtMs            = -1;
            LogLine(rmf::FormatEventLine(ev, qpcFreq, qpcStart, wallStart));
        }
        FlushLog();
    });

    // ---- console status thread -----------------------------------------------------
    std::thread status;
    if (cfg.consoleStatusIntervalMs > 0) {
        status = std::thread([&] {
            while (g_running.load(std::memory_order_relaxed)) {
                const bool inFg = targetInForeground.load();
                ConsoleOut(L"\r  [%ls] events=%llu  injected=%llu  recenter=%llu  jump=%llu  dropped=%llu   ",
                           filter.paused.load() ? L"PAUSED "
                                                : (inFg ? L"ACTIVE " : L"idle   "),
                           static_cast<unsigned long long>(writtenEvents.load()),
                           static_cast<unsigned long long>(injectedCount.load()),
                           static_cast<unsigned long long>(recenterCount.load()),
                           static_cast<unsigned long long>(jumpCount.load()),
                           static_cast<unsigned long long>(queue.Dropped()));
                // Split sleep so shutdown stays responsive without a waitable timer.
                for (unsigned slept = 0;
                     slept < cfg.consoleStatusIntervalMs && g_running.load(std::memory_order_relaxed);
                     slept += 50) {
                    Sleep(50);
                }
            }
        });
    }

    // ---- heartbeat -----------------------------------------------------------------
    // An empty event section is ambiguous on its own: it can mean "nothing happened" or
    // "the target name never matched". The heartbeat separates those two cases, which
    // matters when the capture is taken by someone else on another machine.
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
                const bool inFg = targetInForeground.load();
                wchar_t buf[320];
                swprintf(buf, 320,
                         L"## HEARTBEAT target_foreground=%ls fg_pid=%lu events=%llu "
                         L"injected=%llu recenter=%llu jump=%llu dropped=%llu "
                         L"cursor=%ls clip=%ls",
                         inFg ? L"yes" : L"no", snap.foregroundPid,
                         static_cast<unsigned long long>(writtenEvents.load()),
                         static_cast<unsigned long long>(injectedCount.load()),
                         static_cast<unsigned long long>(recenterCount.load()),
                         static_cast<unsigned long long>(jumpCount.load()),
                         static_cast<unsigned long long>(queue.Dropped()),
                         snap.cursorVisible ? L"shown" : L"HIDDEN",
                         snap.clipIsFullScreen ? L"released" : L"CONFINED");
                LogLine(buf);
                FlushLog();
            }
        });
    }

    // ---- auto-stop timer -----------------------------------------------------------
    // Lets a test run be bounded from the command line, which is also how an unattended
    // capture is taken: start it, do the test, let it close and hand over the log.
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

    // ---- message loop --------------------------------------------------------------
    // WH_MOUSE_LL callbacks are delivered to this thread's message queue, so this loop
    // is what actually drives the hook. It must stay responsive.
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

        if (msg.message == WM_HOTKEY) {
            switch (static_cast<int>(msg.wParam)) {
                case kHotkeyMarker: {
                    ++markerCounter;
                    const rmf::StateSampler::Snapshot snap = sampler.Current();
                    wchar_t buf[256];
                    swprintf(buf, 256,
                             L"## MARKER #%u at cursor=(%ld,%ld) cursor_visible=%ls clip=%ls",
                             markerCounter, snap.cursorPos.x, snap.cursorPos.y,
                             snap.cursorVisible ? L"yes" : L"no",
                             snap.clipIsFullScreen ? L"released" : L"CONFINED");
                    LogLine(buf);
                    FlushLog();
                    ConsoleOut(L"\n  marker #%u written\n", markerCounter);
                    break;
                }
                case kHotkeyPause: {
                    const bool nowPaused = !filter.paused.load();
                    filter.paused.store(nowPaused);
                    LogLine(nowPaused ? L"## CAPTURE PAUSED by operator"
                                      : L"## CAPTURE RESUMED by operator");
                    FlushLog();
                    ConsoleOut(L"\n  capture %ls\n", nowPaused ? L"paused" : L"resumed");
                    break;
                }
                case kHotkeyQuit:
                    ConsoleOut(L"\n  quit requested\n");
                    g_running.store(false);
                    break;
                default:
                    break;
            }
            continue;
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // ---- shutdown ------------------------------------------------------------------
    g_running.store(false);

    UnregisterHotKey(nullptr, kHotkeyMarker);
    UnregisterHotKey(nullptr, kHotkeyPause);
    UnregisterHotKey(nullptr, kHotkeyQuit);

    hook.Uninstall();
    sampler.Stop();

    if (writer.joinable()) {
        writer.join();
    }
    if (status.joinable()) {
        status.join();
    }
    if (timer.joinable()) {
        timer.join();
    }
    if (heartbeat.joinable()) {
        heartbeat.join();
    }

    {
        std::vector<std::wstring> footer;
        wchar_t buf[256];
        footer.push_back(L"#");
        footer.push_back(L"# --- session end " + NowStampReadable() + L" ---");
        swprintf(buf, 256, L"# events logged  : %llu",
                 static_cast<unsigned long long>(writtenEvents.load()));
        footer.push_back(buf);
        swprintf(buf, 256, L"# injected       : %llu",
                 static_cast<unsigned long long>(injectedCount.load()));
        footer.push_back(buf);
        swprintf(buf, 256, L"# recenter tagged: %llu",
                 static_cast<unsigned long long>(recenterCount.load()));
        footer.push_back(buf);
        swprintf(buf, 256, L"# jump tagged    : %llu",
                 static_cast<unsigned long long>(jumpCount.load()));
        footer.push_back(buf);
        swprintf(buf, 256, L"# hook events seen: %llu (fast-path skipped %llu, queue drops %llu)",
                 static_cast<unsigned long long>(rmf::MouseHook::TotalSeen()),
                 static_cast<unsigned long long>(rmf::MouseHook::FastPathSkipped()),
                 static_cast<unsigned long long>(queue.Dropped()));
        footer.push_back(buf);
        swprintf(buf, 256, L"# log rotations  : %u", logger.RotationCount());
        footer.push_back(buf);
        LogLines(footer);
    }

    ConsoleOut(L"\n\n  stopped. %llu events logged (%llu injected, %llu recenter, %llu jump)\n",
               static_cast<unsigned long long>(writtenEvents.load()),
               static_cast<unsigned long long>(injectedCount.load()),
               static_cast<unsigned long long>(recenterCount.load()),
               static_cast<unsigned long long>(jumpCount.load()));

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
