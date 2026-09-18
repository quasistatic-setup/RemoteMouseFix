#pragma once

#include <windows.h>
#include <string>
#include <unordered_map>

namespace rmf {

// True when at least one running process carries this image file name, compared
// case-insensitively. Used by the status panel to tell "the game has not been started
// yet" apart from "the game runs, but another window is in front" - a distinction the
// foreground state alone cannot make, and the one users read wrong most often.
//
// Walks the process list, so it belongs on a slow timer, never in the hook callback.
bool IsProcessRunning(const std::wstring& imageFileName);

// Caches PID -> image file name so the writer thread never pays for a process query
// twice, and so the hook thread can take a fast "is this even the target" decision.
//
// PIDs are reused by Windows, so entries are invalidated once the owning process is
// gone. That is checked lazily: an entry is re-resolved when its recorded start time
// no longer matches.
class ProcessNameCache {
public:
    explicit ProcessNameCache(std::wstring targetProcessName);

    // File name only (e.g. "Wow.exe"), or an empty string when it cannot be determined.
    const std::wstring& NameForPid(DWORD pid);

    // Case-insensitive comparison of the resolved name against the configured target.
    bool IsTargetPid(DWORD pid);

    // True when at least one process query failed with ACCESS_DENIED, which normally
    // means the target runs elevated while this tool does not.
    bool SawAccessDenied() const { return sawAccessDenied_; }

    const std::wstring& TargetProcessName() const { return target_; }

private:
    struct Entry {
        std::wstring name;
        bool         isTarget = false;
        FILETIME     createTime {0, 0};
        bool         haveCreateTime = false;
    };

    Entry& Resolve(DWORD pid);

    std::wstring target_;
    std::unordered_map<DWORD, Entry> cache_;
    std::wstring empty_;
    bool sawAccessDenied_ = false;
};

} // namespace rmf
