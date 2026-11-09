#pragma once

#include "Core/Camera.h"
#include "Core/GameLoop.h"
#include "Core/Logger.h"
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
    // true  -> present with FIFO (vsync: tear-free, paced to refresh rate)
    // false -> prefer IMMEDIATE, then MAILBOX (lowest latency, may tear)
    bool vsync = true;
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
struct EngineConfig {
    LoggerConfig logger;
    LoopConfig loop;
    WindowConfig window;
    RenderConfig render;
    CameraConfig camera;
    InputConfig input;

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
