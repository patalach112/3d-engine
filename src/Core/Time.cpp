#include "Core/Time.h"

namespace engine {
namespace {
// FPS is reported over a quarter-second window of presented frames. It
// reacts quickly enough for the HUD while making it impossible for one
// near-zero loop delta (for example, repeated swapchain recreation while
// resizing) to produce a mathematically bogus 1000+ FPS reading.
constexpr double kFpsWindowSeconds = 0.25;
} // namespace

Time::Time() {
    reset();
}

void Time::reset() {
    m_startTime = Clock::now();
    m_lastTickTime = m_startTime;
    m_fpsWindowStart = m_startTime;
    m_fpsWindowFrames = 0;
    m_deltaSeconds = 0.0;
    m_elapsedSeconds = 0.0;
    m_frameCount = 0;
    m_framesPerSecond = 0.0;
}

double Time::tick() {
    const Clock::time_point current = Clock::now();
    m_deltaSeconds = secondsBetween(m_lastTickTime, current);
    m_lastTickTime = current;
    m_elapsedSeconds = secondsBetween(m_startTime, current);
    ++m_frameCount;
    return m_deltaSeconds;
}

void Time::recordPresentedFrame() {
    if (m_fpsWindowFrames == 0) {
        m_fpsWindowStart = m_lastTickTime;
    }
    ++m_fpsWindowFrames;

    const double windowSeconds = secondsBetween(m_fpsWindowStart, m_lastTickTime);
    if (windowSeconds >= kFpsWindowSeconds) {
        m_framesPerSecond = static_cast<double>(m_fpsWindowFrames) / windowSeconds;
        m_fpsWindowStart = m_lastTickTime;
        m_fpsWindowFrames = 0;
    }
}

void Time::resetFramesPerSecond() {
    m_framesPerSecond = 0.0;
    m_fpsWindowStart = m_lastTickTime;
    m_fpsWindowFrames = 0;
}

} // namespace engine
