#pragma once

#include "rmf/EventRecord.h"

#include <atomic>
#include <array>
#include <cstddef>

namespace rmf {

// Single-producer / single-consumer lock-free ring buffer.
//
// Producer: the WH_MOUSE_LL callback. It must never block, so Push() only ever does
// two atomic loads, a store and a release, and drops the event when the ring is full
// rather than waiting.
// Consumer: the writer thread, which formats and writes to disk.
template <std::size_t Capacity>
class EventQueue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    // Called from the hook thread only.
    bool Push(const RawEvent& ev) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (head - tail >= Capacity) {
            dropped_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        slots_[head & kMask] = ev;
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // Called from the writer thread only.
    bool Pop(RawEvent& out) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (head_.load(std::memory_order_acquire) == tail) {
            return false;
        }
        out = slots_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    std::uint64_t Dropped() const { return dropped_.load(std::memory_order_relaxed); }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    std::array<RawEvent, Capacity> slots_ {};
    std::atomic<std::size_t> head_ {0};
    std::atomic<std::size_t> tail_ {0};
    std::atomic<std::uint64_t> dropped_ {0};
};

// 16384 events is roughly 30 s of continuous 500 Hz mouse movement, far more head room
// than the writer thread needs even when the log file rotates.
using MouseEventQueue = EventQueue<16384>;

} // namespace rmf
