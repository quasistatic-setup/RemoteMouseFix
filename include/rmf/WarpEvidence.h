#pragma once

#include <windows.h>

#include <array>
#include <cstdint>
#include <cstdlib>

namespace rmf {

// Only a *repeated unexplained return* toward one interior location constitutes
// evidence that a hidden cursor is being recentered by the game. A hidden or
// motionless cursor alone is not evidence (MouseLookProbe mode B).
struct RemotePointerSample {
    POINT point {0, 0};
    std::uint64_t serial = 0;
    std::uint64_t tickMs = 0;
};

class WarpEvidence {
public:
    void Reset() {
        candidates_ = {};
        havePrevious_ = false;
        observedOwnCount_ = 0;
    }

    bool Observe(POINT cursor, RECT client, RECT desktop, RemotePointerSample remote,
                 std::uint64_t nowMs, std::uint64_t ownCount,
                 POINT lastOwnTarget, std::uint64_t lastOwnOutputMs,
                 bool ownOutputInHold, POINT& confirmed, unsigned& returns) {
        const bool ownChanged = ownCount != observedOwnCount_;
        observedOwnCount_ = ownCount;
        const bool recentOwn = ownOutputInHold && lastOwnOutputMs != 0 &&
            (nowMs < lastOwnOutputMs || nowMs - lastOwnOutputMs < 160);
        const bool ownTarget = ownOutputInHold &&
            Close(cursor, lastOwnTarget, 2);
        const bool recentlyRemote = remote.serial != 0 &&
            remote.tickMs <= nowMs && nowMs - remote.tickMs <= 80;
        const bool interior = Inside(cursor, client, 24) &&
            Inside(cursor, desktop, 24);

        // If our own SendInput might explain this position, the complete
        // hypothesis is tainted. Do not resume an old count later.
        if (ownChanged || recentOwn || ownTarget) {
            ClearCandidates();
        }

        bool verified = false;
        if (!ownChanged && !recentOwn && !ownTarget &&
            recentlyRemote && interior && Far(cursor, remote.point, 45)) {
            Candidate* slot = nullptr;
            for (Candidate& candidate : candidates_) {
                if (candidate.active && nowMs >= candidate.lastMs &&
                    nowMs - candidate.lastMs <= 280 &&
                    Close(candidate.anchor, cursor, 3)) {
                    slot = &candidate;
                    break;
                }
            }
            if (!slot) {
                for (Candidate& candidate : candidates_) {
                    if (!candidate.active || nowMs < candidate.lastMs ||
                        nowMs - candidate.lastMs > 280) {
                        slot = &candidate;
                        break;
                    }
                }
            }
            if (!slot) {
                slot = &candidates_[0];
                for (Candidate& candidate : candidates_) {
                    if (candidate.lastMs < slot->lastMs) slot = &candidate;
                }
            }
            const bool same = slot->active && nowMs >= slot->lastMs &&
                nowMs - slot->lastMs <= 280 &&
                Close(slot->anchor, cursor, 3);

            if (!same) {
                *slot = Candidate{};
                slot->active = true;
                slot->anchor = cursor;
                slot->firstMs = nowMs;
                slot->firstRemote = remote.point;
            }

            if (remote.serial != slot->lastSerial) {
                slot->lastSerial = remote.serial;
                slot->lastMs = nowMs;
                ++slot->hits;
                if (Far(slot->firstRemote, remote.point, 60)) {
                    slot->remoteMoved = true;
                }
                // This is the critical distinction from a stationary cursor:
                // we must actually SEE the cursor away from this same point,
                // then back while the latest remote input aims elsewhere.
                if (havePrevious_ && Far(previousCursor_, slot->anchor, 45)) {
                    ++slot->returns;
                }
                if (!slot->reported && slot->hits >= 5 && slot->returns >= 2 &&
                    slot->remoteMoved && nowMs - slot->firstMs >= 80) {
                    slot->reported = true;
                    confirmed = slot->anchor;
                    returns = slot->returns;
                    verified = true;
                }
            }
        }

        previousCursor_ = cursor;
        havePrevious_ = true;
        return verified;
    }

private:
    struct Candidate {
        bool active = false;
        bool reported = false;
        bool remoteMoved = false;
        POINT anchor {0, 0};
        POINT firstRemote {0, 0};
        std::uint64_t firstMs = 0;
        std::uint64_t lastMs = 0;
        std::uint64_t lastSerial = 0;
        unsigned hits = 0;
        unsigned returns = 0;
    };

    static bool Close(POINT a, POINT b, long delta) {
        return std::labs(a.x - b.x) <= delta && std::labs(a.y - b.y) <= delta;
    }
    static bool Far(POINT a, POINT b, long delta) {
        return std::labs(a.x - b.x) >= delta || std::labs(a.y - b.y) >= delta;
    }
    static bool Inside(POINT p, RECT rect, long margin) {
        return p.x >= rect.left + margin && p.x < rect.right - margin &&
               p.y >= rect.top + margin && p.y < rect.bottom - margin;
    }
    void ClearCandidates() { candidates_ = {}; }

    std::array<Candidate, 4> candidates_ {};
    POINT previousCursor_ {0, 0};
    bool havePrevious_ = false;
    std::uint64_t observedOwnCount_ = 0;
};

} // namespace rmf
