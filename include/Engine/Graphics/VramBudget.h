#pragma once

#include <atomic>
#include <cstdint>

namespace engine {

// ---------------------------------------------------------------------------
// VramBudget
//
// Thread-safe accounting policy for device-local GPU memory, sized for
// the GTX 970 profile (see Core/HardwareProfile.h): the engine plans
// against a budget below the card's fast VRAM segment so allocations
// fail soft — visibly, in logs and tests — instead of over-committing
// VRAM and letting the driver evict resources unpredictably.
//
// This is the budgeting groundwork: allocation sites reserve against a
// shared budget before creating device-local resources and release on
// destruction. tryReserve() never blocks and never over-commits; the
// check-and-add is a single CAS loop, so concurrent reservers cannot
// race past the limit.
// ---------------------------------------------------------------------------
class VramBudget {
public:
    explicit VramBudget(uint64_t budgetBytes) : m_budgetBytes(budgetBytes) {}

    VramBudget(const VramBudget&) = delete;
    VramBudget& operator=(const VramBudget&) = delete;

    // Atomically reserves `bytes` if they fit in the remaining budget.
    [[nodiscard]] bool tryReserve(uint64_t bytes) {
        uint64_t used = m_usedBytes.load(std::memory_order_relaxed);
        while (true) {
            if (bytes > m_budgetBytes - (used <= m_budgetBytes ? used : m_budgetBytes)) {
                return false;
            }
            if (m_usedBytes.compare_exchange_weak(used, used + bytes, std::memory_order_acq_rel)) {
                return true;
            }
        }
    }

    // Returns a previous reservation (saturating at zero).
    void release(uint64_t bytes) {
        uint64_t used = m_usedBytes.load(std::memory_order_relaxed);
        while (true) {
            const uint64_t next = bytes >= used ? 0 : used - bytes;
            if (m_usedBytes.compare_exchange_weak(used, next, std::memory_order_acq_rel)) {
                return;
            }
        }
    }

    [[nodiscard]] uint64_t budgetBytes() const { return m_budgetBytes; }
    [[nodiscard]] uint64_t usedBytes() const { return m_usedBytes.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t remainingBytes() const {
        const uint64_t used = usedBytes();
        return used >= m_budgetBytes ? 0 : m_budgetBytes - used;
    }
    [[nodiscard]] double utilization() const {
        return m_budgetBytes == 0 ? 0.0
                                  : static_cast<double>(usedBytes()) / static_cast<double>(m_budgetBytes);
    }

private:
    uint64_t m_budgetBytes;
    std::atomic<uint64_t> m_usedBytes{0};
};

} // namespace engine
