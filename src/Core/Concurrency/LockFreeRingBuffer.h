#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

namespace engine::concurrency {

// ---------------------------------------------------------------------------
// LockFreeRingBuffer<T>
//
// Bounded multi-producer / multi-consumer (MPMC) lock-free ring buffer,
// after Vyukov's sequence-number queue design. Every cell carries a
// sequence stamp: producers claim enqueue positions with a CAS, publish
// the value with a release store to the cell's stamp, and consumers claim
// dequeue positions the same way, acquiring the stamp before moving the
// value out. The single-producer/single-consumer case is just the
// degenerate, contention-free use of the same queue.
//
// Properties:
//   * tryPush never blocks; it returns false only when the buffer is
//     full, and a failed push never moves from the caller's value.
//   * tryPop never blocks; it returns false only when the buffer is
//     empty.
//   * The enqueue and dequeue positions live on separate 64-byte cache
//     lines (alignas below), so producers and consumers hammering the
//     two indices never false-share.
//   * Capacity is rounded up to a power of two (minimum 2) so position
//     -> cell mapping is a mask, not a division.
//   * No mutex anywhere on the push/pop paths; all synchronization is
//     std::atomic with explicit acquire/release ordering.
//
// T must be default-constructible and assignable from T&/T&& (std::function,
// integers, and the logger's record type all qualify).
// ---------------------------------------------------------------------------
template <typename T>
class LockFreeRingBuffer {
    static_assert(std::is_default_constructible_v<T>, "LockFreeRingBuffer requires a default-constructible T");
    static_assert(std::is_assignable_v<T&, T&&>, "LockFreeRingBuffer requires a move-assignable T");

public:
    explicit LockFreeRingBuffer(size_t requestedCapacity)
        : m_capacity(normalizeCapacity(requestedCapacity)),
          m_mask(m_capacity - 1),
          m_cells(std::make_unique<Cell[]>(m_capacity)) {
        for (size_t i = 0; i < m_capacity; ++i) {
            m_cells[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    LockFreeRingBuffer(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer& operator=(const LockFreeRingBuffer&) = delete;
    LockFreeRingBuffer(LockFreeRingBuffer&&) = delete;
    LockFreeRingBuffer& operator=(LockFreeRingBuffer&&) = delete;

    // Claims the next enqueue position and publishes `value` into its
    // cell. Returns false (leaving `value` untouched) when full.
    template <typename Value>
    [[nodiscard]] bool tryPush(Value&& value) {
        size_t position = m_enqueuePosition.load(std::memory_order_relaxed);
        while (true) {
            Cell& cell = m_cells[position & m_mask];
            const size_t sequence = cell.sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<int64_t>(sequence) - static_cast<int64_t>(position);
            if (difference == 0) {
                if (m_enqueuePosition.compare_exchange_weak(position, position + 1,
                                                            std::memory_order_relaxed)) {
                    cell.value = std::forward<Value>(value);
                    // Release-publish: the value write above happens-before
                    // any consumer that acquires this sequence stamp.
                    cell.sequence.store(position + 1, std::memory_order_release);
                    return true;
                }
                // CAS failed: `position` was refreshed by the exchange.
            } else if (difference < 0) {
                return false; // full
            } else {
                position = m_enqueuePosition.load(std::memory_order_relaxed);
            }
        }
    }

    // Claims the next dequeue position and moves its value into `out`.
    // Returns false when empty.
    [[nodiscard]] bool tryPop(T& out) {
        size_t position = m_dequeuePosition.load(std::memory_order_relaxed);
        while (true) {
            Cell& cell = m_cells[position & m_mask];
            const size_t sequence = cell.sequence.load(std::memory_order_acquire);
            const auto difference = static_cast<int64_t>(sequence) - static_cast<int64_t>(position + 1);
            if (difference == 0) {
                if (m_dequeuePosition.compare_exchange_weak(position, position + 1,
                                                            std::memory_order_relaxed)) {
                    out = std::move(cell.value);
                    // Hand the cell back to producers for the next lap.
                    cell.sequence.store(position + m_capacity, std::memory_order_release);
                    return true;
                }
            } else if (difference < 0) {
                return false; // empty
            } else {
                position = m_dequeuePosition.load(std::memory_order_relaxed);
            }
        }
    }

    [[nodiscard]] size_t capacity() const { return m_capacity; }

    // Snapshot queries (exact when no operations are in flight, a close
    // approximation under concurrency — intended for flush/wait logic).
    [[nodiscard]] bool emptyApprox() const {
        return m_enqueuePosition.load(std::memory_order_acquire) ==
               m_dequeuePosition.load(std::memory_order_acquire);
    }

    [[nodiscard]] size_t sizeApprox() const {
        const size_t enqueued = m_enqueuePosition.load(std::memory_order_acquire);
        const size_t dequeued = m_dequeuePosition.load(std::memory_order_acquire);
        return enqueued >= dequeued ? enqueued - dequeued : 0;
    }

private:
    struct Cell {
        std::atomic<size_t> sequence{0};
        T value{};
    };

    static size_t normalizeCapacity(size_t requested) {
        size_t capacity = 2;
        while (capacity < requested && capacity <= (SIZE_MAX / 2)) {
            capacity <<= 1;
        }
        return capacity;
    }

    size_t m_capacity;
    size_t m_mask;
    std::unique_ptr<Cell[]> m_cells;

    // Hot indices, each isolated on its own cache line: producers CAS the
    // enqueue position, consumers CAS the dequeue position, and neither
    // side's traffic invalidates the other's line. alignas(64) on both
    // members guarantees at least one full line of separation.
    alignas(64) std::atomic<size_t> m_enqueuePosition{0};
    alignas(64) std::atomic<size_t> m_dequeuePosition{0};
};

} // namespace engine::concurrency
