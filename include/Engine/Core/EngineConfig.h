#pragma once
#include "Core/Export.h"

#include "Core/Camera.h"
#include "Core/GameLoop.h"
#include "Core/Logger.h"
#include "Core/PresentationMode.h"
#include "Physics/PhysicsComponents.h"
#include "Platform/Input.h"

#include <filesystem>
#include <string>
#include <vector>

namespace engine {

// Window creation settings (see config/engine.ini, [Window]).
struct WindowConfig {
    int width = 1280;
    int height = 720;
    std::string title = "3D Engine";
    bool resizable = true;
};

// Presentation settings (see config/engine.ini, [Render]).
struct RenderConfig {
    // In Auto mode: true -> FIFO (vsync), false -> IMMEDIATE/MAILBOX.
    bool vsync = true;
    // auto, fifo, mailbox, immediate, or fifo_relaxed. An explicit mode
    // (anything other than Auto) overrides the vsync switch above.
    PresentationMode presentMode = PresentationMode::Auto;
    // Skip drawing entities whose bounds are outside the view frustum.
    bool frustumCulling = true;
    // Additionally skip entities fully occluded (conservative
    // screen-space test) by static box/OBB colliders.
    bool occlusionCulling = false;
    // Sort draw items by material, then front-to-back, before submission.
    bool sortDrawItems = true;
    // On-screen FPS HUD (bitmap-font overlay): live FPS / frame time /
    // worker count / VRAM usage, refreshed at this interval (seconds).
    bool hudEnabled = true;
    double hudUpdateIntervalSeconds = 0.25;
    // Device-local VRAM planning budget for the GTX 970 profile: the
    // engine accounts allocations against this cap (below the card's
    // ~3.5 GiB fast segment) instead of over-committing VRAM.
    uint32_t vramBudgetMiB = 3072;
    // Budget for host-visible staging allocations used in uploads.
    uint32_t stagingBufferBudgetMiB = 256;
};

// Performance & memory tuning (see config/engine.ini, [Performance]).
struct PerformanceConfig {
    // JobSystem worker threads. 0 = automatic (hardware threads - 1).
    uint32_t jobWorkerCount = 0;
    // Component storage reserved up front in the scene's memory pool.
    uint32_t entityReserve = 1024;
    // Minimum visible meshes before render preparation parallelizes
    // (below this, sequential evaluation is cheaper than dispatching).
    uint32_t parallelMinDrawItems = 128;
    // Default parallelFor batch size (items). Tuned to the target's
    // per-core L2: ~1024 hot ECS items per batch. 0 = profile heuristic.
    uint32_t parallelBatchSize = 1024;
};

// ---------------------------------------------------------------------------
// EngineConfig
//
// Single source of truth for all engine tuning. Values come, in order of
// precedence, from:
//   1. an explicit path (command-line argument),
//   2. the ENGINE_CONFIG_PATH environment variable,
//   3. config/engine.ini relative to the working directory,
//   4. the config/engine.ini shipped with the source tree (baked in by
//      CMake as a development fallback),
//   5. the built-in defaults in the structs above.
// Parsing problems never abort startup: they are collected in loadWarnings
// (and reported through the logger once it exists), offending values keep
// their defaults.
// ---------------------------------------------------------------------------
struct ENGINE_CORE_API EngineConfig {
    LoggerConfig logger;
    LoopConfig loop;
    WindowConfig window;
    RenderConfig render;
    CameraConfig camera;
    InputConfig input;
    PerformanceConfig performance;
    PhysicsSettings physics;

    // File the values were actually loaded from; empty when defaults were used.
    std::string sourcePath;
    std::vector<std::string> loadWarnings;

    // Resolves and loads configuration as described above.
    static EngineConfig load(const std::string& explicitPath = {});

    // Loads one specific file (missing/unreadable files yield defaults plus
    // a warning, never an exception).
    static EngineConfig loadFromFile(const std::filesystem::path& path);
};

} // namespace engine
