// InputEchoCheck - answers the one question behind the most confusing way RemoteMouseFix
// can fail: while the game is in front, does our own SendInput come back through our own
// low-level hook?
//
// When it does not, RemoteMouseFix switches the correction off and logs "own moves never
// reached the hook". That message names UIPI as the usual cause, but it cannot prove it,
// because Windows discards synthetic input to a higher-integrity foreground window
// without failing the call (see the SendInput documentation). This tool reproduces that
// single step outside the game and additionally reports the integrity level of the
// foreground process, which is what decides the UIPI question.
//
// It moves the pointer by one pixel and back, touches nothing else, and writes its result
// to input-echo-check.txt next to the working directory so it can be attached to a report.
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>

namespace {

// Our own signature, ASCII "RMFE". Deliberately not RemoteMouseFix's "RMFX": a running
// RemoteMouseFix signs its corrections too, and counting those as our echo turned a
// healthy result into "46 of 8 came back".
constexpr ULONG_PTR kSignature    = 0x524D4645;
constexpr ULONG_PTR kRmfSignature = 0x524D4658;

HHOOK g_hook             = nullptr;
int   g_own              = 0; // came back carrying our signature
int   g_fromRmf          = 0; // corrections from a RemoteMouseFix running alongside
int   g_injectedByOthers = 0; // injected by someone else: the remote-control software
int   g_physical         = 0; // real mouse
FILE* g_out              = nullptr;

void Say(const wchar_t* format, ...) {
    wchar_t line[512];
    va_list args;
    va_start(args, format);
    vswprintf(line, 512, format, args);
    va_end(args);
    wprintf(L"  %ls\n", line);
    if (g_out != nullptr) {
        fwprintf(g_out, L"%ls\n", line);
        fflush(g_out);
    }
    fflush(stdout);
}

LRESULT CALLBACK HookProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION && wParam == WM_MOUSEMOVE) {
        const MSLLHOOKSTRUCT* event = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        if (event->dwExtraInfo == kSignature) {
            ++g_own;
        } else if (event->dwExtraInfo == kRmfSignature) {
            ++g_fromRmf;
        } else if ((event->flags & LLMHF_INJECTED) != 0) {
            ++g_injectedByOthers;
        } else {
            ++g_physical;
        }
    }
    return CallNextHookEx(g_hook, code, wParam, lParam);
}

// The hook is delivered to this thread, so the message queue has to keep running while we
// wait for our own input to come back.
void Pump(int milliseconds) {
    MSG msg;
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 10) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        Sleep(10);
    }
}

bool IsRemoteMouseFixRunning() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    PROCESSENTRY32W entry {};
    entry.dwSize = sizeof(entry);
    bool found   = false;
    if (Process32FirstW(snapshot, &entry) != 0) {
        do {
            if (CompareStringOrdinal(entry.szExeFile, -1, L"RemoteMouseFix.exe", -1, TRUE) ==
                CSTR_EQUAL) {
                found = true;
                break;
            }
        } while (Process32NextW(snapshot, &entry) != 0);
    }
    CloseHandle(snapshot);
    return found;
}

const wchar_t* IntegrityLevelOf(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        return L"<cannot open: higher integrity, or the process is gone>";
    }
    HANDLE         token  = nullptr;
    const wchar_t* result = L"<unknown>";
    if (OpenProcessToken(process, TOKEN_QUERY, &token) != 0) {
        DWORD size = 0;
        GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
        if (size > 0) {
            std::vector<BYTE> buffer(size);
            if (GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size) != 0) {
                auto*       label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
                const DWORD count = *GetSidSubAuthorityCount(label->Label.Sid);
                const DWORD rid   = *GetSidSubAuthority(label->Label.Sid, count - 1);
                if (rid >= SECURITY_MANDATORY_SYSTEM_RID) {
                    result = L"System";
                } else if (rid >= SECURITY_MANDATORY_HIGH_RID) {
                    result = L"High (elevated)";
                } else if (rid >= SECURITY_MANDATORY_MEDIUM_RID) {
                    result = L"Medium (normal)";
                } else {
                    result = L"Low";
                }
            }
        }
        CloseHandle(token);
    }
    CloseHandle(process);
    return result;
}

} // namespace

// Plain main with CommandLineToArgvW, like RemoteMouseFix itself: wmain would tie the
// MinGW build to -municode for no gain.
int main() {
    int countdown = 12;
    int argc      = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        if (argc > 1) {
            countdown = static_cast<int>(std::wcstol(argv[1], nullptr, 10));
        }
        LocalFree(argv);
    }
    if (countdown < 0) {
        countdown = 0;
    }

    if (_wfopen_s(&g_out, L"input-echo-check.txt", L"w, ccs=UTF-8") != 0) {
        g_out = nullptr; // the console output alone still answers the question
    }

    wprintf(L"\n  InputEchoCheck\n  --------------\n");
    wprintf(L"  Click into the GAME window now, and hold the right mouse button if you\n");
    wprintf(L"  want the mouse-look state measured. The check runs in %d seconds and\n", countdown);
    wprintf(L"  moves the pointer by one pixel, nothing else.\n\n");
    if (IsRemoteMouseFixRunning()) {
        wprintf(L"  NOTE: RemoteMouseFix is running. While it corrects, it withholds exactly\n");
        wprintf(L"  the kind of input this check sends, so a missing echo would be its doing.\n");
        wprintf(L"  Quit it first (Ctrl+Alt+Q) for a result about the system itself.\n\n");
    }
    for (int left = countdown; left > 0; --left) {
        wprintf(L"\r  starting in %2d ...", left);
        fflush(stdout);
        Sleep(1000);
    }
    wprintf(L"\r                      \r");

    const HWND foreground = GetForegroundWindow();
    DWORD      pid        = 0;
    GetWindowThreadProcessId(foreground, &pid);
    wchar_t path[MAX_PATH] = L"<no foreground window>";
    if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        DWORD size = MAX_PATH;
        QueryFullProcessImageNameW(process, 0, path, &size);
        CloseHandle(process);
    }
    CURSORINFO cursor {};
    cursor.cbSize = sizeof(cursor);
    GetCursorInfo(&cursor);

    Say(L"foreground   : pid=%lu %ls", pid, path);
    Say(L"integrity    : %ls  (this tool: %ls)", IntegrityLevelOf(pid),
        IntegrityLevelOf(GetCurrentProcessId()));
    Say(L"cursor       : %ls",
        (cursor.flags & CURSOR_SHOWING) != 0 ? L"shown" : L"HIDDEN (mouse-look active)");

    g_hook = SetWindowsHookExW(WH_MOUSE_LL, HookProc, GetModuleHandleW(nullptr), 0);
    if (g_hook == nullptr) {
        Say(L"FAILED       : SetWindowsHookEx error %lu", GetLastError());
        if (g_out != nullptr) {
            fclose(g_out);
        }
        return 1;
    }

    Pump(300); // let anything already in flight drain before counting
    g_own = g_injectedByOthers = g_physical = 0;

    int sent = 0, refused = 0;
    for (int i = 0; i < 8; ++i) {
        INPUT input {};
        input.type            = INPUT_MOUSE;
        input.mi.dx           = (i % 2 == 0) ? 1 : -1; // net zero, so the pointer stays put
        input.mi.dwFlags      = MOUSEEVENTF_MOVE;
        input.mi.dwExtraInfo  = kSignature;
        SetLastError(0);
        if (SendInput(1, &input, sizeof(INPUT)) == 1) {
            ++sent;
        } else {
            ++refused;
            Say(L"SendInput    : refused with error %lu", GetLastError());
        }
        Pump(150);
    }
    Pump(500);
    UnhookWindowsHookEx(g_hook);

    Say(L"sent         : %d (SendInput refused %d)", sent, refused);
    Say(L"came back    : %d of %d carrying our signature", g_own, sent);
    Say(L"also seen    : %d injected by others, %d physical, %d from a running RemoteMouseFix",
        g_injectedByOthers, g_physical, g_fromRmf);
    Say(L"verdict      : %ls",
        g_own >= sent
            ? L"echo intact - RemoteMouseFix can correct in this state"
            : (g_own == 0 ? L"NO echo - our input is discarded before it reaches the hook"
                          : L"partial echo - input is being dropped intermittently"));
    if (g_own < sent && g_fromRmf > 0) {
        Say(L"             : but RemoteMouseFix was correcting alongside this check, and its");
        Say(L"               filter withholds foreign injected moves. Quit it and repeat.");
    }

    if (g_out != nullptr) {
        fclose(g_out);
        wprintf(L"\n  Written to input-echo-check.txt. Press Enter to close.\n");
    } else {
        wprintf(L"\n  Press Enter to close.\n");
    }
    getwchar();
    return 0;
}
