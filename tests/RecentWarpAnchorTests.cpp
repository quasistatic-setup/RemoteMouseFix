#include "rmf/RecentWarpAnchor.h"

#include <cstdlib>

namespace {

POINT At(long x, long y) { return POINT{x, y}; }

void Check(bool ok) { if (!ok) std::abort(); }

void TestInsufficientSamples() {
    rmf::RecentWarpAnchor tracker;
    POINT anchor {};
    unsigned hits = 0, total = 0;
    for (int i = 0; i < 7; ++i) tracker.Add(At(380, 330));
    Check(!tracker.Stable(anchor, hits, total));
    Check(total == 7);
}

void TestStableGameWarp() {
    rmf::RecentWarpAnchor tracker;
    POINT anchor {};
    unsigned hits = 0, total = 0;
    for (int i = 0; i < 9; ++i) tracker.Add(At(380, 330));
    for (int i = 0; i < 3; ++i) tracker.Add(At(390 + i, 335));
    Check(tracker.Stable(anchor, hits, total));
    Check(anchor.x == 380 && anchor.y == 330);
    Check(hits == 9 && total == 12);
}

void TestScatteredRemoteSamplesRejected() {
    rmf::RecentWarpAnchor tracker;
    POINT anchor {};
    unsigned hits = 0, total = 0;
    for (int i = 0; i < 12; ++i) tracker.Add(At(380 + i, 330));
    Check(!tracker.Stable(anchor, hits, total));
}

void TestStaleHistoryExpires() {
    rmf::RecentWarpAnchor tracker;
    POINT anchor {};
    unsigned hits = 0, total = 0;
    for (int i = 0; i < 12; ++i) tracker.Add(At(380, 330));
    for (int i = 0; i < 12; ++i) tracker.Add(At(537, 276));
    Check(tracker.Stable(anchor, hits, total));
    Check(anchor.x == 537 && anchor.y == 276);
    Check(hits == 12 && total == 12);
    tracker.Reset();
    Check(!tracker.Stable(anchor, hits, total));
}

void TestOwnCorrectionTargetNeverBecomesAnchor() {
    rmf::RecentWarpAnchor tracker;
    POINT candidate {};
    unsigned hits = 0, total = 0;
    const POINT ownTarget = At(922, 568); // 20 px from the real warp (942,568)
    for (std::uint64_t t = 1000; t < 1600; t += 10) {
        Check(!tracker.Observe(ownTarget, 1, ownTarget, t, 900));
    }
    Check(!tracker.Stable(candidate, hits, total));
}

void TestRecentOutputQuarantinesSamples() {
    rmf::RecentWarpAnchor tracker;
    POINT candidate {};
    unsigned hits = 0, total = 0;
    const POINT gameWarp = At(847, 573);
    const POINT ownTarget = At(922, 568);
    // Even a perfectly stable candidate is rejected while outputs continue.
    for (std::uint64_t t = 1000; t < 1150; t += 10) {
        Check(!tracker.Observe(gameWarp, 5, ownTarget, t, 1000));
    }
    Check(!tracker.Stable(candidate, hits, total));
    // Once the output is quiet, the *different* true recenter may be learned.
    for (std::uint64_t t = 1170; t < 1290; t += 10) {
        Check(tracker.Observe(gameWarp, 5, ownTarget, t, 1000));
    }
    Check(tracker.Stable(candidate, hits, total));
    Check(candidate.x == 847 && candidate.y == 573);
}

void TestOutputWhileCollectingClearsOldConsensus() {
    rmf::RecentWarpAnchor tracker;
    POINT candidate {};
    unsigned hits = 0, total = 0;
    const POINT gameWarp = At(847, 573);
    const POINT ownTarget = At(922, 568);
    for (std::uint64_t t = 1200; t < 1270; t += 10)
        tracker.Observe(gameWarp, 2, ownTarget, t, 1000);
    Check(!tracker.Stable(candidate, hits, total));
    Check(!tracker.Observe(ownTarget, 3, ownTarget, 1270, 1270));
    Check(!tracker.Stable(candidate, hits, total));
    for (std::uint64_t t = 1440; t < 1560; t += 10)
        tracker.Observe(gameWarp, 3, ownTarget, t, 1270);
    Check(tracker.Stable(candidate, hits, total));
    Check(candidate.x == 847);
}

} // namespace

int main() {
    TestInsufficientSamples();
    TestStableGameWarp();
    TestScatteredRemoteSamplesRejected();
    TestStaleHistoryExpires();
    TestOwnCorrectionTargetNeverBecomesAnchor();
    TestRecentOutputQuarantinesSamples();
    TestOutputWhileCollectingClearsOldConsensus();
    return 0;
}
