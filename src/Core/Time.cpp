#include "Core/Time.h"

namespace engine {
namespace {
// Exponential smoothing factor for the FPS estimate: high enough to react
// quickly, low enough to keep the reported value readable.
constexpr double kFpsSmoothing = 0.1;
} // namespace

Time::Time() {
    reset();
}

void Time::reset() {
    m_startTime = Clock::now();
    m_lastTickTime = m_startTime;
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

    if (m_deltaSeconds > 0.0) {
        const double instantaneousFps = 1.0 / m_deltaSeconds;
        m_framesPerSecond = m_frameCount == 1
                                ? instantaneousFps
                                : m_framesPerSecond + (instantaneousFps - m_framesPerSecond) * kFpsSmoothing;
    }
    return m_deltaSeconds;
}

} // namespace engine
