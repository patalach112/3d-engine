#include "Core/Jobs/JobSystem.h"

#include "Core/HardwareProfile.h"
#include "Core/Logger.h"

#include <algorithm>

namespace engine {
namespace {

// Bounded per-worker ring capacity. If every ring fills up, submit()
// runs the job inline on the caller's thread instead of blocking or
// dropping work, so the bound costs latency predictability, not jobs.
constexpr size_t kWorkerQueueCapacity = 1024;

uint32_t resolveWorkerCount(uint32_t requested) {
    // Profile-driven: on the 4-core target this yields 3 workers, so
    // workers + main thread occupy the physical cores exactly.
    return hardware::resolveWorkerCount(requested, std::thread::hardware_concurrency());
}

} // namespace

JobSystem::JobSystem() : JobSystem(Config{}) {}

JobSystem::JobSystem(const Config& config) : m_defaultBatchSize(config.defaultBatchSize) {
    const uint32_t workerCount = resolveWorkerCount(config.workerThreads);
    m_queues.reserve(workerCount);
    for (uint32_t i = 0; i < workerCount; ++i) {
        m_queues.push_back(std::make_unique<WorkerQueue>(kWorkerQueueCapacity));
    }
    m_workers.reserve(workerCount);
    for (uint32_t i = 0; i < workerCount; ++i) {
        m_workers.emplace_back([this, i] { workerLoop(i); });
    }
    ENGINE_LOG_INFO("JobSystem: started " << workerCount << " worker thread(s) (hardware concurrency "
                                          << std::thread::hardware_concurrency() << ")");
}

JobSystem::~JobSystem() {
    // Workers drain remaining jobs before exiting (their loop only stops
    // once nothing is pending), so shutdown never drops queued work.
    m_stopping.store(true, std::memory_order_release);
    m_wakeCondition.notify_all();
    for (std::thread& worker : m_workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    ENGINE_LOG_DEBUG("JobSystem: shut down after " << m_completedJobs.load(std::memory_order_relaxed)
                                                   << " completed job(s), "
                                                   << m_failedJobs.load(std::memory_order_relaxed) << " failed");
}

void JobSystem::submit(std::function<void()> job) {
    if (m_stopping.load(std::memory_order_acquire) || m_queues.empty()) {
        // Submitting during shutdown (or with no workers) must not lose
        // work: run it inline on the caller's thread.
        executeJob(std::move(job), false);
        return;
    }

    m_pendingJobs.fetch_add(1, std::memory_order_acq_rel);
    const uint32_t start = m_nextQueue.fetch_add(1, std::memory_order_relaxed) %
                           static_cast<uint32_t>(m_queues.size());
    // Lock-free push, round-robin first, then any ring with room. A
    // failed tryPush leaves `job` intact, so falling through is safe.
    for (size_t offset = 0; offset < m_queues.size(); ++offset) {
        WorkerQueue& queue = *m_queues[(start + offset) % m_queues.size()];
        if (queue.jobs.tryPush(std::move(job))) {
            m_wakeCondition.notify_one();
            return;
        }
    }
    // Every ring is full: run inline (still tracked, so waitForAll and
    // the job counters stay exact) rather than blocking a producer.
    executeJob(std::move(job), true);
}

void JobSystem::waitForAll() {
    std::unique_lock lock(m_idleMutex);
    m_idleCondition.wait(lock, [this] { return m_pendingJobs.load(std::memory_order_relaxed) == 0; });
}

void JobSystem::parallelFor(size_t itemCount, size_t batchSize,
                            const std::function<void(size_t begin, size_t end)>& function) {
    if (itemCount == 0) {
        return;
    }
    if (batchSize == 0) {
        batchSize = hardware::resolveParallelBatchSize(itemCount, m_workers.size(), m_defaultBatchSize);
    }

    struct SharedState {
        std::atomic<size_t> remaining{0};
        std::mutex mutex;
        std::condition_variable condition;
    };
    auto state = std::make_shared<SharedState>();

    const size_t batchCount = (itemCount + batchSize - 1) / batchSize;
    state->remaining.store(batchCount, std::memory_order_relaxed);

    for (size_t batch = 0; batch < batchCount; ++batch) {
        const size_t begin = batch * batchSize;
        const size_t end = std::min(begin + batchSize, itemCount);
        submit([state, &function, begin, end] {
            try {
                function(begin, end);
            } catch (...) {
                // executeJob (below) already counts/logs job failures; the
                // batch bookkeeping must still complete or callers hang.
                if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                    std::lock_guard lock(state->mutex);
                    state->condition.notify_all();
                }
                throw;
            }
            if (state->remaining.fetch_sub(1, std::memory_order_acq_rel) == 1) {
                std::lock_guard lock(state->mutex);
                state->condition.notify_all();
            }
        });
    }

    // Help execute batches while waiting: this keeps parallelFor safe to
    // call from worker threads (no pool-exhaustion deadlock) and makes
    // the calling thread do useful work instead of sleeping.
    const uint32_t helpQueue = m_queues.empty() ? 0
                               : m_nextQueue.load(std::memory_order_relaxed) %
                                     static_cast<uint32_t>(m_queues.size());
    while (state->remaining.load(std::memory_order_acquire) > 0) {
        if (!tryRunOneJob(helpQueue)) {
            std::unique_lock lock(state->mutex);
            state->condition.wait_for(lock, std::chrono::milliseconds(1), [&state] {
                return state->remaining.load(std::memory_order_acquire) == 0;
            });
        }
    }
}

void JobSystem::workerLoop(uint32_t workerIndex) {
    while (true) {
        if (tryRunOneJob(workerIndex)) {
            continue;
        }
        if (m_stopping.load(std::memory_order_acquire) &&
            m_pendingJobs.load(std::memory_order_acquire) == 0) {
            return;
        }
        // Short timed wait: workers re-scan for stealable work regularly,
        // so a global broadcast on every submit is unnecessary.
        std::unique_lock lock(m_wakeMutex);
        m_wakeCondition.wait_for(lock, std::chrono::milliseconds(1));
    }
}

bool JobSystem::tryRunOneJob(uint32_t preferredQueue) {
    const size_t queueCount = m_queues.size();
    if (queueCount == 0) {
        return false;
    }

    for (size_t offset = 0; offset < queueCount; ++offset) {
        const size_t index = (preferredQueue + offset) % queueCount;
        WorkerQueue& queue = *m_queues[index];
        // Lock-free pop: offset 0 drains the worker's own ring; any other
        // ring is a steal. Ring order is FIFO, so steals take the oldest
        // (usually largest) outstanding job first.
        std::function<void()> job;
        if (queue.jobs.tryPop(job)) {
            executeJob(std::move(job), true);
            return true;
        }
    }
    return false;
}

void JobSystem::executeJob(std::function<void()> job, bool trackedJob) {
    try {
        job();
        m_completedJobs.fetch_add(1, std::memory_order_relaxed);
    } catch (const std::exception& error) {
        m_failedJobs.fetch_add(1, std::memory_order_relaxed);
        ENGINE_LOG_ERROR("JobSystem: job threw an exception: " << error.what());
    } catch (...) {
        m_failedJobs.fetch_add(1, std::memory_order_relaxed);
        ENGINE_LOG_ERROR("JobSystem: job threw an unknown exception");
    }

    if (trackedJob && m_pendingJobs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard lock(m_idleMutex);
        m_idleCondition.notify_all();
    }
}

} // namespace engine
