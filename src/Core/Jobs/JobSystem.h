#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// JobSystem
//
// Work-stealing thread pool used to parallelize data-parallel engine work
// (transform integration, model-matrix building, future physics).
//
// Each worker owns a job deque: it pops its own newest job (LIFO, cache
// friendly) and steals the oldest job (FIFO) from other workers when idle,
// which balances load without a central bottleneck. Jobs submitted from
// outside are distributed round-robin.
//
// Synchronization model:
//   * submit()       — fire-and-forget background work.
//   * waitForAll()   — barrier: blocks (main thread) until every queued and
//                      running job has finished.
//   * parallelFor()  — synchronous data-parallel loop; the calling thread
//                      also executes batches while waiting, so it is safe
//                      to call from worker threads too, and returning from
//                      it is a full barrier for the submitted range (this
//                      is what systems rely on before rendering).
//
// Job exceptions are caught, counted and logged — a throwing job can never
// kill a worker thread.
// ---------------------------------------------------------------------------
class JobSystem {
public:
    struct Config {
        // 0 = automatic: hardware threads minus one (the main thread),
        // at least 1. Values above hardware concurrency are allowed but
        // rarely useful.
        uint32_t workerThreads = 0;
    };

    JobSystem();
    explicit JobSystem(const Config& config);
    ~JobSystem();

    JobSystem(const JobSystem&) = delete;
    JobSystem& operator=(const JobSystem&) = delete;
    JobSystem(JobSystem&&) = delete;
    JobSystem& operator=(JobSystem&&) = delete;

    // Queues a job for background execution.
    void submit(std::function<void()> job);

    // Barrier for fire-and-forget work. Call from non-worker threads.
    void waitForAll();

    // Runs function(begin, end) over [0, itemCount) in parallel batches
    // and returns only when every batch has completed. batchSize 0 picks
    // a heuristic based on worker count.
    void parallelFor(size_t itemCount, size_t batchSize,
                     const std::function<void(size_t begin, size_t end)>& function);

    [[nodiscard]] uint32_t workerCount() const { return static_cast<uint32_t>(m_workers.size()); }
    [[nodiscard]] uint64_t completedJobCount() const { return m_completedJobs.load(std::memory_order_relaxed); }
    [[nodiscard]] uint64_t failedJobCount() const { return m_failedJobs.load(std::memory_order_relaxed); }
    [[nodiscard]] bool isShuttingDown() const { return m_stopping.load(std::memory_order_relaxed); }

private:
    struct WorkerQueue {
        std::deque<std::function<void()>> jobs;
        std::mutex mutex;
    };

    void workerLoop(uint32_t workerIndex);
    // Executes one queued job (own queue LIFO first when workerIndex is a
    // worker, otherwise steals). Returns false when no work was found.
    bool tryRunOneJob(uint32_t preferredQueue);
    // Runs a job with exception shielding. trackedJobs are those counted
    // in m_pendingJobs by submit(); inline fallback jobs pass false.
    void executeJob(std::function<void()> job, bool trackedJob);

    std::vector<std::unique_ptr<WorkerQueue>> m_queues;
    std::vector<std::thread> m_workers;

    std::atomic<uint32_t> m_nextQueue{0};
    std::atomic<uint64_t> m_pendingJobs{0}; // queued + running
    std::atomic<uint64_t> m_completedJobs{0};
    std::atomic<uint64_t> m_failedJobs{0};
    std::atomic<bool> m_stopping{false};

    std::condition_variable m_wakeCondition;
    std::mutex m_wakeMutex;
    std::condition_variable m_idleCondition;
    std::mutex m_idleMutex;
};

} // namespace engine
