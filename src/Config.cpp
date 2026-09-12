#include "rmf/Config.h"
#include "rmf/WinCompat.h"

#include <windows.h>

#include <cstdlib>
#include <string>
#include <vector>

namespace rmf {
namespace {

// Minimal reader for a flat JSON object of strings, booleans and numbers.
//
// A full JSON library would be dead weight here: config.json is a handful of scalar
// keys and the tool must stay a single dependency-free EXE. Anything this reader does
// not understand becomes a warning, never a silent default change.
class FlatJsonReader {
public:
    explicit FlatJsonReader(const std::wstring& text) : text_(text) {}

    // Parses into key/value pairs. Values keep their JSON token form: quoted strings
    // arrive unquoted and unescaped, everything else arrives verbatim.
    bool Parse(std::vector<std::pair<std::wstring, std::wstring>>& out, std::wstring& error) {
        SkipWs();
        if (!Expect(L'{', error)) return false;
        SkipWs();
        if (Peek() == L'}') { Advance(); return true; }

        for (;;) {
            SkipWs();
            std::wstring key;
            if (!ReadString(key, error)) return false;
            SkipWs();
            if (!Expect(L':', error)) return false;
            SkipWs();
            std::wstring value;
            if (!ReadValue(value, error)) return false;
            out.emplace_back(std::move(key), std::move(value));
            SkipWs();
            const wchar_t c = Peek();
            if (c == L',') { Advance(); continue; }
            if (c == L'}') { Advance(); return true; }
            error = L"expected ',' or '}' at offset " + std::to_wstring(pos_);
            return false;
        }
    }

private:
    wchar_t Peek() const { return pos_ < text_.size() ? text_[pos_] : L'\0'; }
    void    Advance()    { if (pos_ < text_.size()) ++pos_; }

    void SkipWs() {
        for (;;) {
            while (pos_ < text_.size() &&
                   (text_[pos_] == L' ' || text_[pos_] == L'\t' ||
                    text_[pos_] == L'\r' || text_[pos_] == L'\n')) {
                ++pos_;
            }
            // Tolerate // comments so the shipped config.json can explain itself.
            if (pos_ + 1 < text_.size() && text_[pos_] == L'/' && text_[pos_ + 1] == L'/') {
                while (pos_ < text_.size() && text_[pos_] != L'\n') ++pos_;
                continue;
            }
            return;
        }
    }

    bool Expect(wchar_t c, std::wstring& error) {
        if (Peek() != c) {
            error = std::wstring(L"expected '") + c + L"' at offset " + std::to_wstring(pos_);
            return false;
        }
        Advance();
        return true;
    }

    bool ReadString(std::wstring& out, std::wstring& error) {
        if (!Expect(L'"', error)) return false;
        out.clear();
        while (pos_ < text_.size()) {
            const wchar_t c = text_[pos_++];
            if (c == L'"') return true;
            if (c != L'\\') { out.push_back(c); continue; }
            if (pos_ >= text_.size()) break;
            const wchar_t esc = text_[pos_++];
            switch (esc) {
                case L'"':  out.push_back(L'"');  break;
                case L'\\': out.push_back(L'\\'); break;
                case L'/':  out.push_back(L'/');  break;
                case L'n':  out.push_back(L'\n'); break;
                case L't':  out.push_back(L'\t'); break;
                case L'r':  out.push_back(L'\r'); break;
                case L'b':  out.push_back(L'\b'); break;
                case L'f':  out.push_back(L'\f'); break;
                case L'u': {
                    if (pos_ + 4 > text_.size()) { error = L"truncated \\u escape"; return false; }
                    const std::wstring hex = text_.substr(pos_, 4);
                    pos_ += 4;
                    out.push_back(static_cast<wchar_t>(std::wcstoul(hex.c_str(), nullptr, 16)));
                    break;
                }
                default:
                    error = L"unsupported escape \\" + std::wstring(1, esc);
                    return false;
            }
        }
        error = L"unterminated string";
        return false;
    }

    bool ReadValue(std::wstring& out, std::wstring& error) {
        if (Peek() == L'"') {
            return ReadString(out, error);
        }
        out.clear();
        while (pos_ < text_.size()) {
            const wchar_t c = text_[pos_];
            if (c == L',' || c == L'}' || c == L' ' || c == L'\t' ||
                c == L'\r' || c == L'\n') {
                break;
            }
            out.push_back(c);
            ++pos_;
        }
        if (out.empty()) {
            error = L"empty value at offset " + std::to_wstring(pos_);
            return false;
        }
        return true;
    }

    const std::wstring& text_;
    std::size_t pos_ = 0;
};

bool ReadFileUtf8AsWide(const std::wstring& path, std::wstring& out) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER size {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart > (4 << 20)) {
        CloseHandle(file);
        return false;
    }

    std::string bytes(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = bytes.empty() ||
                    (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()),
                              &read, nullptr) && read == bytes.size());
    CloseHandle(file);
    if (!ok) {
        return false;
    }

    // Strip a UTF-8 BOM; editors on Windows add one readily.
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF) {
        bytes.erase(0, 3);
    }

    if (bytes.empty()) { out.clear(); return true; }

    const int needed = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
                                           static_cast<int>(bytes.size()), nullptr, 0);
    if (needed <= 0) {
        return false;
    }
    out.resize(static_cast<std::size_t>(needed));
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
                        out.data(), needed);
    return true;
}

bool ParseBool(const std::wstring& v, bool& out) {
    if (v == L"true"  || v == L"1") { out = true;  return true; }
    if (v == L"false" || v == L"0") { out = false; return true; }
    return false;
}

bool ParseU64(const std::wstring& v, std::uint64_t& out) {
    wchar_t* end = nullptr;
    const unsigned long long parsed = std::wcstoull(v.c_str(), &end, 10);
    if (end == v.c_str() || (end && *end != L'\0')) {
        return false;
    }
    out = static_cast<std::uint64_t>(parsed);
    return true;
}

bool ParseInt(const std::wstring& v, int& out) {
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(v.c_str(), &end, 10);
    if (end == v.c_str() || (end && *end != L'\0')) {
        return false;
    }
    out = static_cast<int>(parsed);
    return true;
}

} // namespace

std::wstring ResolveAgainstExeDir(const std::wstring& path) {
    if (path.empty()) {
        return GetExecutableDirectory();
    }
    // Already absolute: drive letter, UNC, or rooted.
    const bool absolute = (path.size() >= 2 && path[1] == L':') ||
                          (path.size() >= 2 && (path[0] == L'\\' || path[0] == L'/') &&
                                               (path[1] == L'\\' || path[1] == L'/')) ||
                          (path[0] == L'\\' || path[0] == L'/');
    if (absolute) {
        return path;
    }
    return GetExecutableDirectory() + L"\\" + path;
}

bool LoadConfig(const std::wstring& path, Config& cfg) {
    std::wstring text;
    if (!ReadFileUtf8AsWide(path, text)) {
        cfg.warnings.push_back(L"config not readable, using built-in defaults: " + path);
        return true;
    }

    std::vector<std::pair<std::wstring, std::wstring>> pairs;
    std::wstring error;
    FlatJsonReader reader(text);
    if (!reader.Parse(pairs, error)) {
        cfg.warnings.push_back(L"config parse error (" + error + L"), using built-in defaults");
        return false;
    }

    cfg.loadedFrom = path;

    for (const auto& [key, value] : pairs) {
        bool          b = false;
        std::uint64_t u = 0;
        int           i = 0;

        if (key == L"target_process") {
            if (value.empty()) {
                cfg.warnings.push_back(L"target_process is empty, keeping " + cfg.targetProcess);
            } else {
                cfg.targetProcess = value;
            }
        } else if (key == L"diagnostic_mode") {
            if (ParseBool(value, b)) cfg.diagnosticMode = b;
            else cfg.warnings.push_back(L"diagnostic_mode is not a boolean: " + value);
        } else if (key == L"log_mouse_moves") {
            if (ParseBool(value, b)) cfg.logMouseMoves = b;
            else cfg.warnings.push_back(L"log_mouse_moves is not a boolean: " + value);
        } else if (key == L"log_directory") {
            cfg.logDirectory = value;
        } else if (key == L"log_file_prefix") {
            if (!value.empty()) cfg.logFilePrefix = value;
        } else if (key == L"max_log_bytes") {
            if (ParseU64(value, u) && u >= 64 * 1024) cfg.maxLogBytes = u;
            else cfg.warnings.push_back(L"max_log_bytes must be >= 65536: " + value);
        } else if (key == L"max_log_files") {
            if (ParseU64(value, u) && u >= 1 && u <= 100) cfg.maxLogFiles = static_cast<unsigned>(u);
            else cfg.warnings.push_back(L"max_log_files must be 1..100: " + value);
        } else if (key == L"flush_on_button") {
            if (ParseBool(value, b)) cfg.flushOnButton = b;
            else cfg.warnings.push_back(L"flush_on_button is not a boolean: " + value);
        } else if (key == L"jump_threshold_px") {
            if (ParseInt(value, i) && i > 0) cfg.jumpThresholdPx = i;
            else cfg.warnings.push_back(L"jump_threshold_px must be > 0: " + value);
        } else if (key == L"recenter_tolerance_px") {
            if (ParseInt(value, i) && i >= 0) cfg.recenterTolerancePx = i;
            else cfg.warnings.push_back(L"recenter_tolerance_px must be >= 0: " + value);
        } else if (key == L"state_poll_interval_ms") {
            if (ParseU64(value, u) && u >= 1 && u <= 1000) cfg.statePollIntervalMs = static_cast<unsigned>(u);
            else cfg.warnings.push_back(L"state_poll_interval_ms must be 1..1000: " + value);
        } else if (key == L"log_state_changes") {
            if (ParseBool(value, b)) cfg.logStateChanges = b;
            else cfg.warnings.push_back(L"log_state_changes is not a boolean: " + value);
        } else if (key == L"console_status_interval_ms") {
            if (ParseU64(value, u) && u <= 60000) cfg.consoleStatusIntervalMs = static_cast<unsigned>(u);
            else cfg.warnings.push_back(L"console_status_interval_ms must be 0..60000: " + value);
        } else if (key == L"heartbeat_interval_ms") {
            if (ParseU64(value, u) && (u == 0 || (u >= 1000 && u <= 600000)))
                cfg.heartbeatIntervalMs = static_cast<unsigned>(u);
            else cfg.warnings.push_back(L"heartbeat_interval_ms must be 0 or 1000..600000: " + value);
        } else if (key == L"_comment" || key.rfind(L"_", 0) == 0) {
            // Keys starting with an underscore are documentation, ignored on purpose.
        } else {
            cfg.warnings.push_back(L"unknown config key ignored: " + key);
        }
    }

    return true;
}

} // namespace rmf
