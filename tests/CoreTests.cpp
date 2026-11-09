// Unit tests for the Phase 2 core infrastructure: configuration loading,
// logger filtering/rotation, fixed-timestep math, Time and GameLoop
// dispatch. These cover the parts of the engine that can be verified
// without a GPU or a window.

#include "Core/EngineConfig.h"
#include "Core/GameLoop.h"
#include "Core/Logger.h"
#include "Core/Time.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace {

int g_failures = 0;

void check(bool condition, const std::string& name) {
    if (condition) {
        std::cout << "  [PASS] " << name << '\n';
    } else {
        std::cout << "  [FAIL] " << name << '\n';
        ++g_failures;
    }
}

bool nearlyEqual(double a, double b, double epsilon = 1e-9) {
    return std::abs(a - b) <= epsilon;
}

std::filesystem::path makeTempDir(const std::string& name) {
    const auto dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    return dir;
}

std::string readWholeFile(const std::filesystem::path& path) {
    std::ifstream file(path);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

void testConfigLoading() {
    std::cout << "EngineConfig\n";
    const auto dir = makeTempDir("engine_test_config");
    const auto path = dir / "test.ini";
    {
        std::ofstream file(path);
        file << "[Window]\nwidth = 800\nheight = 600\ntitle = Test Title\nresizable = false\n"
             << "[Render]\nvsync = false\n"
             << "[Logger]\nlevel = Warning\nconsole = false\n"
             << "[Loop]\nfixedDeltaSeconds = 0.01\nmaxFps = 144\nmaxFixedStepsPerFrame = 3\n"
             << "bogusKey = 1\n"
             << "[Loop]\nmaxFrameSeconds = not_a_number\n";
    }

    const engine::EngineConfig config = engine::EngineConfig::loadFromFile(path);
    check(config.window.width == 800 && config.window.height == 600, "window size parsed");
    check(config.window.title == "Test Title", "window title parsed");
    check(!config.window.resizable, "window resizable parsed");
    check(!config.render.vsync, "vsync parsed");
    check(config.logger.minimumLevel == engine::LogLevel::Warning, "log level parsed");
    check(!config.logger.consoleEnabled, "logger console flag parsed");
    check(nearlyEqual(config.loop.fixedDeltaSeconds, 0.01), "fixed delta parsed");
    check(config.loop.maxFps == 144, "max FPS parsed");
    check(config.loop.maxFixedStepsPerFrame == 3, "max fixed steps parsed");
    check(nearlyEqual(config.loop.maxFrameSeconds, 0.25), "invalid value keeps default");
    check(config.loadWarnings.size() >= 2, "unknown/invalid keys produce warnings");

    const engine::EngineConfig missing = engine::EngineConfig::loadFromFile(dir / "does_not_exist.ini");
    check(missing.window.width == 1280 && !missing.loadWarnings.empty(),
          "missing file yields defaults plus a warning");

    std::filesystem::remove_all(dir);
}

void testLogger() {
    std::cout << "Logger\n";
    const auto dir = makeTempDir("engine_test_logger");

    engine::LoggerConfig config;
    config.minimumLevel = engine::LogLevel::Warning;
    config.consoleEnabled = false;
    config.fileEnabled = true;
    config.filePath = dir / "test.log";
    config.timestampedFileName = false;
    config.asyncFileWrites = true;

    engine::Logger& logger = engine::Logger::instance();
    logger.initialize(config);
    check(logger.isEnabled(engine::LogLevel::Warning), "warning enabled at Warning level");
    check(!logger.isEnabled(engine::LogLevel::Info), "info filtered at Warning level");

    ENGINE_LOG_INFO("this info message must be filtered out");
    ENGINE_LOG_ERROR("this error message must be written");
    logger.shutdown();

    const std::string contents = readWholeFile(dir / "test.log");
    check(contents.find("this error message must be written") != std::string::npos,
          "error message reached the log file (async sink drained on shutdown)");
    check(contents.find("filtered out") == std::string::npos, "filtered message absent from log file");
    check(contents.find("[ERROR]") != std::string::npos, "level tag present in log line");
    check(contents.find("CoreTests.cpp:") != std::string::npos, "file:line present in log line");

    // Rotation: tiny size limit must produce at least one archive.
    engine::LoggerConfig rotating = config;
    rotating.minimumLevel = engine::LogLevel::Trace;
    rotating.filePath = dir / "rotating.log";
    rotating.maxFileSizeBytes = 200;
    rotating.maxRotatedFiles = 2;
    rotating.asyncFileWrites = false;
    logger.initialize(rotating);
    for (int i = 0; i < 40; ++i) {
        ENGINE_LOG_ERROR("rotation filler message number " << i << " with some extra text");
    }
    logger.shutdown();

    bool foundArchive = false;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().filename().string().starts_with("rotating.1")) {
            foundArchive = true;
        }
    }
    check(foundArchive, "size-based rotation produced an archive file");

    std::filesystem::remove_all(dir);
}

void testFixedTimestep() {
    std::cout << "FixedTimestep\n";
    engine::LoopConfig config;
    config.fixedDeltaSeconds = 0.01;
    config.maxFixedStepsPerFrame = 5;
    config.maxFrameSeconds = 0.25;

    engine::FixedTimestep timestep(config);

    const auto first = timestep.advance(0.025);
    check(first.steps == 2, "0.025s at 0.01s/step runs 2 steps");
    check(nearlyEqual(first.interpolationAlpha, 0.5, 1e-6), "leftover is half a step (alpha 0.5)");
    check(nearlyEqual(first.droppedSeconds, 0.0), "nothing dropped for a normal frame");

    const auto second = timestep.advance(0.004);
    check(second.steps == 0, "sub-step delta accumulates without stepping");
    check(nearlyEqual(second.interpolationAlpha, 0.9, 1e-6), "accumulator carries across frames");

    const auto huge = timestep.advance(10.0);
    check(huge.steps == 5, "step cap limits catch-up work per frame");
    check(huge.droppedSeconds > 0.0, "excess time is dropped (spiral-of-death protection)");
    check(nearlyEqual(huge.interpolationAlpha, 0.0), "backlog reset leaves alpha 0");

    const auto negative = timestep.advance(-1.0);
    check(negative.steps == 0, "negative deltas never produce steps");
}

void testTime() {
    std::cout << "Time\n";
    engine::Time time;
    time.reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    const double delta = time.tick();
    check(delta > 0.0, "tick reports a positive delta after sleeping");
    check(time.elapsedSeconds() >= delta, "elapsed time accumulates at least one delta");
    check(time.frameCount() == 1, "frame count increments per tick");
}

void testGameLoop() {
    std::cout << "GameLoop\n";
    engine::LoopConfig config;
    config.fixedDeltaSeconds = 0.001; // tiny steps: real frames will need several
    config.maxFixedStepsPerFrame = 100;
    config.maxFrameSeconds = 0.25;
    config.maxFps = 0;

    engine::GameLoop loop(config);

    int polls = 0;
    int updates = 0;
    int renders = 0;
    int fixedSteps = 0;
    int continueChecks = 0;

    engine::LoopCallbacks callbacks;
    callbacks.shouldContinue = [&continueChecks] { return ++continueChecks <= 3; };
    callbacks.pollEvents = [&polls] { ++polls; };
    callbacks.fixedUpdate = [&fixedSteps](double) { ++fixedSteps; };
    callbacks.update = [&updates](double) { ++updates; };
    callbacks.render = [&renders](double) { ++renders; };

    loop.run(callbacks);

    check(loop.stats().frames == 3, "loop runs exactly the requested frames");
    check(polls == 3 && updates == 3 && renders == 3, "poll/update/render fire once per frame");
    check(loop.stats().fixedSteps == static_cast<uint64_t>(fixedSteps),
          "fixed-step statistics match executed fixed updates");
}

} // namespace

int main() {
    testConfigLoading();
    testLogger();
    testFixedTimestep();
    testTime();
    testGameLoop();

    if (g_failures == 0) {
        std::cout << "All core tests passed.\n";
        return 0;
    }
    std::cout << g_failures << " core test(s) FAILED.\n";
    return 1;
}
