#pragma once

#include <windows.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace rmf {

// Recent sampler readings only. This is NOT proof of a game warp on its own:
// a motionless remote cursor can also produce a stable position.
// Callers must independently establish a cursor-recenter transition.
class RecentWarpAnchor {
public:
    void Reset() {
        count_ = 0;
        next_ = 0;
        observedOwnCount_ = 0;
    }

    // Keep only samples collected during a quiet interval without our SendInput
    // output. OWN cursor locations are not independent evidence of a game warp.
    // Returns true only when the sample was admitted to the consensus window.
    bool Observe(POINT point, std::uint64_t ownCount, POINT lastOwnTarget,
                 std::uint64_t nowMs, std::uint64_t lastOwnOutputMs) {
        const bool outputChanged = ownCount != observedOwnCount_;
        observedOwnCount_ = ownCount;
        const bool recentlyOutput = lastOwnOutputMs != 0 &&
            (nowMs < lastOwnOutputMs || nowMs - lastOwnOutputMs < 160);
        const bool atOwnTarget = ownCount != 0 &&
            std::labs(point.x - lastOwnTarget.x) <= 2 &&
            std::labs(point.y - lastOwnTarget.y) <= 2;
        if (outputChanged || recentlyOutput || atOwnTarget) {
            ClearSamples();
            return false;
        }
        Add(point);
        return true;
    }

    void Add(POINT point) {
        samples_[next_] = point;
        next_ = (next_ + 1) % samples_.size();
        if (count_ < samples_.size()) ++count_;
    }

    bool Stable(POINT& result, unsigned& hits, unsigned& total) const {
        hits = 0;
        total = static_cast<unsigned>(count_);
        if (count_ < 8) return false;

        for (std::size_t i = 0; i < count_; ++i) {
            unsigned matches = 0;
            for (std::size_t j = 0; j < count_; ++j) {
                if (samples_[i].x == samples_[j].x &&
                    samples_[i].y == samples_[j].y) ++matches;
            }
            if (matches > hits) {
                hits = matches;
                result = samples_[i];
            }
        }
        return hits >= 8 && hits * 100u >= total * 75u;
    }

private:
    void ClearSamples() {
        count_ = 0;
        next_ = 0;
    }

    std::array<POINT, 12> samples_ {};
    std::size_t count_ = 0;
    std::size_t next_ = 0;
    std::uint64_t observedOwnCount_ = 0;
};

} // namespace rmf
