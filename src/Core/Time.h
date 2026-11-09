#pragma once
#include "Core/Export.h"

#include <chrono>
#include <cstdint>

namespace engine {

// ---------------------------------------------------------------------------
// Time
//
// High-resolution frame clock built on std::chrono::steady_clock (monotonic,
// never adjusted backwards). Call tick() once per loop iteration for delta
// time; call recordPresentedFrame() only after a frame is actually
// submitted/presented. FPS is calculated from presented frames over a
// time window, never from the reciprocal of one (possibly near-zero)
// delta, so swapchain recreation/resizing cannot make it spike falsely.
// ---------------------------------------------------------------------------
class ENGINE_FOUNDATION_API Time {
public:
    using Clock = std::chrono::steady_clock;

    Time();

    // Restarts the clock: elapsed time, frame count and FPS are zeroed and
    // the first tick measures from this moment.
    void reset();

    // Advances the clock by one iteration and returns delta time.
    double tick();

    // Records one actually presented frame in the FPS window.
    void recordPresentedFrame();

    // Discards the current FPS window (used after a hitch/resize or a
    // skipped presentation, so stale samples cannot be reported as FPS).
    void resetFramesPerSecond();

    [[nodiscard]] double deltaSeconds() const { return m_deltaSeconds; }
    [[nodiscard]] double elapsedSeconds() const { return m_elapsedSeconds; }
    [[nodiscard]] uint64_t frameCount() const { return m_frameCount; }
    [[nodiscard]] double framesPerSecond() const { return m_framesPerSecond; }

    static Clock::time_point now() { return Clock::now(); }

    // Seconds between two clock readings, as a double.
    static double secondsBetween(Clock::time_point begin, Clock::time_point end) {
        return std::chrono::duration<double>(end - begin).count();
    }

private:
    Clock::time_point m_startTime;
    Clock::time_point m_lastTickTime;
    Clock::time_point m_fpsWindowStart;
    uint64_t m_fpsWindowFrames = 0;
    double m_deltaSeconds = 0.0;
    double m_elapsedSeconds = 0.0;
    uint64_t m_frameCount = 0;
    double m_framesPerSecond = 0.0;
};

} // namespace engine
