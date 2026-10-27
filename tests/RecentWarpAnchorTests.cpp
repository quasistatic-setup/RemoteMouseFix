#include "rmf/RecentWarpAnchor.h"

#include <cassert>

namespace {

POINT At(long x, long y) { return POINT{x, y}; }

void TestInsufficientSamples() {
    rmf::RecentWarpAnchor tracker;
    POINT anchor {};
    unsigned hits = 0, total = 0;
    for (int i = 0; i < 7; ++i) tracker.Add(At(380, 330));
    assert(!tracker.Stable(anchor, hits, total));
    assert(total == 7);
}

void TestStableGameWarp() {
    rmf::RecentWarpAnchor tracker;
    POINT anchor {};
    unsigned hits = 0, total = 0;
    for (int i = 0; i < 9; ++i) tracker.Add(At(380, 330));
    for (int i = 0; i < 3; ++i) tracker.Add(At(390 + i, 335));
    assert(tracker.Stable(anchor, hits, total));
    assert(anchor.x == 380 && anchor.y == 330);
    assert(hits == 9 && total == 12);
}

void TestScatteredRemoteSamplesRejected() {
    rmf::RecentWarpAnchor tracker;
    POINT anchor {};
    unsigned hits = 0, total = 0;
    for (int i = 0; i < 12; ++i) tracker.Add(At(380 + i, 330));
    assert(!tracker.Stable(anchor, hits, total));
}

void TestStaleHistoryExpires() {
    rmf::RecentWarpAnchor tracker;
    POINT anchor {};
    unsigned hits = 0, total = 0;
    for (int i = 0; i < 12; ++i) tracker.Add(At(380, 330));
    for (int i = 0; i < 12; ++i) tracker.Add(At(537, 276));
    assert(tracker.Stable(anchor, hits, total));
    assert(anchor.x == 537 && anchor.y == 276);
    assert(hits == 12 && total == 12);
    tracker.Reset();
    assert(!tracker.Stable(anchor, hits, total));
}

} // namespace

int main() {
    TestInsufficientSamples();
    TestStableGameWarp();
    TestScatteredRemoteSamplesRejected();
    TestStaleHistoryExpires();
    return 0;
}
