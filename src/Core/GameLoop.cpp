#include "Core/GameLoop.h"

#include <algorithm>
#include <thread>

namespace engine {

// ---------------------------------------------------------------------------
// FixedTimestep
// ---------------------------------------------------------------------------
FixedTimestep::FixedTimestep(const LoopConfig& config)
    : m_fixedDeltaSeconds(config.fixedDeltaSeconds),
      m_maxStepsPerFrame(config.maxFixedStepsPerFrame),
      m_maxFrameSeconds(config.maxFrameSeconds) {}

FixedTimestep::Result FixedTimestep::advance(double frameDeltaSeconds) {
    Result result;

    const double clampedDelta = std::clamp(frameDeltaSeconds, 0.0, m_maxFrameSeconds);
    result.droppedSeconds += frameDeltaSeconds - clampedDelta;
    m_accumulatorSeconds += clampedDelta;

    uint32_t steps = 0;
    while (m_accumulatorSeconds >= m_fixedDeltaSeconds && steps < m_maxStepsPerFrame) {
        m_accumulatorSeconds -= m_fixedDeltaSeconds;
        ++steps;
    }

    // The step cap was hit and we are still behind: drop the backlog so a
    // slow period doesn't compound into an ever-growing catch-up debt.
    if (steps == m_maxStepsPerFrame && m_accumulatorSeconds >= m_fixedDeltaSeconds) {
        result.droppedSeconds += m_accumulatorSeconds;
        m_accumulatorSeconds = 0.0;
    }

    result.steps = steps;
    result.interpolationAlpha =
        m_fixedDeltaSeconds > 0.0 ? std::clamp(m_accumulatorSeconds / m_fixedDeltaSeconds, 0.0, 1.0) : 0.0;
    return result;
}

void FixedTimestep::reset() {
    m_accumulatorSeconds = 0.0;
}

// ---------------------------------------------------------------------------
// GameLoop
// ---------------------------------------------------------------------------
GameLoop::GameLoop(LoopConfig config) : m_config(config), m_fixedTimestep(config) {}

void GameLoop::run(const LoopCallbacks& callbacks) {
    m_stopRequested.store(false, std::memory_order_relaxed);
    m_stats = LoopStats{};
    m_time.reset();
    m_fixedTimestep.reset();

    while (!m_stopRequested.load(std::memory_order_relaxed)) {
        if (callbacks.shouldContinue && !callbacks.shouldContinue()) {
            break;
        }
        const Time::Clock::time_point frameStart = Time::now();

        if (callbacks.pollEvents) {
            callbacks.pollEvents();
        }

        const double rawDelta = m_time.tick();
        const double frameDelta = std::min(rawDelta, m_config.maxFrameSeconds);
        m_stats.droppedSeconds += rawDelta - frameDelta;
        if (rawDelta > m_config.maxFrameSeconds) {
            // A window drag/resize or debugger hitch is a break in the
            // presented-frame stream, not an extremely fast or slow run.
            // Start a fresh FPS window after it.
            m_time.resetFramesPerSecond();
        }

        double interpolationAlpha = 0.0;
        if (m_config.fixedTimestepEnabled) {
            const FixedTimestep::Result fixed = m_fixedTimestep.advance(rawDelta);
            m_stats.droppedSeconds += fixed.droppedSeconds;
            interpolationAlpha = fixed.interpolationAlpha;
            if (callbacks.fixedUpdate) {
                for (uint32_t step = 0; step < fixed.steps; ++step) {
                    callbacks.fixedUpdate(m_config.fixedDeltaSeconds);
                }
            }
            m_stats.fixedSteps += fixed.steps;
        }

        if (callbacks.update) {
            callbacks.update(frameDelta);
        }
        bool framePresented = true;
        if (callbacks.render) {
            framePresented = callbacks.render(interpolationAlpha);
        }

        if (framePresented) {
            ++m_stats.frames;
            m_time.recordPresentedFrame();
        } else {
            // A resize/swapchain rebuild may make the loop spin through
            // iterations that display nothing. They are not frames and
            // must not feed either the statistics or the smoothed FPS.
            m_time.resetFramesPerSecond();
        }
        m_stats.elapsedSeconds = m_time.elapsedSeconds();

        applyFrameRateCap(frameStart);
    }
}

void GameLoop::applyFrameRateCap(Time::Clock::time_point frameStart) const {
    if (m_config.maxFps == 0) {
        return;
    }
    const auto frameBudget = std::chrono::duration<double>(1.0 / static_cast<double>(m_config.maxFps));
    const Time::Clock::time_point deadline = frameStart + std::chrono::duration_cast<Time::Clock::duration>(frameBudget);
    // Sleep until the budget expires. Presentation (vsync) may already have
    // consumed it, in which case this is a no-op.
    if (Time::now() < deadline) {
        std::this_thread::sleep_until(deadline);
    }
}

} // namespace engine
