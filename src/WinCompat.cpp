#include "rmf/WinCompat.h"

#include <cstdint>
#include <vector>

namespace rmf {
namespace {

// Declared locally so the build does not depend on a particular SDK/mingw header
// vintage. All three are resolved at runtime.
using SetProcessDpiAwarenessContextFn = BOOL (WINAPI*)(HANDLE);
using GetDpiForWindowFn               = UINT (WINAPI*)(HWND);
using SetProcessDpiAwarenessFn        = HRESULT (WINAPI*)(int);

// DPI_AWARENESS_CONTEXT values are opaque negative handles; a reinterpret_cast is not
// a constant expression, so these are built on use rather than declared constexpr.
inline HANDLE DpiContext(std::intptr_t value) {
    return reinterpret_cast<HANDLE>(value);
}

constexpr std::intptr_t kPerMonitorAwareV2 = -4;
constexpr std::intptr_t kPerMonitorAware   = -3;
constexpr int           kShcorePerMonitor  = 2; // PROCESS_PER_MONITOR_DPI_AWARE

} // namespace

std::wstring InitDpiAwareness() {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto setCtx = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
        if (setCtx) {
            if (setCtx(DpiContext(kPerMonitorAwareV2))) {
                return L"per-monitor-v2";
            }
            if (setCtx(DpiContext(kPerMonitorAware))) {
                return L"per-monitor-v1";
            }
            // Already set by a manifest: not an error, just report it.
            if (GetLastError() == ERROR_ACCESS_DENIED) {
                return L"already-set-by-manifest";
            }
        }
    }

    // Windows 8.1 era fallback.
    if (HMODULE shcore = LoadLibraryW(L"shcore.dll")) {
        auto setAwareness = reinterpret_cast<SetProcessDpiAwarenessFn>(
            reinterpret_cast<void*>(GetProcAddress(shcore, "SetProcessDpiAwareness")));
        if (setAwareness && SUCCEEDED(setAwareness(kShcorePerMonitor))) {
            return L"per-monitor-shcore";
        }
    }

    if (SetProcessDPIAware()) {
        return L"system-dpi";
    }
    return L"dpi-unaware";
}

UINT GetWindowDpi(HWND hwnd) {
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto getDpi = reinterpret_cast<GetDpiForWindowFn>(
            reinterpret_cast<void*>(GetProcAddress(user32, "GetDpiForWindow")));
        if (getDpi && hwnd) {
            const UINT dpi = getDpi(hwnd);
            if (dpi != 0) {
                return dpi;
            }
        }
    }

    if (HDC screen = GetDC(nullptr)) {
        const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
        if (dpi > 0) {
            return static_cast<UINT>(dpi);
        }
    }
    return 0;
}

bool QueryProcessImagePath(DWORD pid, std::wstring& outPath, bool& outAccessDenied) {
    outPath.clear();
    outAccessDenied = false;
    if (pid == 0) {
        return false;
    }

    // PROCESS_QUERY_LIMITED_INFORMATION is the whole reason this tool needs no admin
    // rights: it is granted for same-user, same-integrity processes.
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        const DWORD err = GetLastError();
        outAccessDenied = (err == ERROR_ACCESS_DENIED);
        return false;
    }

    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        DWORD size = static_cast<DWORD>(buffer.size());
        if (QueryFullProcessImageNameW(proc, 0, buffer.data(), &size)) {
            outPath.assign(buffer.data(), size);
            CloseHandle(proc);
            return true;
        }
        if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || buffer.size() >= 32768) {
            CloseHandle(proc);
            return false;
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool IsProcessElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation {};
    DWORD returned = 0;
    const bool ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &returned) != 0;
    CloseHandle(token);
    return ok && elevation.TokenIsElevated != 0;
}

std::wstring GetExecutableDirectory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD len = GetModuleFileNameW(nullptr, buffer.data(),
                                             static_cast<DWORD>(buffer.size()));
        if (len == 0) {
            return L".";
        }
        if (len < buffer.size() - 1) {
            std::wstring path(buffer.data(), len);
            const std::size_t slash = path.find_last_of(L"\\/");
            return (slash == std::wstring::npos) ? std::wstring(L".") : path.substr(0, slash);
        }
        if (buffer.size() >= 32768) {
            return L".";
        }
        buffer.resize(buffer.size() * 2);
    }
}

} // namespace rmf
