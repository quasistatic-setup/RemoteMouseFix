#pragma once

#include <windows.h>
#include <cstdint>
#include <fstream>
#include <string>

namespace rmf {

// Size-rotating UTF-8 text log.
//
// Deliberately plain text, one record per line, fixed columns: the whole point of
// Phase 1 is that a human can read the file and see what happened around a click.
class Logger {
public:
    Logger() = default;
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    // Creates `directory` if needed and opens the first file.
    // `maxBytes` is the rotation threshold, `maxFiles` the number of files to keep.
    bool Open(const std::wstring& directory, const std::wstring& prefix,
              std::uint64_t maxBytes, unsigned maxFiles, std::wstring& outError);

    void WriteLine(const std::wstring& line);
    void Flush();
    void Close();

    const std::wstring& CurrentPath() const { return currentPath_; }
    std::uint64_t LinesWritten() const { return linesWritten_; }
    unsigned RotationCount() const { return rotations_; }

private:
    bool OpenNewFile(std::wstring& outError);
    void PruneOldFiles();

    std::wstring  directory_;
    std::wstring  prefix_;
    std::wstring  currentPath_;
    std::ofstream out_;
    std::uint64_t bytesWritten_ = 0;
    std::uint64_t linesWritten_ = 0;
    std::uint64_t maxBytes_     = 0;
    unsigned      maxFiles_     = 1;
    unsigned      rotations_    = 0;
};

} // namespace rmf
