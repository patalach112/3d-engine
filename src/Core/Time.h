#pragma once

#include <chrono>
#include <cstdint>

namespace engine {

// ---------------------------------------------------------------------------
// Time
//
// High-resolution frame clock built on std::chrono::steady_clock (monotonic,
// never adjusted backwards). Call tick() once per frame; it reports the
// delta since the previous tick, accumulates total elapsed time, counts
// frames, and maintains a smoothed frames-per-second estimate.
// ---------------------------------------------------------------------------
class Time {
public:
    using Clock = std::chrono::steady_clock;

    Time();

    // Restarts the clock: elapsed time, frame count and FPS are zeroed and
    // the first tick measures from this moment.
    void reset();

    // Advances the clock by one frame and returns the delta time in seconds.
    double tick();

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
    double m_deltaSeconds = 0.0;
    double m_elapsedSeconds = 0.0;
    uint64_t m_frameCount = 0;
    double m_framesPerSecond = 0.0;
};

} // namespace engine
