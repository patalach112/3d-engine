#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <utility>

namespace engine::memory {

// Snapshot of a pool's memory usage, for logging and tests.
struct PoolStats {
    size_t pageCount = 0;          // large pages carved into blocks
    size_t blocksInUse = 0;        // blocks currently handed out
    size_t blocksReserved = 0;     // blocks carved across all pages
    size_t fallbackAllocations = 0; // live oversized/over-aligned allocations
    size_t peakBlocksInUse = 0;
};

// ---------------------------------------------------------------------------
// PoolCore
//
// Shared storage behind PoolAllocator. Memory is served from large pages
// (64 KiB) that are carved into fixed-size blocks of several size classes
// (16 B .. 4 KiB). Freed blocks go onto an intrusive free list and are
// reused immediately, which keeps high-frequency, similarly-sized
// allocations (ECS component storage) free of heap fragmentation and
// gives them predictable, cache-friendly addresses.
//
// Oversized (> 4 KiB) or over-aligned requests fall back to aligned global
// allocation. All pages are released when the last allocator referencing
// this core dies (RAII). Thread-safe: every operation locks internally,
// so worker threads may allocate concurrently.
// ---------------------------------------------------------------------------
class PoolCore {
public:
    PoolCore();
    ~PoolCore();

    PoolCore(const PoolCore&) = delete;
    PoolCore& operator=(const PoolCore&) = delete;

    void* allocate(size_t bytes, size_t alignment);
    void deallocate(void* pointer, size_t bytes, size_t alignment) noexcept;

    [[nodiscard]] PoolStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

// ---------------------------------------------------------------------------
// PoolAllocator<T>
//
// STL-compatible allocator facade over a shared PoolCore. Copies and
// rebound allocators (other T) share the same core, which is exactly the
// semantics EnTT needs: every storage of one registry draws from one pool.
// ---------------------------------------------------------------------------
template <typename T>
class PoolAllocator {
public:
    using value_type = T;
    using size_type = size_t;
    using difference_type = std::ptrdiff_t;

    template <typename U>
    struct rebind {
        using other = PoolAllocator<U>;
    };

    PoolAllocator() : m_core(std::make_shared<PoolCore>()) {}
    explicit PoolAllocator(std::shared_ptr<PoolCore> core) : m_core(std::move(core)) {}

    template <typename U>
    PoolAllocator(const PoolAllocator<U>& other) : m_core(other.core()) {}

    [[nodiscard]] T* allocate(size_t count) {
        if (count > static_cast<size_t>(-1) / sizeof(T)) {
            throw std::bad_alloc();
        }
        void* pointer = m_core->allocate(count * sizeof(T), alignof(T));
        if (pointer == nullptr) {
            throw std::bad_alloc();
        }
        return static_cast<T*>(pointer);
    }

    void deallocate(T* pointer, size_t count) noexcept {
        m_core->deallocate(pointer, count * sizeof(T), alignof(T));
    }

    [[nodiscard]] std::shared_ptr<PoolCore> core() const { return m_core; }
    [[nodiscard]] PoolStats stats() const { return m_core->stats(); }

    template <typename U>
    bool operator==(const PoolAllocator<U>& other) const {
        return m_core == other.core();
    }
    template <typename U>
    bool operator!=(const PoolAllocator<U>& other) const {
        return !(*this == other);
    }

private:
    std::shared_ptr<PoolCore> m_core;
};

} // namespace engine::memory
