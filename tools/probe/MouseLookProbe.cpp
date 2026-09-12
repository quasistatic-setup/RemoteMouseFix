// MouseLookProbe - stand-in for a game's mouse-look loop, used to test RemoteMouseFix
// without the game.
//
// It reproduces the mechanism measured in WoW 3.3.5a (docs/findings-2026-09-12.md): on a
// button press it hides the cursor and warps it to a fixed, deliberately off-centre
// anchor; on a timer it reads the distance from that anchor as camera movement and warps
// back; on release it restores the cursor. Every phase is summarised in a log file.
//
// It reads the pointer through GetCursorPos. A game reading Raw Input would behave
// differently, so a pass here proves the correction logic, not compatibility with a
// specific game.

#include "rmf/WinCompat.h"

#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

namespace {

constexpr int      kClientWidth  = 1000;
constexpr int      kClientHeight = 700;
constexpr POINT    kAnchorOffset {380, 330}; // not the centre (500,350), like the real game
constexpr UINT_PTR kTimerLook    = 1;
constexpr UINT_PTR kTimerQuit    = 2;
constexpr UINT     kLookPeriodMs = 10;       // clamped to the ~15.6 ms timer tick, about a game frame
constexpr long     kJumpPx       = 40;

struct Phase {
    int       number      = 0;
    wchar_t   button      = L'?';
    unsigned  ticks       = 0;
    unsigned  motionTicks = 0;
    unsigned  jumps       = 0;
    long      sumX        = 0;
    long      sumY        = 0;
    long      maxTick     = 0;
    ULONGLONG startTick   = 0;
};

HWND         g_window        = nullptr;
FILE*        g_log           = nullptr;
bool         g_looking       = false;
int          g_buttons       = 0;
POINT        g_restore       {0, 0};
Phase        g_phase;
int          g_phaseCount    = 0;
long         g_yaw           = 0;
long         g_pitch         = 0;
std::wstring g_lastSummary   = L"(none yet)";
LARGE_INTEGER g_qpcFreq      {};
LARGE_INTEGER g_qpcStart     {};

double Elapsed() {
    LARGE_INTEGER now {};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - g_qpcStart.QuadPart) /
           static_cast<double>(g_qpcFreq.QuadPart);
}

void LogF(const char* format, ...) {
    if (!g_log) {
        return;
    }
    va_list args;
    va_start(args, format);
    std::vfprintf(g_log, format, args);
    va_end(args);
    std::fflush(g_log);
}

POINT AnchorScreen() {
    POINT anchor = kAnchorOffset;
    ClientToScreen(g_window, &anchor);
    return anchor;
}

// One "frame" of mouse-look: read the offset from the anchor, consume it, warp back.
long long QpcMs() {
    LARGE_INTEGER now {};
    QueryPerformanceCounter(&now);
    return static_cast<long long>(now.QuadPart * 1000 / g_qpcFreq.QuadPart);
}

void Sample() {
    POINT cursor {};
    if (!GetCursorPos(&cursor)) {
        return;
    }
    const long long readMs = QpcMs();
    const POINT anchor = AnchorScreen();
    const long dx = cursor.x - anchor.x;
    const long dy = cursor.y - anchor.y;
    ++g_phase.ticks;
    if (dx == 0 && dy == 0) {
        // Idle ticks are traced too: when a pointer update goes missing, the question is
        // whether a read happened in between and what it saw.
        LogF("%10.3f idle cur=(%ld,%ld) qpc_ms=%lld\n", Elapsed(), cursor.x, cursor.y, readMs);
        return;
    }

    const long magnitude = std::max(std::labs(dx), std::labs(dy));
    ++g_phase.motionTicks;
    g_phase.sumX += dx;
    g_phase.sumY += dy;
    g_phase.maxTick = std::max(g_phase.maxTick, magnitude);
    if (magnitude >= kJumpPx) {
        ++g_phase.jumps;
    }
    // Warp back immediately after reading, as a game loop does. Anything slow between the
    // read and the warp (logging, file flushes) would widen the window in which a pointer
    // update is overwritten, and make the probe report losses a real game would not have.
    SetCursorPos(anchor.x, anchor.y);
    g_yaw   += dx;
    g_pitch += dy;
    LogF("%10.3f tick d=(%+ld,%+ld) cur=(%ld,%ld) qpc_ms=%lld%s\n", Elapsed(), dx, dy,
         cursor.x, cursor.y, readMs, magnitude >= kJumpPx ? " JUMP" : "");
}

void StartLook(wchar_t button) {
    GetCursorPos(&g_restore);
    while (ShowCursor(FALSE) >= 0) {
    }
    SetCapture(g_window);

    const POINT anchor = AnchorScreen();
    SetCursorPos(anchor.x, anchor.y);

    g_looking          = true;
    g_phase            = Phase{};
    g_phase.number     = ++g_phaseCount;
    g_phase.button     = button;
    g_phase.startTick  = GetTickCount64();
    SetTimer(g_window, kTimerLook, kLookPeriodMs, nullptr);

    LogF("%10.3f LOOK-START #%d button=%lc restore=(%ld,%ld) anchor=(%ld,%ld)\n",
         Elapsed(), g_phase.number, button, g_restore.x, g_restore.y, anchor.x, anchor.y);
}

void EndLook() {
    if (!g_looking) {
        return;
    }
    Sample(); // consume what arrived since the last tick, as a game does on its next frame
    g_looking = false;
    KillTimer(g_window, kTimerLook);

    SetCursorPos(g_restore.x, g_restore.y);
    while (ShowCursor(TRUE) < 0) {
    }
    ReleaseCapture();

    const ULONGLONG duration = GetTickCount64() - g_phase.startTick;
    LogF("%10.3f PHASE #%d button=%lc ticks=%u motion_ticks=%u sum=(%+ld,%+ld) max_tick=%ld jumps40=%u duration_ms=%llu\n",
         Elapsed(), g_phase.number, g_phase.button, g_phase.ticks, g_phase.motionTicks,
         g_phase.sumX, g_phase.sumY, g_phase.maxTick, g_phase.jumps,
         static_cast<unsigned long long>(duration));

    wchar_t buffer[256];
    swprintf(buffer, 256, L"#%d %lc: sum=(%+ld,%+ld) max_tick=%ld jumps=%u",
             g_phase.number, g_phase.button, g_phase.sumX, g_phase.sumY, g_phase.maxTick, g_phase.jumps);
    g_lastSummary = buffer;
    InvalidateRect(g_window, nullptr, TRUE);
}

void OnButtonDown(int bit, wchar_t button) {
    g_buttons |= bit;
    if (!g_looking) {
        StartLook(button);
    }
}

void OnButtonUp(int bit) {
    g_buttons &= ~bit;
    if (g_looking && g_buttons == 0) {
        EndLook();
    }
}

void Paint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT client {};
    GetClientRect(hwnd, &client);
    FillRect(dc, &client, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));

    wchar_t text[512];
    swprintf(text, 512,
             L"MouseLookProbe - stand-in for a game mouse-look loop\n\n"
             L"Hold the left or right mouse button in this window to turn the camera.\n"
             L"yaw=%ld  pitch=%ld  phases=%d\n"
             L"last phase: %ls\n\n"
             L"Esc closes.",
             g_yaw, g_pitch, g_phaseCount, g_lastSummary.c_str());
    RECT textRect {20, 20, client.right - 20, client.bottom - 20};
    DrawTextW(dc, text, -1, &textRect, DT_LEFT | DT_TOP);

    // Mark the anchor so a human tester can see where the warp goes.
    MoveToEx(dc, kAnchorOffset.x - 10, kAnchorOffset.y, nullptr);
    LineTo(dc, kAnchorOffset.x + 11, kAnchorOffset.y);
    MoveToEx(dc, kAnchorOffset.x, kAnchorOffset.y - 10, nullptr);
    LineTo(dc, kAnchorOffset.x, kAnchorOffset.y + 11);
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_LBUTTONDOWN: OnButtonDown(1, L'L'); return 0;
        case WM_RBUTTONDOWN: OnButtonDown(2, L'R'); return 0;
        case WM_LBUTTONUP:   OnButtonUp(1);         return 0;
        case WM_RBUTTONUP:   OnButtonUp(2);         return 0;
        case WM_TIMER:
            if (wParam == kTimerLook && g_looking) {
                Sample();
            } else if (wParam == kTimerQuit) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_CAPTURECHANGED:
            // Losing capture mid-look (Alt+Tab, another window) must never leave the cursor
            // hidden.
            if (g_looking && reinterpret_cast<HWND>(lParam) != hwnd) {
                g_buttons = 0;
                EndLook();
            }
            return 0;
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE) {
                DestroyWindow(hwnd);
            }
            return 0;
        case WM_PAINT:
            Paint(hwnd);
            return 0;
        case WM_DESTROY:
            g_buttons = 0;
            EndLook();
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
    }
}

} // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    rmf::InitDpiAwareness();
    QueryPerformanceFrequency(&g_qpcFreq);
    QueryPerformanceCounter(&g_qpcStart);

    std::wstring logPath = rmf::GetExecutableDirectory() + L"\\mouselook-probe.log";
    unsigned quitSeconds = 0;
    {
        int argc = 0;
        if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
            for (int i = 1; i + 1 < argc; ++i) {
                const std::wstring arg = argv[i];
                if (arg == L"--seconds") {
                    quitSeconds = static_cast<unsigned>(std::max(0L, std::wcstol(argv[++i], nullptr, 10)));
                } else if (arg == L"--log") {
                    logPath = argv[++i];
                }
            }
            LocalFree(argv);
        }
    }

    g_log = _wfopen(logPath.c_str(), L"w");
    LogF("# MouseLookProbe client=%dx%d anchor_offset=(%ld,%ld) jump=%ld look_period_ms=%u\n",
         kClientWidth, kClientHeight, kAnchorOffset.x, kAnchorOffset.y, kJumpPx, kLookPeriodMs);

    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc   = WindowProc;
    windowClass.hInstance     = instance;
    windowClass.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    windowClass.lpszClassName = L"MouseLookProbeWindow";
    RegisterClassW(&windowClass);

    RECT frame {0, 0, kClientWidth, kClientHeight};
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRect(&frame, style, FALSE);

    g_window = CreateWindowW(windowClass.lpszClassName, L"MouseLookProbe", style,
                             120, 120, frame.right - frame.left, frame.bottom - frame.top,
                             nullptr, nullptr, instance, nullptr);
    if (!g_window) {
        return 1;
    }
    ShowWindow(g_window, showCommand);
    UpdateWindow(g_window);

    if (quitSeconds > 0) {
        SetTimer(g_window, kTimerQuit, quitSeconds * 1000, nullptr);
    }

    MSG msg {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    LogF("# end yaw=%ld pitch=%ld phases=%d\n", g_yaw, g_pitch, g_phaseCount);
    if (g_log) {
        std::fclose(g_log);
    }
    return 0;
}
