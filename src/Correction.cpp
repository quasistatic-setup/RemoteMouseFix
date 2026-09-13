#include "rmf/Correction.h"

#include <algorithm>
#include <cwctype>

namespace rmf {

const wchar_t* CorrectionModeName(CorrectionMode mode) {
    switch (mode) {
        case CorrectionMode::Off:       return L"off";
        case CorrectionMode::Absolute:  return L"absolute";
        case CorrectionMode::Relative:  return L"relative";
    }
    return L"?";
}

bool ParseCorrectionMode(const std::wstring& text, CorrectionMode& out) {
    std::wstring lower;
    lower.reserve(text.size());
    for (const wchar_t c : text) {
        lower.push_back(static_cast<wchar_t>(std::towlower(c)));
    }
    if (lower == L"off")          { out = CorrectionMode::Off;       return true; }
    if (lower == L"absolute")     { out = CorrectionMode::Absolute;  return true; }
    if (lower == L"relative")     { out = CorrectionMode::Relative;  return true; }
    return false;
}

CorrectionMode CorrectionState::SetMode(CorrectionMode next) {
    const auto previous = static_cast<CorrectionMode>(
        mode.exchange(static_cast<std::uint8_t>(next)));
    generation.fetch_add(1);
    pendingEcho.store(0);
    return previous;
}

CorrectionMode CorrectionState::ForceOff() {
    std::uint8_t current = mode.load();
    while (current != 0) {
        if (mode.compare_exchange_weak(current, 0)) {
            generation.fetch_add(1);
            pendingEcho.store(0);
            return static_cast<CorrectionMode>(current);
        }
    }
    return CorrectionMode::Off;
}

namespace {

// Maps a pixel offset inside the virtual desktop onto SendInput's normalised 0..65535
// range, with the end points landing exactly on the first and last pixel.
LONG NormaliseAbsolute(long offset, int extent) {
    if (extent <= 1) {
        return 0;
    }
    const long long scaled = (static_cast<long long>(offset) * 65535 + (extent - 1) / 2) / (extent - 1);
    return static_cast<LONG>(scaled);
}

} // namespace

bool ApplyRemoteDelta(CorrectionMode mode, long dx, long dy, POINT& outTarget) {
    POINT current {};
    if (!GetCursorPos(&current)) {
        return false;
    }
    outTarget = current;

    switch (mode) {
        case CorrectionMode::Absolute: {
            // Relative to wherever the cursor is now. While the game holds the mouse that
            // is its anchor, so the game reads exactly (dx, dy). If the game is not warping
            // after all, the pointer just moves by (dx, dy) and stays usable.
            const int left   = GetSystemMetrics(SM_XVIRTUALSCREEN);
            const int top    = GetSystemMetrics(SM_YVIRTUALSCREEN);
            const int width  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            const int height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            if (width <= 1 || height <= 1) {
                return false;
            }
            outTarget.x = std::clamp(current.x + dx, static_cast<long>(left), static_cast<long>(left + width - 1));
            outTarget.y = std::clamp(current.y + dy, static_cast<long>(top),  static_cast<long>(top + height - 1));

            INPUT input {};
            input.type           = INPUT_MOUSE;
            input.mi.dx          = NormaliseAbsolute(outTarget.x - left, width);
            input.mi.dy          = NormaliseAbsolute(outTarget.y - top, height);
            input.mi.dwFlags     = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
            input.mi.dwExtraInfo = kOwnInputSignature;
            return SendInput(1, &input, sizeof(INPUT)) == 1;
        }
        case CorrectionMode::Relative: {
            INPUT input {};
            input.type           = INPUT_MOUSE;
            input.mi.dx          = dx;
            input.mi.dy          = dy;
            input.mi.dwFlags     = MOUSEEVENTF_MOVE;
            input.mi.dwExtraInfo = kOwnInputSignature;
            return SendInput(1, &input, sizeof(INPUT)) == 1;
        }
        case CorrectionMode::Off:
            return true;
    }
    return false;
}

void AnchorLearner::Add(POINT screenPos) {
    const std::pair<long, long> key {screenPos.x, screenPos.y};
    if (counts_.size() >= kMaxDistinct && counts_.find(key) == counts_.end()) {
        return; // a phase this scattered has no anchor worth learning
    }
    ++counts_[key];
    ++total_;
}

bool AnchorLearner::Estimate(POINT& outPos, unsigned& outHits, unsigned& outTotal) const {
    unsigned bestHits = 0;
    std::pair<long, long> best {0, 0};
    for (const auto& [pos, hits] : counts_) {
        if (hits > bestHits) {
            bestHits = hits;
            best     = pos;
        }
    }

    outHits  = bestHits;
    outTotal = total_;
    const bool dominant = bestHits >= kMinSamples &&
                          bestHits * 100u >= total_ * kMinSharePercent;
    if (dominant) {
        outPos.x = best.first;
        outPos.y = best.second;
    }

    return dominant;
}

bool AnchorLearner::Finish(POINT& outPos, unsigned& outHits, unsigned& outTotal) {
    const bool dominant = Estimate(outPos, outHits, outTotal);
    counts_.clear();
    total_ = 0;
    return dominant;
}

} // namespace rmf
