#include "rmf/Logger.h"

#include <algorithm>
#include <vector>

namespace rmf {
namespace {

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.data(),
                                           static_cast<int>(text.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring TimestampForFileName() {
    SYSTEMTIME st {};
    GetLocalTime(&st);
    wchar_t buf[32];
    swprintf(buf, 32, L"%04u%02u%02u-%02u%02u%02u",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

bool EnsureDirectory(const std::wstring& dir) {
    if (dir.empty()) {
        return false;
    }
    const DWORD attrs = GetFileAttributesW(dir.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    // Create parents first so a nested log_directory works.
    const std::size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos && slash > 2) {
        EnsureDirectory(dir.substr(0, slash));
    }
    return CreateDirectoryW(dir.c_str(), nullptr) != 0 ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

} // namespace

Logger::~Logger() {
    Close();
}

bool Logger::Open(const std::wstring& directory, const std::wstring& prefix,
                  std::uint64_t maxBytes, unsigned maxFiles, std::wstring& outError) {
    directory_ = directory;
    prefix_    = prefix;
    maxBytes_  = maxBytes;
    maxFiles_  = std::max(1u, maxFiles);

    if (!EnsureDirectory(directory_)) {
        outError = L"cannot create log directory: " + directory_;
        return false;
    }
    return OpenNewFile(outError);
}

bool Logger::OpenNewFile(std::wstring& outError) {
    if (out_.is_open()) {
        out_.close();
    }

    // Second-resolution stamps can collide if a rotation happens inside the same
    // second, so a disambiguating suffix is appended when needed.
    const std::wstring stamp = TimestampForFileName();
    std::wstring candidate;
    for (unsigned attempt = 0; attempt < 1000; ++attempt) {
        candidate = directory_ + L"\\" + prefix_ + L"-" + stamp;
        if (attempt > 0) {
            candidate += L"-" + std::to_wstring(attempt);
        }
        candidate += L".log";
        if (GetFileAttributesW(candidate.c_str()) == INVALID_FILE_ATTRIBUTES) {
            break;
        }
    }

    out_.open(candidate.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
    if (!out_.is_open()) {
        outError = L"cannot open log file: " + candidate;
        return false;
    }

    currentPath_  = candidate;
    bytesWritten_ = 0;
    PruneOldFiles();
    return true;
}

void Logger::PruneOldFiles() {
    const std::wstring pattern = directory_ + L"\\" + prefix_ + L"-*.log";
    WIN32_FIND_DATAW data {};
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }

    std::vector<std::pair<ULONGLONG, std::wstring>> files;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }
        ULARGE_INTEGER when {};
        when.LowPart  = data.ftLastWriteTime.dwLowDateTime;
        when.HighPart = data.ftLastWriteTime.dwHighDateTime;
        files.emplace_back(when.QuadPart, data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);

    if (files.size() <= maxFiles_) {
        return;
    }

    // Oldest first, then delete everything beyond the retention count.
    std::sort(files.begin(), files.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    const std::size_t excess = files.size() - maxFiles_;
    for (std::size_t i = 0; i < excess; ++i) {
        const std::wstring full = directory_ + L"\\" + files[i].second;
        if (full != currentPath_) {
            DeleteFileW(full.c_str());
        }
    }
}

void Logger::WriteLine(const std::wstring& line) {
    if (!out_.is_open()) {
        return;
    }

    if (maxBytes_ > 0 && bytesWritten_ >= maxBytes_) {
        out_ << "# --- log rotated, size limit " << maxBytes_ << " bytes reached ---\r\n";
        out_.flush();
        std::wstring error;
        ++rotations_;
        if (!OpenNewFile(error)) {
            return;
        }
        out_ << "# --- continued from rotation #" << rotations_ << " ---\r\n";
        bytesWritten_ += 44;
    }

    const std::string utf8 = ToUtf8(line);
    // CRLF so the file opens cleanly in Notepad, which is where this will be read.
    out_.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    out_.write("\r\n", 2);
    bytesWritten_ += utf8.size() + 2;
    ++linesWritten_;
}

void Logger::Flush() {
    if (out_.is_open()) {
        out_.flush();
    }
}

void Logger::Close() {
    if (out_.is_open()) {
        out_.flush();
        out_.close();
    }
}

} // namespace rmf
