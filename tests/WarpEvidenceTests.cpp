#include "rmf/WarpEvidence.h"

#include <cstdlib>
#include <cstdint>

namespace {
constexpr RECT kClient {100, 100, 1100, 800};
constexpr RECT kScreen {0, 0, 1920, 1080};

void Check(bool ok) { if (!ok) std::abort(); }
POINT P(long x, long y) { return POINT{x, y}; }

bool Step(rmf::WarpEvidence& detector, POINT cursor, POINT remote, unsigned seq,
          std::uint64_t ms, std::uint64_t own = 0,
          POINT lastOwn = POINT{0, 0}, std::uint64_t lastOwnMs = 0,
          bool ownInHold = false) {
    POINT verified {};
    unsigned returns = 0;
    return detector.Observe(cursor, kClient, kScreen,
        rmf::RemotePointerSample{remote, seq, ms}, ms,
        own, lastOwn, lastOwnMs, ownInHold, verified, returns);
}

void TestHiddenNoWarpInputPasses() {
    rmf::WarpEvidence detector;
    for (unsigned i = 1; i < 200; ++i) {
        const POINT cursor = P(300 + static_cast<long>(i * 2),
                               300 + static_cast<long>(i % 10));
        Check(!Step(detector, cursor, cursor, i, 1000 + i * 16));
    }
}

void TestStationaryNoWarpDoesNotProveRecenter() {
    rmf::WarpEvidence detector;
    // Cursor remains inside the client even as the remote updates. A naive
    // 5/5 stable-anchor learner incorrectly accepted this in Probe B.
    for (unsigned i = 1; i < 35; ++i) {
        Check(!Step(detector, P(500, 450),
                    P(600 + static_cast<long>(i * 3), 650),
                    i, 1000 + i * 16));
    }
}

void TestIntentionalReturnWithRemoteToSamePointDoesNotCount() {
    rmf::WarpEvidence detector;
    // Moving the cursor back manually is a remote event, NOT a hidden game warp.
    for (unsigned i = 1; i < 60; ++i) {
        const POINT cursor = i % 2 == 0 ? P(500, 450) : P(700, 450);
        Check(!Step(detector, cursor, cursor, i, 1000 + i * 16));
    }
}

void TestRepeatedUnexplainedWarpArmsGate() {
    rmf::WarpEvidence detector;
    const POINT anchor = P(500, 450);
    Check(!Step(detector, anchor, P(650, 450), 1, 1000));
    Check(!Step(detector, P(660, 450), P(660, 450), 2, 1020));
    Check(!Step(detector, anchor, P(720, 450), 3, 1040));
    Check(!Step(detector, P(730, 450), P(730, 450), 4, 1060));
    Check(!Step(detector, anchor, P(790, 450), 5, 1080));
    Check(!Step(detector, anchor, P(810, 450), 6, 1100));
    Check(Step(detector, anchor, P(850, 450), 7, 1120));
    // Once confirmed, the same candidate isn't announced every sample.
    Check(!Step(detector, anchor, P(870, 450), 8, 1140));
}

void TestOwnOutputCannotManufactureWarp() {
    rmf::WarpEvidence detector;
    const POINT anchor = P(500, 450);
    for (unsigned i = 1; i <= 50; ++i) {
        const POINT c = i % 2 == 0 ? anchor : P(650, 450);
        Check(!Step(detector, c, P(900, 450), i, 1000 + i * 16,
                    i, anchor, 1000 + i * 16, true));
    }
}

void TestScreenBoundaryDoesNotCount() {
    rmf::WarpEvidence detector;
    const POINT pinned = P(5, 450);
    for (unsigned i = 1; i <= 80; ++i) {
        Check(!Step(detector, pinned, P(800 + static_cast<long>(i), 450),
                    i, 1000 + i * 16));
    }
}

void TestResetRequiresFreshEvidence() {
    rmf::WarpEvidence detector;
    const POINT anchor = P(500, 450);
    Step(detector, anchor, P(650, 450), 1, 1000);
    Step(detector, P(660, 450), P(660, 450), 2, 1020);
    Step(detector, anchor, P(720, 450), 3, 1040);
    detector.Reset();
    Check(!Step(detector, anchor, P(800, 450), 4, 1060));
    Check(!Step(detector, anchor, P(820, 450), 5, 1080));
    Check(!Step(detector, anchor, P(840, 450), 6, 1100));
}

void TestOwnQuietInterval() {
    rmf::WarpEvidence detector;
    const POINT anchor = P(500, 450);
    const POINT ownTarget = P(520, 450);
    for (unsigned i = 1; i <= 8; ++i) {
        Check(!Step(detector, anchor, P(700 + i * 10, 450), i,
                    1000 + i * 16, 3, ownTarget, 1000, true));
    }
}
} // namespace

int main() {
    TestHiddenNoWarpInputPasses();
    TestStationaryNoWarpDoesNotProveRecenter();
    TestIntentionalReturnWithRemoteToSamePointDoesNotCount();
    TestRepeatedUnexplainedWarpArmsGate();
    TestOwnOutputCannotManufactureWarp();
    TestScreenBoundaryDoesNotCount();
    TestResetRequiresFreshEvidence();
    TestOwnQuietInterval();
    return 0;
}
