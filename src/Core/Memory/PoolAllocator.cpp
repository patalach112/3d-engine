#include "Core/Memory/PoolAllocator.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <mutex>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
#endif

namespace engine::memory {
namespace {

constexpr size_t kPageBytes = 64 * 1024;
constexpr size_t kMaxPooledBytes = 4096;
// Pages are allocated 64-byte aligned and block sizes are powers of two,
// so any alignment up to a full cache line is honored by pooled blocks.
constexpr size_t kMaxPooledAlignment = 64;

// Block size classes: powers of two from 16 B to 4 KiB.
constexpr std::array<size_t, 9> kBlockSizes = {16, 32, 64, 128, 256, 512, 1024, 2048, 4096};

size_t classIndexFor(size_t bytes) {
    for (size_t i = 0; i < kBlockSizes.size(); ++i) {
        if (bytes <= kBlockSizes[i]) {
            return i;
        }
    }
    return kBlockSizes.size(); // not pooled
}

void* alignedAllocate(size_t bytes, size_t alignment) {
    const size_t rounded = ((bytes + alignment - 1) / alignment) * alignment;
#ifdef _WIN32
    return _aligned_malloc(rounded == 0 ? alignment : rounded, alignment);
#else
    return std::aligned_alloc(alignment, rounded == 0 ? alignment : rounded);
#endif
}

void alignedDeallocate(void* pointer) {
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

} // namespace

struct PoolCore::Impl {
    struct SizeClass {
        void* freeList = nullptr;
        size_t blocksInUse = 0;
        size_t blocksTotal = 0;
        size_t peakBlocksInUse = 0;
        std::vector<void*> pages; // 64-byte aligned raw pages, freed by ~Impl

        void addPage(size_t blockSize) {
            void* page = alignedAllocate(kPageBytes, kMaxPooledAlignment);
            const size_t blockCount = kPageBytes / blockSize;
            // Push every block onto the intrusive free list (each block's
            // first bytes store the next pointer).
            for (size_t i = 0; i < blockCount; ++i) {
                void* block = static_cast<std::byte*>(page) + i * blockSize;
                *static_cast<void**>(block) = freeList;
                freeList = block;
            }
            blocksTotal += blockCount;
            pages.push_back(page);
        }
    };

    ~Impl() {
        for (SizeClass& sizeClass : classes) {
            for (void* page : sizeClass.pages) {
                alignedDeallocate(page);
            }
        }
    }


    std::array<SizeClass, kBlockSizes.size()> classes;
    size_t fallbackAllocations = 0;
    mutable std::mutex mutex;
};

PoolCore::PoolCore() : m_impl(std::make_unique<Impl>()) {}
PoolCore::~PoolCore() = default;

void* PoolCore::allocate(size_t bytes, size_t alignment) {
    if (bytes == 0) {
        bytes = 1;
    }
    // A block's offset within its 64-byte aligned page is a multiple of
    // the block size, so sizing the class by max(bytes, alignment) keeps
    // every block aligned (block sizes are powers of two).
    const size_t classIndex = classIndexFor(std::max(bytes, alignment));
    if (classIndex >= kBlockSizes.size() || alignment > kMaxPooledAlignment) {
        void* pointer = alignedAllocate(bytes, std::max(alignment, sizeof(void*)));
        if (pointer != nullptr) {
            std::lock_guard lock(m_impl->mutex);
            ++m_impl->fallbackAllocations;
        }
        return pointer;
    }

    std::lock_guard lock(m_impl->mutex);
    Impl::SizeClass& sizeClass = m_impl->classes[classIndex];
    if (sizeClass.freeList == nullptr) {
        sizeClass.addPage(kBlockSizes[classIndex]);
    }
    void* block = sizeClass.freeList;
    sizeClass.freeList = *static_cast<void**>(block);
    ++sizeClass.blocksInUse;
    sizeClass.peakBlocksInUse = std::max(sizeClass.peakBlocksInUse, sizeClass.blocksInUse);
    return block;
}

void PoolCore::deallocate(void* pointer, size_t bytes, size_t alignment) noexcept {
    if (pointer == nullptr) {
        return;
    }
    if (bytes == 0) {
        bytes = 1;
    }
    const size_t classIndex = classIndexFor(std::max(bytes, alignment));
    if (classIndex >= kBlockSizes.size() || alignment > kMaxPooledAlignment) {
        {
            std::lock_guard lock(m_impl->mutex);
            if (m_impl->fallbackAllocations > 0) {
                --m_impl->fallbackAllocations;
            }
        }
        alignedDeallocate(pointer);
        return;
    }

    std::lock_guard lock(m_impl->mutex);
    Impl::SizeClass& sizeClass = m_impl->classes[classIndex];
    *static_cast<void**>(pointer) = sizeClass.freeList;
    sizeClass.freeList = pointer;
    if (sizeClass.blocksInUse > 0) {
        --sizeClass.blocksInUse;
    }
}

PoolStats PoolCore::stats() const {
    std::lock_guard lock(m_impl->mutex);
    PoolStats result;
    for (const Impl::SizeClass& sizeClass : m_impl->classes) {
        result.pageCount += sizeClass.pages.size();
        result.blocksInUse += sizeClass.blocksInUse;
        result.blocksReserved += sizeClass.blocksTotal;
        result.peakBlocksInUse += sizeClass.peakBlocksInUse;
    }
    result.fallbackAllocations = m_impl->fallbackAllocations;
    return result;
}

} // namespace engine::memory
