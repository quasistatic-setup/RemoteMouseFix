#include "rmf/ProcessInfo.h"
#include "rmf/WinCompat.h"

#include <algorithm>

namespace rmf {
namespace {

std::wstring FileNameOf(const std::wstring& path) {
    const std::size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() &&
           CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                b.c_str(), static_cast<int>(b.size()),
                                TRUE) == CSTR_EQUAL;
}

// Process creation time, used to notice PID reuse.
bool GetProcessCreateTime(DWORD pid, FILETIME& out) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        return false;
    }
    FILETIME exitTime {}, kernelTime {}, userTime {};
    const bool ok = GetProcessTimes(proc, &out, &exitTime, &kernelTime, &userTime) != 0;
    CloseHandle(proc);
    return ok;
}

} // namespace

ProcessNameCache::ProcessNameCache(std::wstring targetProcessName)
    : target_(std::move(targetProcessName)) {}

ProcessNameCache::Entry& ProcessNameCache::Resolve(DWORD pid) {
    auto it = cache_.find(pid);
    if (it != cache_.end()) {
        Entry& cached = it->second;
        // Detect PID reuse: if the creation time moved, the PID belongs to a new
        // process and the cached name is stale.
        if (cached.haveCreateTime) {
            FILETIME now {};
            if (GetProcessCreateTime(pid, now)) {
                if (now.dwLowDateTime == cached.createTime.dwLowDateTime &&
                    now.dwHighDateTime == cached.createTime.dwHighDateTime) {
                    return cached;
                }
            } else {
                // Process gone or no longer queryable; fall through and re-resolve.
            }
        } else {
            return cached;
        }
        cache_.erase(it);
    }

    Entry entry;
    std::wstring path;
    bool accessDenied = false;
    if (QueryProcessImagePath(pid, path, accessDenied)) {
        entry.name     = FileNameOf(path);
        entry.isTarget = EqualsIgnoreCase(entry.name, target_);
        entry.haveCreateTime = GetProcessCreateTime(pid, entry.createTime);
    } else if (accessDenied) {
        sawAccessDenied_ = true;
        entry.name = L"<access-denied>";
    } else {
        entry.name = L"<unknown>";
    }

    // Keep the cache from growing without bound across a long session; PID churn on a
    // busy desktop is slow, so a hard reset is cheaper than LRU bookkeeping.
    if (cache_.size() > 512) {
        cache_.clear();
    }
    return cache_.emplace(pid, std::move(entry)).first->second;
}

const std::wstring& ProcessNameCache::NameForPid(DWORD pid) {
    if (pid == 0) {
        return empty_;
    }
    return Resolve(pid).name;
}

bool ProcessNameCache::IsTargetPid(DWORD pid) {
    if (pid == 0) {
        return false;
    }
    return Resolve(pid).isTarget;
}

} // namespace rmf
