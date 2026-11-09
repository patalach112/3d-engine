#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace engine::hardware {

// ---------------------------------------------------------------------------
// Target hardware profile: Intel Core i5-3470 + NVIDIA GTX 970.
//
// The engine's default tuning (worker counts, batch granularity, pool
// geometry, GPU budgets) is derived from these constants so the whole
// codebase tunes from one place. Explicit config values always override
// the profile; the profile only shapes the *automatic* choices.
//
// i5-3470 (Ivy Bridge, 2012):
//   * 4 physical cores, no Hyper-Threading (4 logical threads)
//   * 32 KiB L1d + 256 KiB L2 per core, 6 MiB shared L3
//   * 64-byte cache lines (AVX, no AVX2), DDR3-1600 dual channel
//     (~25.6 GB/s theoretical — bandwidth is precious, stream don't chase)
//
// GTX 970 (Maxwell, 2014):
//   * 4 GiB VRAM, of which the top 0.5 GiB sits behind a narrower bus
//     segment — the practical fast budget is ~3.5 GiB, and the engine
//     plans against 3 GiB to leave the driver/compositor headroom.
// ---------------------------------------------------------------------------

inline constexpr uint32_t targetPhysicalCores = 4;
inline constexpr uint32_t targetLogicalThreads = 4;
inline constexpr size_t cacheLineBytes = 64;
inline constexpr size_t l1DataCacheBytes = 32 * 1024;
inline constexpr size_t l2CacheBytes = 256 * 1024;
inline constexpr size_t l3CacheBytes = 6 * 1024 * 1024;

// Workers + the calling (main) thread together should occupy the
// physical cores exactly: 3 workers + main = 4 threads on the target.
// Oversubscription on a 4-core part buys only context-switch thrash.
inline constexpr uint32_t recommendedWorkerCount = targetPhysicalCores - 1;

// Default parallelFor batch granularity. Hot ECS working sets are
// ~64 B (transform) + ~16-64 B (component/output) per item, so ~1024
// items ≈ 128-256 KiB — one batch roughly fills a core's L2 without
// spilling, while still giving the pool several batches per worker to
// balance load.
inline constexpr size_t parallelBatchItems = 1024;

// Paged pool geometry: 64 KiB pages (16 OS pages, carved once, always
// 64-byte aligned) with power-of-two block classes up to one 4 KiB OS
// page; larger requests fall back to aligned allocation.
inline constexpr size_t poolPageBytes = 64 * 1024;
inline constexpr size_t poolMaxBlockBytes = 4096;

// GTX 970 VRAM planning numbers (see header note).
inline constexpr uint64_t gpuVramTotalBytes = 4ull * 1024 * 1024 * 1024;
inline constexpr uint64_t gpuVramFastBytes = gpuVramTotalBytes - gpuVramTotalBytes / 8; // ~3.5 GiB
inline constexpr uint64_t recommendedVramBudgetBytes = 3ull * 1024 * 1024 * 1024;
inline constexpr uint64_t recommendedStagingBudgetBytes = 256ull * 1024 * 1024;

// Resolves a JobSystem worker count. An explicit request always wins;
// otherwise workers + caller fill — never exceed — the target's physical
// cores, and never exceed what the host reports.
[[nodiscard]] inline uint32_t resolveWorkerCount(uint32_t requested, uint32_t hardwareConcurrency) {
    if (requested > 0) {
        return requested;
    }
    if (hardwareConcurrency == 0) {
        return recommendedWorkerCount; // host could not tell us: trust the profile
    }
    if (hardwareConcurrency == 1) {
        return 1;
    }
    return std::min(hardwareConcurrency - 1, recommendedWorkerCount);
}

// Resolves a parallelFor batch size. An explicit size always wins;
// otherwise aim for ~4 batches per worker, capped at the L2-friendly
// profile batch so huge ranges don't create cache-spilling mega-batches.
[[nodiscard]] inline size_t resolveParallelBatchSize(size_t itemCount, size_t workerCount,
                                                     size_t configuredBatchSize) {
    if (itemCount == 0) {
        return 1;
    }
    if (configuredBatchSize > 0) {
        return configuredBatchSize;
    }
    const size_t targetBatches = std::max(workerCount * 4, size_t{1});
    const size_t heuristic = (itemCount + targetBatches - 1) / targetBatches;
    return std::max(std::min(heuristic, parallelBatchItems), size_t{1});
}

} // namespace engine::hardware
