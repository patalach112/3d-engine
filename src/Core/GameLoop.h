#pragma once

#include "Core/Time.h"

#include <atomic>
#include <cstdint>
#include <functional>

namespace engine {

// ---------------------------------------------------------------------------
// LoopConfig — all game-loop tuning, supplied by the engine configuration
// (config/engine.ini) instead of constants buried in the loop.
// ---------------------------------------------------------------------------
struct LoopConfig {
    // Duration of one fixed update step, in seconds (e.g. 1/60). Fixed
    // updates are where deterministic simulation (physics, gameplay) will
    // live in later phases.
    double fixedDeltaSeconds = 1.0 / 60.0;
    // Upper bound on fixed steps per rendered frame. If the simulation
    // still falls behind after this many steps, the remaining accumulated
    // time is dropped ("spiral of death" protection) instead of making
    // every subsequent frame longer.
    uint32_t maxFixedStepsPerFrame = 5;
    // A single frame's delta is clamped to this many seconds, so hitches
    // (debugger breaks, window drags) don't teleport the simulation.
    double maxFrameSeconds = 0.25;
    // Optional software frame-rate cap in frames per second.
    // 0 means uncapped (presentation/vsync may still pace the loop).
    uint32_t maxFps = 0;
    // Master switch for the fixed-timestep path.
    bool fixedTimestepEnabled = true;
    // When > 0, the application logs loop statistics at this interval
    // (seconds). 0 disables periodic statistics.
    double statsLogIntervalSeconds = 0.0;
};

// ---------------------------------------------------------------------------
// FixedTimestep
//
// Pure accumulator logic that converts variable frame deltas into an
// integral number of fixed steps. It is deliberately free of callbacks and
// clocks so it can be unit-tested deterministically.
// ---------------------------------------------------------------------------
class FixedTimestep {
public:
    struct Result {
        uint32_t steps = 0;          // fixed steps the caller should execute now
        double interpolationAlpha = 0.0; // leftover fraction of a step, in [0, 1)
        double droppedSeconds = 0.0;     // time discarded by clamping/step cap
    };

    explicit FixedTimestep(const LoopConfig& config);

    // Feeds one frame's delta time and reports how many fixed steps to run.
    Result advance(double frameDeltaSeconds);
    void reset();

    [[nodiscard]] double fixedDeltaSeconds() const { return m_fixedDeltaSeconds; }
    [[nodiscard]] double accumulatorSeconds() const { return m_accumulatorSeconds; }

private:
    double m_fixedDeltaSeconds;
    uint32_t m_maxStepsPerFrame;
    double m_maxFrameSeconds;
    double m_accumulatorSeconds = 0.0;
};

// ---------------------------------------------------------------------------
// LoopCallbacks — everything the loop needs from the application, kept as
// plain callables so the loop has no dependency on Window/Renderer/etc.
// Any callback may be left empty except shouldContinue.
// ---------------------------------------------------------------------------
struct LoopCallbacks {
    // Return false to end the loop (checked once per frame). Required.
    std::function<bool()> shouldContinue;
    // Platform event pumping, invoked at the top of every frame.
    std::function<void()> pollEvents;
    // Deterministic simulation step; called 0..N times per frame with the
    // configured fixed delta.
    std::function<void(double fixedDeltaSeconds)> fixedUpdate;
    // Variable-rate update; called once per frame with the (clamped) frame
    // delta. Good for cameras, animation and other non-deterministic logic.
    std::function<void(double deltaSeconds)> update;
    // Rendering; called once per frame. interpolationAlpha in [0, 1) says
    // how far the render state sits between the last two fixed states.
    std::function<void(double interpolationAlpha)> render;
};

struct LoopStats {
    uint64_t frames = 0;
    uint64_t fixedSteps = 0;
    double elapsedSeconds = 0.0;
    double droppedSeconds = 0.0; // simulation time discarded to stay real-time

    [[nodiscard]] double averageFramesPerSecond() const {
        return elapsedSeconds > 0.0 ? static_cast<double>(frames) / elapsedSeconds : 0.0;
    }
};

// ---------------------------------------------------------------------------
// GameLoop
//
// Owns the engine's main loop orchestration: timing, fixed/variable update
// dispatch, rendering, optional FPS capping and statistics. It knows
// nothing about windows or graphics APIs — the Application wires those in
// through LoopCallbacks.
// ---------------------------------------------------------------------------
class GameLoop {
public:
    explicit GameLoop(LoopConfig config);

    // Runs until shouldContinue() returns false or requestStop() is called.
    void run(const LoopCallbacks& callbacks);

    // Thread-safe stop request, honored at the next frame boundary.
    void requestStop() { m_stopRequested.store(true, std::memory_order_relaxed); }

    [[nodiscard]] const LoopConfig& config() const { return m_config; }
    [[nodiscard]] const LoopStats& stats() const { return m_stats; }
    [[nodiscard]] const Time& time() const { return m_time; }

private:
    void applyFrameRateCap(Time::Clock::time_point frameStart) const;

    LoopConfig m_config;
    FixedTimestep m_fixedTimestep;
    Time m_time;
    LoopStats m_stats;
    std::atomic<bool> m_stopRequested{false};
};

} // namespace engine
