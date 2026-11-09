#include "Graphics/RenderQueue.h"

#include "Core/Jobs/JobSystem.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace engine {
namespace {

bool keyLess(const DrawItem& lhs, const DrawItem& rhs) {
    return lhs.sortKey < rhs.sortKey;
}

} // namespace

uint64_t RenderQueue::makeSortKey(uint32_t materialId, float viewDepth) {
    const float clampedDepth = viewDepth > 0.0f ? viewDepth : 0.0f;
    const uint32_t depthBits = std::bit_cast<uint32_t>(clampedDepth);
    return (static_cast<uint64_t>(materialId) << 32) | depthBits;
}

void RenderQueue::reserve(size_t capacity) {
    m_items.reserve(capacity);
    m_scratch.reserve(capacity);
}

void RenderQueue::ensureScratch() {
    if (m_scratch.size() < m_items.size()) {
        m_scratch.resize(m_items.size());
    }
}

void RenderQueue::sort() {
    const size_t count = m_items.size();
    if (count < 2) {
        return;
    }
    ensureScratch();

    // Stable LSD radix sort: 8 counting-sort passes over the key bytes,
    // least significant first. An even number of passes leaves the result
    // back in m_items. O(n) time, no comparisons, no allocations.
    std::vector<DrawItem>* source = &m_items;
    std::vector<DrawItem>* destination = &m_scratch;
    std::array<size_t, 256> offsets{};
    for (unsigned pass = 0; pass < 8; ++pass) {
        offsets.fill(0);
        const unsigned shift = pass * 8;
        for (const DrawItem& item : *source) {
            ++offsets[(item.sortKey >> shift) & 0xFFu];
        }
        size_t running = 0;
        for (size_t& offset : offsets) {
            const size_t bucket = offset;
            offset = running;
            running += bucket;
        }
        for (const DrawItem& item : *source) {
            (*destination)[offsets[(item.sortKey >> shift) & 0xFFu]++] = item;
        }
        std::swap(source, destination);
    }
}

void RenderQueue::sort(JobSystem* jobs, size_t parallelMinItems) {
    const size_t count = m_items.size();
    if (jobs == nullptr || count < parallelMinItems || count < 2) {
        sort();
        return;
    }
    ensureScratch();

    // Phase 1: sort segments in parallel (one segment per batch).
    const size_t segmentCount = std::min(count, static_cast<size_t>(std::max(jobs->workerCount(), 1u)) * 4);
    const size_t segmentSize = (count + segmentCount - 1) / segmentCount;
    const size_t segments = (count + segmentSize - 1) / segmentSize;
    jobs->parallelFor(segments, 1, [this, segmentSize, count](size_t begin, size_t end) {
        for (size_t segment = begin; segment < end; ++segment) {
            const size_t first = segment * segmentSize;
            const size_t last = std::min(first + segmentSize, count);
            std::sort(m_items.begin() + static_cast<std::ptrdiff_t>(first),
                      m_items.begin() + static_cast<std::ptrdiff_t>(last), keyLess);
        }
    });

    // Phase 2: stable merge stages, ping-ponging between the item buffer
    // and the preallocated scratch buffer. Each merge writes a disjoint
    // destination range, so stages parallelize without locks.
    DrawItem* source = m_items.data();
    DrawItem* destination = m_scratch.data();
    for (size_t width = segmentSize; width < count; width *= 2) {
        const size_t mergeCount = (count + 2 * width - 1) / (2 * width);
        jobs->parallelFor(mergeCount, 1, [source, destination, width, count](size_t begin, size_t end) {
            for (size_t merge = begin; merge < end; ++merge) {
                const size_t first = merge * 2 * width;
                const size_t middle = std::min(first + width, count);
                const size_t last = std::min(middle + width, count);
                std::merge(source + first, source + middle, source + middle, source + last,
                           destination + first, keyLess);
            }
        });
        std::swap(source, destination);
    }

    if (source != m_items.data()) {
        std::copy(source, source + count, m_items.data());
    }
}

} // namespace engine
