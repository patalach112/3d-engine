// Unit tests for the Phase 2 core infrastructure: configuration loading,
// logger filtering/rotation, fixed-timestep math, Time and GameLoop
// dispatch. These cover the parts of the engine that can be verified
// without a GPU or a window.

#include "Core/Camera.h"
#include "Core/EngineConfig.h"
#include "Platform/Window.h"
#include "Core/GameLoop.h"
#include "Core/Logger.h"
#include "Core/Time.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec4.hpp>

#include "Scene/DenseIteration.h"
#include "Scene/Scene.h"
#include "Scene/Systems/RenderSystem.h"
#include "Scene/Systems/RotationSystem.h"

#include "Core/Concurrency/LockFreeRingBuffer.h"
#include "Assets/AssetManager.h"
#include "Core/HardwareProfile.h"
#include "Geometry/Cube.h"
#include "Graphics/PipelineState.h"
#include "Core/Jobs/JobSystem.h"
#include "Graphics/VramBudget.h"
#include "Graphics/HudFont.h"
#include "Graphics/RenderQueue.h"
#include "Core/Memory/PoolAllocator.h"
#include "Graphics/Frustum.h"

#include <atomic>

#include <chrono>
#include <cmath>
#include <cstring>
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
             << "[Render]\nvsync = false\nsortDrawItems = false\n"
             << "[Logger]\nlevel = Warning\nconsole = false\n"
             << "[Loop]\nfixedDeltaSeconds = 0.01\nmaxFps = 144\nmaxFixedStepsPerFrame = 3\n"
             << "bogusKey = 1\n"
             << "[Loop]\nmaxFrameSeconds = not_a_number\n"
             << "[Camera]\nmovementSpeed = 7.5\nmouseSensitivity = 0.25\npositionZ = 5\n"
             << "lookSmoothingSeconds = 0.04\nmovementSmoothingSeconds = 0.09\n"
             << "[Input]\ncaptureMouseOnStart = false\n"
             << "[Render]\nfrustumCulling = false\n"
             << "[Performance]\njobWorkerCount = 2\nentityReserve = 4096\nparallelBatchSize = 512\n"
             << "[Render]\nvramBudgetMiB = 2048\nstagingBufferBudgetMiB = 128\nhudEnabled = false\n";
    }

    const engine::EngineConfig config = engine::EngineConfig::loadFromFile(path);
    check(config.window.width == 800 && config.window.height == 600, "window size parsed");
    check(config.window.title == "Test Title", "window title parsed");
    check(!config.window.resizable, "window resizable parsed");
    check(!config.render.vsync, "vsync parsed");
    check(!config.render.sortDrawItems, "render queue sorting flag parsed");
    check(config.logger.minimumLevel == engine::LogLevel::Warning, "log level parsed");
    check(!config.logger.consoleEnabled, "logger console flag parsed");
    check(nearlyEqual(config.loop.fixedDeltaSeconds, 0.01), "fixed delta parsed");
    check(config.loop.maxFps == 144, "max FPS parsed");
    check(config.loop.maxFixedStepsPerFrame == 3, "max fixed steps parsed");
    check(nearlyEqual(config.loop.maxFrameSeconds, 0.25), "invalid value keeps default");
    check(nearlyEqual(config.camera.movementSpeed, 7.5), "camera movement speed parsed");
    check(nearlyEqual(config.camera.mouseSensitivity, 0.25), "camera mouse sensitivity parsed");
    check(nearlyEqual(config.camera.startPosition.z, 5.0), "camera start position parsed");
    check(nearlyEqual(config.camera.lookSmoothingSeconds, 0.04f, 1e-6), "camera look smoothing parsed");
    check(nearlyEqual(config.camera.movementSmoothingSeconds, 0.09f, 1e-6), "camera movement smoothing parsed");
    check(!config.input.captureMouseOnStart, "input capture flag parsed");
    check(!config.render.frustumCulling, "frustum culling flag parsed");
    check(config.performance.jobWorkerCount == 2, "job worker count parsed");
    check(config.performance.parallelBatchSize == 512, "parallel batch size parsed");
    check(config.render.vramBudgetMiB == 2048, "VRAM budget parsed");
    check(config.render.stagingBufferBudgetMiB == 128, "staging budget parsed");
    check(!config.render.hudEnabled, "HUD enabled flag parsed");
    check(config.performance.entityReserve == 4096, "entity reserve parsed");
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

bool nearlyVec3(const glm::vec3& value, const glm::vec3& expected, float epsilon = 1e-5f) {
    return std::abs(value.x - expected.x) <= epsilon && std::abs(value.y - expected.y) <= epsilon &&
           std::abs(value.z - expected.z) <= epsilon;
}

void testCamera() {
    std::cout << "Camera\n";
    engine::CameraConfig config;
    config.lookSmoothingSeconds = 0.0f;
    config.movementSmoothingSeconds = 0.0f;
    engine::Camera camera(config);

    check(nearlyVec3(camera.position(), glm::vec3(0.0f, 0.0f, 3.0f)), "camera starts at configured position");
    check(nearlyVec3(camera.front(), glm::vec3(0.0f, 0.0f, -1.0f)), "yaw -90 faces down -Z");

    // The origin (cube location) must sit 3 units in front of the camera.
    const glm::vec4 originInView = camera.viewMatrix() * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    check(nearlyVec3(glm::vec3(originInView), glm::vec3(0.0f, 0.0f, -3.0f)),
          "view matrix places the origin ahead of the camera");

    // Moving the mouse right turns the camera right (front gains +X).
    camera.processLook({100.0f, 0.0f});
    check(camera.yawDegrees() > -90.0f && camera.front().x > 0.0f, "mouse right turns camera right");

    // Pitch clamps at the configured limit instead of flipping (no gimbal lock).
    camera.processLook({0.0f, -100000.0f}); // mouse far up
    check(nearlyEqual(camera.pitchDegrees(), config.pitchLimitDegrees, 1e-4), "pitch clamps at +limit");
    check(camera.front().y > 0.99f && camera.front().y < 1.0f, "pitched-up front stays below vertical");
    camera.processLook({0.0f, 200000.0f}); // mouse far down
    check(nearlyEqual(camera.pitchDegrees(), -static_cast<double>(config.pitchLimitDegrees), 1e-4),
          "pitch clamps at -limit");

    // Forward translation: 1 second at 3 units/s along -Z from (0,0,3).
    camera.reset();
    camera.translate(glm::vec3(0.0f, 0.0f, 1.0f), 1.0);
    check(nearlyVec3(camera.position(), glm::vec3(0.0f, 0.0f, 0.0f)), "forward translation reaches the origin");

    // Diagonal input is normalized: same distance as straight movement.
    camera.reset();
    camera.translate(glm::vec3(1.0f, 0.0f, 1.0f), 1.0);
    const float diagonalDistance = glm::length(camera.position() - glm::vec3(0.0f, 0.0f, 3.0f));
    check(nearlyEqual(diagonalDistance, config.movementSpeed, 1e-4), "diagonal movement is normalized");

    // Vulkan projection: Y axis flipped relative to GLM's OpenGL default.
    const glm::mat4 projection = camera.projectionMatrix(16.0f / 9.0f);
    check(projection[0][0] > 0.0f && projection[1][1] < 0.0f, "projection has Vulkan Y flip applied");

    // Reset restores the configured start state.
    camera.processLook({250.0f, -250.0f});
    camera.reset();
    check(nearlyVec3(camera.position(), config.startPosition) &&
              nearlyEqual(camera.yawDegrees(), config.startYawDegrees, 1e-4),
          "reset restores start position and orientation");

    // Splitting one second of translation into different frame counts
    // must travel the same distance: speed is units/second, not units/frame.
    camera.reset();
    for (int frame = 0; frame < 30; ++frame) {
        camera.translate(glm::vec3(0.0f, 0.0f, 1.0f), 1.0 / 30.0);
    }
    const glm::vec3 positionAt30Fps = camera.position();
    camera.reset();
    for (int frame = 0; frame < 120; ++frame) {
        camera.translate(glm::vec3(0.0f, 0.0f, 1.0f), 1.0 / 120.0);
    }
    check(nearlyVec3(camera.position(), positionAt30Fps, 1e-4f),
          "translation distance is independent of frame rate");

    // Mouse look consumes total pixels travelled, not pixels * frame
    // time. Deliver the same 600 px over one simulated second at two
    // frame rates, then settle: both cameras must reach the same yaw.
    engine::CameraConfig lookConfig;
    lookConfig.lookSmoothingSeconds = 0.05f;
    lookConfig.movementSmoothingSeconds = 0.0f;
    engine::Camera lookAt30Fps(lookConfig);
    engine::Camera lookAt120Fps(lookConfig);
    for (int frame = 0; frame < 30; ++frame) {
        lookAt30Fps.processLook({20.0f, 0.0f});
        lookAt30Fps.update(1.0 / 30.0);
    }
    for (int frame = 0; frame < 120; ++frame) {
        lookAt120Fps.processLook({5.0f, 0.0f});
        lookAt120Fps.update(1.0 / 120.0);
    }
    check(nearlyEqual(lookAt30Fps.targetYawDegrees(), lookAt120Fps.targetYawDegrees(), 1e-4),
          "mouse target accumulates pixels independent of frame rate");
    lookAt30Fps.update(10.0);
    lookAt120Fps.update(10.0);
    check(nearlyEqual(lookAt30Fps.yawDegrees(), lookAt120Fps.yawDegrees(), 1e-3) &&
              nearlyEqual(lookAt30Fps.yawDegrees(), lookAt30Fps.targetYawDegrees(), 1e-3),
          "smoothed look converges to the same orientation at any frame rate");

    // One smoothed frame should move towards the target without applying
    // the whole flick instantly (that instant application was the visible
    // judder when GLFW delivered uneven cursor samples).
    engine::Camera smoothedLook(lookConfig);
    smoothedLook.processLook({100.0f, 0.0f});
    smoothedLook.update(1.0 / 60.0);
    check(smoothedLook.yawDegrees() > lookConfig.startYawDegrees &&
              smoothedLook.yawDegrees() < smoothedLook.targetYawDegrees(),
          "look smoothing partially converges in one frame");

    // Smoothed movement: hold forward for one second, then release for
    // two seconds so acceleration and glide both settle. Different frame
    // rates should produce nearly the same total travel and stop.
    engine::CameraConfig moveConfig;
    moveConfig.lookSmoothingSeconds = 0.0f;
    moveConfig.movementSmoothingSeconds = 0.07f;
    engine::Camera moveAt30Fps(moveConfig);
    engine::Camera moveAt240Fps(moveConfig);
    for (int frame = 0; frame < 30; ++frame) {
        moveAt30Fps.updateMovement({0.0f, 0.0f, 1.0f}, 1.0 / 30.0);
    }
    for (int frame = 0; frame < 60; ++frame) {
        moveAt30Fps.updateMovement({0.0f, 0.0f, 0.0f}, 1.0 / 30.0);
    }
    for (int frame = 0; frame < 240; ++frame) {
        moveAt240Fps.updateMovement({0.0f, 0.0f, 1.0f}, 1.0 / 240.0);
    }
    for (int frame = 0; frame < 480; ++frame) {
        moveAt240Fps.updateMovement({0.0f, 0.0f, 0.0f}, 1.0 / 240.0);
    }
    const float moveDistance30 = glm::length(moveAt30Fps.position() - moveConfig.startPosition);
    const float moveDistance240 = glm::length(moveAt240Fps.position() - moveConfig.startPosition);
    check(std::abs(moveDistance30 - moveDistance240) < 0.05f,
          "smoothed movement distance is frame-rate independent");
    check(glm::length(moveAt30Fps.velocity()) < 1e-3f && glm::length(moveAt240Fps.velocity()) < 1e-3f,
          "smoothed movement velocity settles to zero after release");

    // Cursor capture maps to GLFW's disabled-cursor mode, which is the
    // mode that provides unbounded virtual mouse movement for free look.
    check(engine::Window::cursorModeForCapture(true) == GLFW_CURSOR_DISABLED,
          "captured cursor uses GLFW_CURSOR_DISABLED");
    check(engine::Window::cursorModeForCapture(false) == GLFW_CURSOR_NORMAL,
          "released cursor uses GLFW_CURSOR_NORMAL");
}

void testSceneAndSystems() {
    std::cout << "Scene / ECS systems\n";
    engine::Scene scene;
    check(scene.entityCount() == 0, "new scene is empty");

    // --- Entity creation and component attachment/retrieval -----------------
    engine::Entity cube = scene.createEntity("Cube");
    check(cube.isValid(), "created entity is valid");
    check(cube.name() == "Cube", "entity carries its tag name");
    check(scene.entityCount() == 1, "entity count tracks creation");

    auto& transform = cube.addComponent<engine::TransformComponent>(glm::vec3{1.0f, 2.0f, 3.0f});
    cube.addComponent<engine::MeshComponent>("cube");
    check(cube.hasComponent<engine::TransformComponent>(), "attached component is present");
    check(!cube.hasComponent<engine::CameraComponent>(), "unattached component is absent");
    check(nearlyVec3(cube.getComponent<engine::TransformComponent>().position, glm::vec3{1.0f, 2.0f, 3.0f}),
          "component data round-trips through the registry");
    transform.scale = glm::vec3{2.0f};
    const glm::mat4 model = cube.getComponent<engine::TransformComponent>().modelMatrix();
    check(nearlyEqual(model[3][0], 1.0) && nearlyEqual(model[3][1], 2.0) && nearlyEqual(model[3][2], 3.0),
          "model matrix contains the translation");
    check(nearlyEqual(model[0][0], 2.0), "model matrix contains the scale");

    cube.removeComponent<engine::MeshComponent>();
    check(!cube.hasComponent<engine::MeshComponent>(), "removed component is gone");
    cube.addComponent<engine::MeshComponent>("cube");

    // --- Cameras: active selection -------------------------------------------
    engine::Entity inactiveCam = scene.createEntity("InactiveCamera");
    auto& inactive = inactiveCam.addComponent<engine::CameraComponent>(engine::CameraConfig{});
    inactive.isActive = false;
    check(scene.findActiveCamera() == nullptr, "inactive camera is not selected");

    engine::Entity player = scene.createEntity("Player");
    auto& playerCamera = player.addComponent<engine::CameraComponent>(engine::CameraConfig{});
    check(scene.findActiveCamera() == &playerCamera, "first active camera is selected");

    // --- System execution order: update systems, then render prep ------------
    // Spinning cube integrated by the fixed-step rotation system.
    cube.addComponent<engine::SpinComponent>(glm::vec3{0.0f, 90.0f, 0.0f});
    engine::RotationSystem::update(scene, 0.5); // half a second at 90 deg/s
    check(nearlyEqual(cube.getComponent<engine::TransformComponent>().rotationDegrees.y, 45.0),
          "rotation system integrates spin at the fixed rate");

    // Culling disabled here: this test isolates transform/render data flow
    // (the test cube sits behind the default camera). Culling itself is
    // covered by the dedicated frustum tests below.
    engine::FramePrepareOptions noCulling;
    noCulling.frustumCulling = false;
    const engine::PreparedFrame before = engine::RenderSystem::prepareFrame(scene, 16.0f / 9.0f, noCulling);
    check(before.hasCamera, "render prep finds the active camera");
    check(before.renderQueue.size() == 1, "render prep collects visible mesh entities");

    // This is what CameraSystem does with input each frame (look + move);
    // exercised through the camera's window-free API here.
    playerCamera.camera.translate(glm::vec3{0.0f, 0.0f, 1.0f}, 1.0); // 3 units forward
    const engine::PreparedFrame after = engine::RenderSystem::prepareFrame(scene, 16.0f / 9.0f, noCulling);
    check(!nearlyEqual(after.view[3][2], before.view[3][2], 1e-6),
          "render prep after camera update uses the new view matrix");
    check(nearlyEqual(after.renderQueue.items()[0].model[3][0], before.renderQueue.items()[0].model[3][0]),
          "camera movement does not disturb entity transforms");

    // Hidden entities are excluded from rendering.
    engine::Entity hidden = scene.createEntity("Hidden");
    hidden.addComponent<engine::TransformComponent>();
    auto& hiddenMesh = hidden.addComponent<engine::MeshComponent>("cube");
    hiddenMesh.visible = false;
    check(engine::RenderSystem::prepareFrame(scene, 1.0f, noCulling).renderQueue.size() == 1,
          "invisible entities are skipped by render prep");

    // --- Lifetime -------------------------------------------------------------
    const engine::Entity doomed = scene.createEntity("Doomed");
    const size_t countBeforeDestroy = scene.entityCount();
    scene.destroyEntity(doomed);
    check(!doomed.isValid(), "destroyed entity handle reports invalid");
    check(!scene.isValid(doomed), "scene agrees the entity is gone");
    check(scene.entityCount() == countBeforeDestroy - 1, "entity count tracks destruction");
    scene.destroyEntity(doomed); // stale handle: must be a safe no-op
    check(scene.entityCount() == countBeforeDestroy - 1, "double destroy is a no-op");
}

void testPoolAllocator() {
    std::cout << "PoolAllocator\n";
    engine::memory::PoolCore core;

    void* first = core.allocate(64, 8);
    check(first != nullptr, "pool hands out memory");
    std::memset(first, 0xAB, 64);
    core.deallocate(first, 64, 8);

    void* second = core.allocate(64, 8);
    check(second == first, "freed block is reused (no heap round-trip)");
    core.deallocate(second, 64, 8);

    const engine::memory::PoolStats stats = core.stats();
    check(stats.pageCount >= 1, "allocation carved a page");
    check(stats.blocksInUse == 0, "block accounting returns to zero after frees");
    check(stats.blocksReserved > 0, "page blocks are tracked as reserved");

    // Over-aligned requests take the aligned fallback path and stay aligned.
    void* aligned = core.allocate(32, 64);
    check(reinterpret_cast<uintptr_t>(aligned) % 64 == 0, "over-aligned allocation honors alignment");
    core.deallocate(aligned, 32, 64);

    // Oversized requests also fall back and remain writable.
    void* big = core.allocate(64 * 1024, 8);
    check(big != nullptr, "oversized allocation succeeds via fallback");
    std::memset(big, 0xCD, 64 * 1024);
    core.deallocate(big, 64 * 1024, 8);

    // STL allocator facade: rebound types share one core.
    engine::memory::PoolAllocator<int> intAllocator;
    engine::memory::PoolAllocator<double> doubleAllocator(intAllocator);
    check(intAllocator == doubleAllocator, "rebound allocators share the pool core");
    int* numbers = intAllocator.allocate(4);
    numbers[0] = 42;
    check(numbers[0] == 42, "facade allocation is usable memory");
    intAllocator.deallocate(numbers, 4);
}

void testJobSystem() {
    std::cout << "JobSystem\n";
    engine::JobSystem jobs(engine::JobSystem::Config{4});
    check(jobs.workerCount() == 4, "job system starts the requested workers");

    // Fire-and-forget + barrier.
    std::atomic<int> counter{0};
    constexpr int kJobs = 500;
    for (int i = 0; i < kJobs; ++i) {
        jobs.submit([&counter] { counter.fetch_add(1, std::memory_order_relaxed); });
    }
    jobs.waitForAll();
    check(counter.load() == kJobs, "waitForAll barrier completes every submitted job");

    // parallelFor covers every index exactly once.
    constexpr size_t kItems = 10000;
    std::vector<std::atomic<int>> hits(kItems);
    for (auto& hit : hits) {
        hit.store(0);
    }
    jobs.parallelFor(kItems, 0, [&hits](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            hits[i].fetch_add(1, std::memory_order_relaxed);
        }
    });
    bool allOnce = true;
    for (const auto& hit : hits) {
        allOnce = allOnce && hit.load() == 1;
    }
    check(allOnce, "parallelFor covers the range exactly once");

    std::atomic<long long> sum{0};
    jobs.parallelFor(1000, 100, [&sum](size_t begin, size_t end) {
        long long local = 0;
        for (size_t i = begin; i < end; ++i) {
            local += static_cast<long long>(i);
        }
        sum.fetch_add(local, std::memory_order_relaxed);
    });
    check(sum.load() == 1000LL * 999 / 2, "parallelFor reduction result is correct");

    // Concurrent producers: 8 threads hammer submit() simultaneously;
    // every job must execute exactly once (no loss, no duplication).
    {
        constexpr int kProducers = 8;
        constexpr int kPerProducer = 2500;
        constexpr int kTotal = kProducers * kPerProducer;
        std::vector<std::atomic<int>> hits(kTotal);
        for (auto& hit : hits) {
            hit.store(0, std::memory_order_relaxed);
        }
        std::vector<std::thread> producers;
        producers.reserve(kProducers);
        for (int p = 0; p < kProducers; ++p) {
            producers.emplace_back([&jobs, &hits, p] {
                for (int i = 0; i < kPerProducer; ++i) {
                    const int index = p * kPerProducer + i;
                    jobs.submit([&hits, index] { hits[index].fetch_add(1, std::memory_order_relaxed); });
                }
            });
        }
        for (std::thread& producer : producers) {
            producer.join();
        }
        jobs.waitForAll();
        bool allOnce = true;
        for (const auto& hit : hits) {
            allOnce = allOnce && hit.load(std::memory_order_relaxed) == 1;
        }
        check(allOnce, "concurrent producers: 20k submitted jobs execute exactly once");
    }

    // A throwing job is contained: counted, logged, workers keep going.
    jobs.submit([] { throw std::runtime_error("expected test failure"); });
    std::atomic<bool> afterThrow{false};
    jobs.submit([&afterThrow] { afterThrow.store(true); });
    jobs.waitForAll();
    check(afterThrow.load(), "workers survive a throwing job");
    check(jobs.failedJobCount() >= 1, "throwing job is counted as failed");
}

void testHudOverlay() {
    std::cout << "HUD overlay\n";

    check(engine::buildHudVertices("", 12, 12, 2, 1280, 720).empty(), "empty HUD text builds no vertices");
    check(engine::buildHudVertices("A", 12, 12, 2, 0, 720).empty(), "zero viewport builds no vertices");

    // Vertex count = filled font pixels x 6 (two triangles per pixel).
    size_t filledPixels = 0;
    for (const std::string_view row : engine::hudGlyphFor('A')) {
        for (const char c : row) {
            filledPixels += c == '#' ? 1 : 0;
        }
    }
    const std::vector<engine::HudVertex> vertices = engine::buildHudVertices("A", 12, 12, 2, 1280, 720);
    check(vertices.size() == filledPixels * 6, "glyph layout emits two triangles per filled pixel");

    bool insideNdc = true;
    for (const engine::HudVertex& vertex : vertices) {
        insideNdc = insideNdc && vertex.x >= -1.0f && vertex.x <= 1.0f && vertex.y >= -1.0f && vertex.y <= 1.0f;
    }
    check(insideNdc, "HUD vertices stay inside NDC");
    check(!vertices.empty() && vertices.front().x < -0.9f && vertices.front().y < -0.9f,
          "HUD starts in the top-left corner");

    // Second line is laid out below the first (larger NDC y in Vulkan
    // framebuffer space), unknown glyphs fall back safely.
    const std::vector<engine::HudVertex> twoLines = engine::buildHudVertices("A\n~", 12, 12, 2, 1280, 720);
    float secondLineMinY = 2.0f;
    const size_t secondLineStart = vertices.size();
    for (size_t i = secondLineStart; i < twoLines.size(); ++i) {
        secondLineMinY = std::min(secondLineMinY, twoLines[i].y);
    }
    check(twoLines.size() > secondLineStart && secondLineMinY > vertices.front().y,
          "multi-line HUD flows downward with glyph fallback");

    const std::string hud = engine::formatHudText(60.0, 16.67, 3, 12, 3072, 5, 2);
    check(hud.find("FPS 60.0") != std::string::npos && hud.find("FRAME 16.67 MS") != std::string::npos,
          "HUD text carries FPS and frame time");
    check(hud.find("WORKERS 3+1") != std::string::npos && hud.find("VRAM 12/3072 MIB") != std::string::npos,
          "HUD text carries worker count and VRAM usage");
    check(hud.find("DRAWN 5 CULLED 2") != std::string::npos, "HUD text carries draw statistics");
}

void testWindowLoopIntegration() {
    std::cout << "Window loop integration\n";
    // The real GLFW window cannot open in CI, so this verifies the exact
    // callback contract Application wires into GameLoop: per frame the
    // order is poll -> fixed -> update -> render, render always last,
    // and the loop terminates when shouldContinue reports the window
    // close (3 frames simulated here).
    engine::LoopConfig config;
    config.fixedDeltaSeconds = 0.001;
    config.maxFixedStepsPerFrame = 100;
    config.maxFrameSeconds = 0.25;
    config.maxFps = 0;
    engine::GameLoop loop(config);

    std::vector<std::string> order;
    int frames = 0;
    engine::LoopCallbacks callbacks;
    callbacks.shouldContinue = [&frames] { return frames < 3; };
    callbacks.pollEvents = [&order] { order.push_back("poll"); };
    callbacks.fixedUpdate = [&order](double) { order.push_back("fixed"); };
    callbacks.update = [&order](double) { order.push_back("update"); };
    callbacks.render = [&order, &frames](double) {
        order.push_back("render");
        ++frames;
    };
    loop.run(callbacks);

    check(frames == 3, "window loop runs until the close condition");
    size_t renders = 0;
    bool renderAlwaysAfterUpdate = true;
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == "render") {
            ++renders;
            renderAlwaysAfterUpdate = renderAlwaysAfterUpdate && i > 0 && order[i - 1] == "update";
        }
    }
    check(renders == 3 && renderAlwaysAfterUpdate, "render is the last callback of every frame");
    check(loop.stats().frames == 3, "loop statistics count the integrated frames");
}

void testHardwareProfile() {
    std::cout << "Hardware profile (i5-3470 / GTX 970)\n";
    namespace hw = engine::hardware;

    check(hw::targetPhysicalCores == 4 && hw::recommendedWorkerCount == 3,
          "profile: 3 workers + main fill the 4 physical cores");
    check(hw::cacheLineBytes == 64 && hw::l2CacheBytes == 256 * 1024,
          "profile: 64-byte cache lines, 256 KiB per-core L2");
    check(hw::gpuVramFastBytes == hw::gpuVramTotalBytes - hw::gpuVramTotalBytes / 8,
          "profile: GTX 970 fast VRAM segment is 3.5 of 4 GiB");
    check(hw::recommendedVramBudgetBytes < hw::gpuVramFastBytes,
          "profile: VRAM budget stays inside the fast segment");

    // Worker resolution: explicit wins; automatic never oversubscribes.
    check(hw::resolveWorkerCount(4, 4) == 4, "explicit worker count always wins");
    check(hw::resolveWorkerCount(0, 4) == 3, "auto workers on 4 threads = 3 (main keeps a core)");
    check(hw::resolveWorkerCount(0, 8) == 3, "auto workers capped at physical cores under Hyper-Threading");
    check(hw::resolveWorkerCount(0, 2) == 1, "auto workers on 2 threads = 1");
    check(hw::resolveWorkerCount(0, 0) == 3, "unknown host concurrency falls back to the profile");

    // A default-constructed JobSystem must respect the same cap.
    {
        engine::JobSystem jobs;
        const uint32_t expected =
            hw::resolveWorkerCount(0, std::thread::hardware_concurrency());
        check(jobs.workerCount() == expected && jobs.workerCount() <= hw::recommendedWorkerCount,
              "default JobSystem worker count matches the profile cap");
    }

    // Batch granularity: ~4 batches/worker, capped at the L2-sized batch.
    check(hw::resolveParallelBatchSize(100, 3, 0) == 9, "small ranges split ~4 ways per worker");
    check(hw::resolveParallelBatchSize(1000000, 3, 0) == hw::parallelBatchItems,
          "huge ranges cap at the L2-friendly batch size");
    check(hw::resolveParallelBatchSize(1000000, 3, 77) == 77, "configured batch size always wins");
    check(hw::resolveParallelBatchSize(0, 3, 0) == 1, "empty range resolves to a safe batch");

    // Defaults in the config system match the profile recommendations.
    const engine::EngineConfig defaults;
    check(defaults.performance.parallelBatchSize == hw::parallelBatchItems,
          "config default batch size matches the profile");
    check(defaults.render.vramBudgetMiB * 1024ull * 1024ull == hw::recommendedVramBudgetBytes,
          "config default VRAM budget matches the profile");
    check(defaults.render.stagingBufferBudgetMiB * 1024ull * 1024ull ==
              hw::recommendedStagingBudgetBytes,
          "config default staging budget matches the profile");
}

void testVramBudget() {
    std::cout << "VramBudget\n";
    engine::VramBudget budget(1000);
    check(budget.tryReserve(600) && budget.usedBytes() == 600, "reserve within budget succeeds");
    check(!budget.tryReserve(500) && budget.usedBytes() == 600, "over-budget reserve fails without side effects");
    check(budget.remainingBytes() == 400, "remaining budget tracks reservations");
    budget.release(200);
    check(budget.tryReserve(500) && budget.usedBytes() == 900, "released budget can be reserved again");
    budget.release(100000);
    check(budget.usedBytes() == 0 && budget.remainingBytes() == 1000, "release saturates at zero");

    // Concurrent reservers can never push past the cap.
    engine::VramBudget contended(10000);
    std::atomic<int> successes{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t) {
        threads.emplace_back([&contended, &successes] {
            for (int i = 0; i < 100; ++i) {
                if (contended.tryReserve(100)) {
                    successes.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    check(successes.load() == 100 && contended.usedBytes() == 10000,
          "concurrent reservations stop exactly at the budget cap");
}

void testAssetManagement() {
    std::cout << "Asset management & materials\n";

    engine::VramBudget vram(1024 * 1024);
    engine::VramBudget staging(1024 * 1024);
    {
        engine::AssetManager assets(vram, staging);

        // Mesh registration: stable id, computed bounds, VRAM accounting.
        const engine::MeshId cubeId = assets.registerMesh("cube", engine::Cube::vertices(), engine::Cube::indices());
        check(cubeId == 1, "first mesh gets stable id 1");
        const engine::MeshAsset* cube = assets.mesh("cube");
        check(cube != nullptr && cube->vramBytes > 0, "mesh VRAM footprint accounted");
        check(cube != nullptr && nearlyEqual(cube->bounds.extents().x, 0.5, 1e-4),
              "mesh bounds computed from vertices");
        check(vram.usedBytes() == cube->vramBytes, "mesh registration reserves VRAM budget");
        check(assets.registerMesh("cube", engine::Cube::vertices(), engine::Cube::indices()) == cubeId &&
                  assets.meshCount() == 1,
              "mesh registration is idempotent by name");

        // Shader registration.
        const std::vector<uint32_t> fakeSpirv = {0x07230203u, 0x00010000u};
        check(assets.registerShader("test", fakeSpirv) == 1 && assets.shader(1)->byteSize == 8,
              "shader registered with stable id and byte size");
        check(assets.loadShader("missing", "definitely/not/a/file.spv") == engine::kInvalidAssetId,
              "missing shader file yields invalid id");

        // Materials: dense stable ids + descriptor indices.
        engine::MaterialDesc defaultDesc;
        defaultDesc.name = "default";
        engine::MaterialDesc doubleSided = defaultDesc;
        doubleSided.name = "doubleSided";
        doubleSided.pipeline.cullMode = engine::CullMode::None;
        const engine::MaterialId defaultId = assets.registerMaterial(defaultDesc);
        const engine::MaterialId doubleId = assets.registerMaterial(doubleSided);
        check(defaultId == 1 && doubleId == 2, "material ids are dense and stable");
        check(assets.material("doubleSided")->descriptorIndex == 1,
              "descriptor index is the dense material slot");
        check(assets.registerMaterial(defaultDesc) == defaultId && assets.materialCount() == 2,
              "material registration is idempotent by name");

        // PSO cache: same state interns to the same PSO, new state creates.
        engine::PipelineStateCache psoCache;
        const uint32_t psoA = psoCache.intern(engine::PipelineStateDesc{});
        engine::PipelineStateDesc otherState;
        otherState.cullMode = engine::CullMode::None;
        const uint32_t psoB = psoCache.intern(otherState);
        check(psoA == 1 && psoB == 2 && psoCache.intern(engine::PipelineStateDesc{}) == psoA &&
                  psoCache.creationCount() == 2,
              "PSO cache deduplicates states deterministically");
        check(engine::hashPipelineState(engine::PipelineStateDesc{}) != engine::hashPipelineState(otherState),
              "pipeline state hash distinguishes states");
    }
    check(vram.usedBytes() == 0, "asset manager releases VRAM reservations on destruction");

    // Budget discipline: an undersized budget rejects registration cleanly.
    engine::VramBudget tinyVram(64);
    engine::VramBudget tinyStaging(64);
    engine::AssetManager limited(tinyVram, tinyStaging);
    check(limited.registerMesh("cube", engine::Cube::vertices(), engine::Cube::indices()) ==
              engine::kInvalidAssetId,
          "mesh exceeding the VRAM budget is rejected (no over-commit)");

    // Material resolution through RenderSystem: DrawItems carry the
    // registered stable ids and descriptor indices, sorted by material.
    engine::VramBudget bigVram(1024 * 1024);
    engine::VramBudget bigStaging(1024 * 1024);
    engine::AssetManager assets(bigVram, bigStaging);
    engine::MaterialDesc matA;
    matA.name = "matA";
    engine::MaterialDesc matB;
    matB.name = "matB";
    const engine::MaterialId idA = assets.registerMaterial(matA);
    const engine::MaterialId idB = assets.registerMaterial(matB);

    engine::Scene scene;
    engine::Entity player = scene.createEntity("Player");
    player.addComponent<engine::CameraComponent>(engine::CameraConfig{});
    const auto addCube = [&scene](float z, const char* material) {
        engine::Entity cube = scene.createEntity("Cube");
        cube.addComponent<engine::TransformComponent>(glm::vec3{0.0f, 0.0f, z});
        auto& mesh = cube.addComponent<engine::MeshComponent>("cube");
        mesh.materialName = material;
    };
    addCube(-1.0f, "matB");
    addCube(-4.0f, "matA");
    addCube(-2.0f, "matB");

    engine::FramePrepareOptions options;
    options.assets = &assets;
    options.frustumCulling = false;
    const engine::PreparedFrame frame = engine::RenderSystem::prepareFrame(scene, 16.0f / 9.0f, options);
    bool idsOk = frame.renderQueue.size() == 3;
    bool indicesOk = true;
    for (const engine::DrawItem& item : frame.renderQueue.items()) {
        idsOk = idsOk && (item.materialId == idA || item.materialId == idB);
        indicesOk = indicesOk && item.descriptorIndex == item.materialId - 1;
    }
    check(idsOk, "render items carry stable registered material ids");
    check(indicesOk, "render items carry dense descriptor indices");
    check(frame.renderQueue.items().front().materialId == std::min(idA, idB),
          "render queue groups by registered material id");
}

void testLockFreeRingBuffer() {
    std::cout << "LockFreeRingBuffer\n";
    using engine::concurrency::LockFreeRingBuffer;

    check(alignof(LockFreeRingBuffer<int>) >= 64, "ring buffer type is cache-line aligned");

    // Sequential semantics: power-of-two capacity, FIFO order, full/empty.
    LockFreeRingBuffer<int> queue(7);
    check(queue.capacity() == 8, "capacity rounds up to a power of two");
    check(queue.emptyApprox(), "new ring buffer is empty");
    bool allPushed = true;
    for (int i = 0; i < 8; ++i) {
        allPushed = allPushed && queue.tryPush(i);
    }
    check(allPushed && queue.sizeApprox() == 8, "ring buffer fills to capacity");
    int extra = 99;
    check(!queue.tryPush(extra) && extra == 99, "push to a full ring fails without consuming the value");
    bool fifoOrder = true;
    for (int i = 0; i < 8; ++i) {
        int value = -1;
        fifoOrder = fifoOrder && queue.tryPop(value) && value == i;
    }
    int value = -1;
    check(fifoOrder && queue.emptyApprox() && !queue.tryPop(value), "ring buffer drains in FIFO order");

    // Wrap-around: indices lap the ring 100k times in sequence.
    LockFreeRingBuffer<int> wrap(8);
    bool wrapOk = true;
    for (int i = 0; i < 100000; ++i) {
        int out = -1;
        wrapOk = wrapOk && wrap.tryPush(i) && wrap.tryPop(out) && out == i;
    }
    check(wrapOk && wrap.emptyApprox(), "ring buffer wraps around cleanly (100k laps)");

    // MPMC stress: 4 producers x 4 consumers, 20k unique items. Every
    // item must be consumed exactly once — the definitive lost/duplicated
    // work check for the lock-free protocol.
    {
        constexpr int kProducers = 4;
        constexpr int kConsumers = 4;
        constexpr int kPerProducer = 5000;
        constexpr int kTotal = kProducers * kPerProducer;
        LockFreeRingBuffer<int> mpmc(1024);
        std::vector<std::atomic<int>> hits(kTotal);
        for (auto& hit : hits) {
            hit.store(0, std::memory_order_relaxed);
        }
        std::atomic<int> consumed{0};
        std::vector<std::thread> threads;
        threads.reserve(kProducers + kConsumers);
        for (int p = 0; p < kProducers; ++p) {
            threads.emplace_back([&mpmc, p] {
                for (int i = 0; i < kPerProducer; ++i) {
                    const int item = p * kPerProducer + i;
                    while (!mpmc.tryPush(item)) {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (int c = 0; c < kConsumers; ++c) {
            threads.emplace_back([&mpmc, &hits, &consumed] {
                int item = -1;
                while (consumed.load(std::memory_order_relaxed) < kTotal) {
                    if (mpmc.tryPop(item)) {
                        hits[item].fetch_add(1, std::memory_order_relaxed);
                        consumed.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        std::this_thread::yield();
                    }
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        bool allOnce = true;
        for (const auto& hit : hits) {
            allOnce = allOnce && hit.load(std::memory_order_relaxed) == 1;
        }
        check(allOnce, "MPMC stress: 20k items consumed exactly once (none lost, none duplicated)");
        check(consumed.load() == kTotal && mpmc.emptyApprox(), "MPMC stress drains the ring completely");
    }
}

void testAsyncLoggerConcurrency() {
    std::cout << "Async logger concurrency\n";
    const auto dir = makeTempDir("engine_test_async_logger");

    engine::LoggerConfig config;
    config.minimumLevel = engine::LogLevel::Info;
    config.consoleEnabled = false;
    config.fileEnabled = true;
    config.filePath = dir / "async.log";
    config.timestampedFileName = false;
    config.asyncFileWrites = true;
    config.asyncQueueCapacity = 8192;

    engine::Logger& logger = engine::Logger::instance();
    logger.initialize(config);

    // Multiple producer threads log through the lock-free ingestion ring
    // at once; after shutdown every message must be in the file.
    constexpr int kProducers = 4;
    constexpr int kPerProducer = 250;
    constexpr int kTotal = kProducers * kPerProducer;
    std::vector<std::thread> producers;
    producers.reserve(kProducers);
    for (int p = 0; p < kProducers; ++p) {
        producers.emplace_back([p] {
            for (int i = 0; i < kPerProducer; ++i) {
                ENGINE_LOG_INFO("async-ingest-" << p << '-' << i);
            }
        });
    }
    for (std::thread& producer : producers) {
        producer.join();
    }
    logger.shutdown();

    const std::string contents = readWholeFile(dir / "async.log");
    size_t found = 0;
    size_t pos = 0;
    while ((pos = contents.find("async-ingest-", pos)) != std::string::npos) {
        ++found;
        ++pos;
    }
    check(found == static_cast<size_t>(kTotal), "async logger delivers every concurrent message (lock-free ring)");

    std::filesystem::remove_all(dir);
}

void testRenderQueue() {
    std::cout << "RenderQueue sorting\n";

    const auto makeItem = [](uint32_t material, float depth, float id) {
        engine::DrawItem item;
        item.model[3][0] = id; // encode identity in the translation
        item.materialId = material;
        item.viewDepth = depth;
        item.sortKey = engine::RenderQueue::makeSortKey(material, depth);
        return item;
    };

    // Sort key structure: material dominates, depth orders within it.
    check(engine::RenderQueue::makeSortKey(1, 100.0f) < engine::RenderQueue::makeSortKey(2, 0.5f),
          "sort key orders by material before depth");
    check(engine::RenderQueue::makeSortKey(1, 0.5f) < engine::RenderQueue::makeSortKey(1, 2.0f),
          "sort key orders front-to-back within a material");

    engine::RenderQueue queue;
    queue.reserve(6);
    queue.push(makeItem(2, 1.0f, 0));
    queue.push(makeItem(1, 5.0f, 1));
    queue.push(makeItem(1, 2.0f, 2));
    queue.push(makeItem(2, 0.5f, 3));
    queue.push(makeItem(1, 2.0f, 4)); // exact key tie with id 2: stability check
    queue.push(makeItem(3, 0.1f, 5));
    queue.sort();

    // Expected: mat1 {id2, id4 (tie, stable), id1}, mat2 {id3, id0}, mat3 {id5}.
    const std::vector<float> expectedIds = {2, 4, 1, 3, 0, 5};
    bool orderOk = queue.size() == expectedIds.size();
    bool keysAscending = true;
    for (size_t i = 0; i < queue.size() && i < expectedIds.size(); ++i) {
        orderOk = orderOk && queue.items()[i].model[3][0] == expectedIds[i];
        keysAscending = keysAscending && (i == 0 || queue.items()[i - 1].sortKey <= queue.items()[i].sortKey);
    }
    check(orderOk, "sequential sort: material groups, front-to-back, stable ties");
    check(keysAscending, "sorted queue keys are non-decreasing");

    // Command-stream integrity under the parallel sort: 5000 unique keys,
    // every item present exactly once, fully ordered, deterministic.
    engine::JobSystem jobs(engine::JobSystem::Config{4});
    const auto buildQueue = [&makeItem] {
        engine::RenderQueue big;
        big.reserve(5000);
        for (int i = 0; i < 5000; ++i) {
            // (i * 37) % 5000 is a permutation (gcd(37,5000)=1): unique keys.
            const float keyDepth = static_cast<float>((i * 37) % 5000);
            big.push(makeItem(static_cast<uint32_t>(i % 7), keyDepth, static_cast<float>(i)));
        }
        return big;
    };
    engine::RenderQueue parallel = buildQueue();
    parallel.sort(&jobs, 1); // force the parallel path
    bool sorted = true;
    std::vector<int> seen(5000, 0);
    for (size_t i = 0; i < parallel.size(); ++i) {
        sorted = sorted && (i == 0 || parallel.items()[i - 1].sortKey <= parallel.items()[i].sortKey);
        seen[static_cast<size_t>(parallel.items()[i].model[3][0])] += 1;
    }
    bool allOnce = true;
    for (const int count : seen) {
        allOnce = allOnce && count == 1;
    }
    check(sorted && allOnce, "parallel sort: 5000 items ordered, none lost or duplicated");

    engine::RenderQueue parallelAgain = buildQueue();
    parallelAgain.sort(&jobs, 1);
    bool deterministic = parallelAgain.size() == parallel.size();
    for (size_t i = 0; i < parallel.size() && i < parallelAgain.size(); ++i) {
        deterministic = deterministic && parallelAgain.items()[i].sortKey == parallel.items()[i].sortKey &&
                        parallelAgain.items()[i].model[3][0] == parallel.items()[i].model[3][0];
    }
    check(deterministic, "parallel sort is deterministic across runs");

    queue.clear();
    check(queue.empty(), "cleared render queue is empty (capacity retained for reuse)");
}

void testRenderSortingIntegration() {
    std::cout << "Render queue integration\n";
    engine::JobSystem jobs(engine::JobSystem::Config{4});
    engine::Scene scene;

    engine::Entity player = scene.createEntity("Player");
    player.addComponent<engine::CameraComponent>(engine::CameraConfig{});

    // Two materials interleaved in depth; default camera at (0,0,3)
    // looking down -Z, so view depth = 3 - z.
    const auto addCube = [&scene](float x, float z, const char* material) {
        engine::Entity cube = scene.createEntity("Cube");
        cube.addComponent<engine::TransformComponent>(glm::vec3{x, 0.0f, z});
        auto& mesh = cube.addComponent<engine::MeshComponent>("cube");
        mesh.materialName = material;
    };
    addCube(0.0f, -1.0f, "matA"); // depth 4
    addCube(1.0f, 1.0f, "matB");  // depth 2
    addCube(-1.0f, -4.0f, "matA"); // depth 7
    addCube(2.0f, 0.0f, "matB");  // depth 3

    engine::FramePrepareOptions options;
    options.jobs = &jobs;
    options.frustumCulling = false;
    options.parallelMinItems = 1;

    const engine::PreparedFrame frame = engine::RenderSystem::prepareFrame(scene, 16.0f / 9.0f, options);
    check(frame.renderQueue.size() == 4, "sorted frame contains every visible mesh");

    bool keysAscending = true;
    bool groupsContiguous = true;
    bool depthsAscendingInGroup = true;
    std::vector<uint32_t> seenMaterials;
    for (size_t i = 0; i < frame.renderQueue.size(); ++i) {
        const engine::DrawItem& item = frame.renderQueue.items()[i];
        if (i > 0) {
            const engine::DrawItem& prev = frame.renderQueue.items()[i - 1];
            keysAscending = keysAscending && prev.sortKey <= item.sortKey;
            if (item.materialId == prev.materialId) {
                depthsAscendingInGroup = depthsAscendingInGroup && prev.viewDepth <= item.viewDepth;
            }
        }
        if (std::find(seenMaterials.begin(), seenMaterials.end(), item.materialId) == seenMaterials.end()) {
            seenMaterials.push_back(item.materialId);
        } else if (seenMaterials.back() != item.materialId) {
            groupsContiguous = false; // a material reappears after another group
        }
    }
    check(keysAscending, "integrated frame: sort keys non-decreasing");
    check(groupsContiguous && seenMaterials.size() == 2, "integrated frame: materials form contiguous groups");
    check(depthsAscendingInGroup, "integrated frame: front-to-back inside each material");

    // Deterministic across repeated parallel preparation.
    const engine::PreparedFrame again = engine::RenderSystem::prepareFrame(scene, 16.0f / 9.0f, options);
    bool sameOrder = again.renderQueue.size() == frame.renderQueue.size();
    for (size_t i = 0; i < frame.renderQueue.size() && i < again.renderQueue.size(); ++i) {
        sameOrder = sameOrder && again.renderQueue.items()[i].sortKey == frame.renderQueue.items()[i].sortKey &&
                    again.renderQueue.items()[i].model[3][0] == frame.renderQueue.items()[i].model[3][0];
    }
    check(sameOrder, "integrated frame: repeated parallel preparation is deterministic");

    // Sorting can be switched off (collection order preserved, all items kept).
    options.sortDrawItems = false;
    const engine::PreparedFrame unsorted = engine::RenderSystem::prepareFrame(scene, 16.0f / 9.0f, options);
    check(unsorted.renderQueue.size() == 4, "unsorted frame keeps every mesh");
}

void testFrustumCulling() {
    std::cout << "Frustum culling\n";
    engine::CameraConfig cameraConfig;
    cameraConfig.startPosition = glm::vec3{0.0f, 0.0f, 0.0f};
    const engine::Camera camera(cameraConfig); // at origin, looking down -Z
    const engine::Frustum frustum{camera.projectionMatrix(16.0f / 9.0f) * camera.viewMatrix()};

    const engine::AABB inFront{glm::vec3{-0.5f, -0.5f, -3.5f}, glm::vec3{0.5f, 0.5f, -2.5f}};
    const engine::AABB behind{glm::vec3{-0.5f, -0.5f, 2.5f}, glm::vec3{0.5f, 0.5f, 3.5f}};
    const engine::AABB farAway{glm::vec3{-0.5f, -0.5f, -500.0f}, glm::vec3{0.5f, 0.5f, -499.0f}};
    const engine::AABB farRight{glm::vec3{50.0f, -0.5f, -3.5f}, glm::vec3{51.0f, 0.5f, -2.5f}};
    const engine::AABB crossingNear{glm::vec3{-0.5f, -0.5f, -0.2f}, glm::vec3{0.5f, 0.5f, 0.2f}};

    check(frustum.intersects(inFront), "box in front of the camera is visible");
    check(!frustum.intersects(behind), "box behind the camera is culled");
    check(!frustum.intersects(farAway), "box beyond the far plane is culled");
    check(!frustum.intersects(farRight), "box outside the right plane is culled");
    check(frustum.intersects(crossingNear), "box crossing the near plane stays visible");
    check(frustum.intersectsSphere(glm::vec3{0.0f, 0.0f, -5.0f}, 1.0f), "sphere in view intersects");
    check(!frustum.intersectsSphere(glm::vec3{0.0f, 0.0f, 5.0f}, 1.0f), "sphere behind is culled");

    // World bounds of a rotated box must grow (rotation is accounted for).
    const glm::mat4 rotated = glm::rotate(glm::mat4{1.0f}, glm::radians(45.0f), glm::vec3{0.0f, 0.0f, 1.0f});
    const engine::AABB unitBounds{glm::vec3{-0.5f}, glm::vec3{0.5f}};
    const engine::AABB worldBounds = unitBounds.transformed(rotated);
    check(worldBounds.extents().x > 0.7f && worldBounds.extents().x < 0.71f,
          "rotated AABB expands to the diagonal extent");
}

void testParallelSystemsAndCulling() {
    std::cout << "Parallel systems + culling integration\n";
    engine::JobSystem jobs(engine::JobSystem::Config{4});
    engine::Scene scene;
    scene.reserveCapacity(256);
    check(scene.memoryStats().blocksReserved > 0, "scene reserve allocates pooled storage up front");

    engine::CameraConfig cameraConfig;
    cameraConfig.startPosition = glm::vec3{0.0f, 0.0f, 3.0f};
    engine::Entity player = scene.createEntity("Player");
    player.addComponent<engine::CameraComponent>(cameraConfig);

    // 100 spinning cubes arranged in front of the camera.
    constexpr int kCubes = 100;
    for (int i = 0; i < kCubes; ++i) {
        engine::Entity cube = scene.createEntity("Cube" + std::to_string(i));
        auto& transform = cube.addComponent<engine::TransformComponent>(
            glm::vec3{static_cast<float>(i % 10) - 4.5f, static_cast<float>(i / 10) - 4.5f, -5.0f});
        transform.scale = glm::vec3{0.3f};
        cube.addComponent<engine::MeshComponent>("cube");
        cube.addComponent<engine::SpinComponent>(glm::vec3{0.0f, 90.0f, 0.0f});
    }
    // One cube behind the camera: must be frustum-culled.
    engine::Entity behind = scene.createEntity("BehindCamera");
    behind.addComponent<engine::TransformComponent>(glm::vec3{0.0f, 0.0f, 10.0f});
    behind.addComponent<engine::MeshComponent>("cube");

    // Parallel fixed-step rotation: every cube turns exactly 45 degrees.
    engine::RotationSystem::update(scene, 0.5, jobs);
    bool allRotated = true;
    scene.registry().view<engine::TransformComponent, engine::SpinComponent>().each(
        [&allRotated](const engine::TransformComponent& transform, const engine::SpinComponent&) {
            allRotated = allRotated && nearlyEqual(transform.rotationDegrees.y, 45.0);
        });
    check(allRotated, "parallel rotation updates every entity exactly once");

    // Parallel render preparation (threshold lowered to force the path).
    engine::FramePrepareOptions options;
    options.jobs = &jobs;
    options.frustumCulling = true;
    options.parallelMinItems = 1;
    const engine::PreparedFrame frame = engine::RenderSystem::prepareFrame(scene, 16.0f / 9.0f, options);
    check(frame.hasCamera, "render prep finds camera in populated scene");
    check(frame.meshCount == static_cast<uint32_t>(kCubes) + 1, "render prep counts all visible meshes");
    check(frame.culledCount >= 1, "behind-camera cube is frustum-culled");
    check(frame.renderQueue.size() + frame.culledCount == frame.meshCount,
          "drawn + culled accounts for every mesh");

    // Without culling, everything is drawn.
    options.frustumCulling = false;
    const engine::PreparedFrame unculled = engine::RenderSystem::prepareFrame(scene, 16.0f / 9.0f, options);
    check(unculled.culledCount == 0 && unculled.renderQueue.size() == frame.meshCount,
          "disabling culling draws every mesh");
}

void testDataOrientedLayout() {
    std::cout << "Data-oriented layout\n";

    // Compile-time layout contract (static_asserts also live in Components.h).
    check(sizeof(engine::TransformComponent) == 64 && alignof(engine::TransformComponent) == 64,
          "TransformComponent occupies exactly one 64-byte cache line");
    check(sizeof(engine::SpinComponent) == 16 && alignof(engine::SpinComponent) == 16,
          "SpinComponent packs densely at 16 bytes");

    // Pool allocator serves cache-line-aligned component arrays.
    engine::memory::PoolAllocator<engine::TransformComponent> allocator;
    engine::TransformComponent* array = allocator.allocate(16);
    check(reinterpret_cast<uintptr_t>(array) % 64 == 0, "pooled transform array is 64-byte aligned");
    const auto byteStride = reinterpret_cast<const char*>(&array[5]) - reinterpret_cast<const char*>(&array[2]);
    check(byteStride == 3 * static_cast<std::ptrdiff_t>(sizeof(engine::TransformComponent)),
          "pooled array elements are densely packed with no gaps");
    for (int i = 0; i < 16; ++i) {
        array[i].position = glm::vec3{static_cast<float>(i)};
    }
    check(nearlyVec3(array[15].position, glm::vec3{15.0f}), "pooled array storage is fully usable");
    allocator.deallocate(array, 16);

    // EnTT dense storage: components of sequentially created entities form
    // one contiguous, cache-line-aligned array.
    engine::Scene scene;
    constexpr int kEntities = 1000;
    constexpr int kStaticTransforms = 8;
    std::vector<engine::Entity> staticEntities;
    staticEntities.reserve(kStaticTransforms);
    for (int i = 0; i < 3; ++i) {
        engine::Entity entity = scene.createEntity("StaticBefore" + std::to_string(i));
        entity.addComponent<engine::TransformComponent>(glm::vec3{-1.0f, 0.0f, 0.0f});
        staticEntities.push_back(entity);
    }
    std::vector<engine::Entity> entities;
    entities.reserve(kEntities);
    for (int i = 0; i < kEntities; ++i) {
        engine::Entity entity = scene.createEntity("Packed" + std::to_string(i));
        entity.addComponent<engine::TransformComponent>(glm::vec3{static_cast<float>(i), 0.0f, 0.0f});
        auto& spin = entity.addComponent<engine::SpinComponent>();
        spin.degreesPerSecond = glm::vec3{0.0f, static_cast<float>(i % 8) * 10.0f, 0.0f};
        entities.push_back(entity);
    }
    for (int i = 3; i < kStaticTransforms; ++i) {
        engine::Entity entity = scene.createEntity("StaticAfter" + std::to_string(i));
        entity.addComponent<engine::TransformComponent>(glm::vec3{-1.0f, 0.0f, 0.0f});
        staticEntities.push_back(entity);
    }

    // EnTT packs components into dense pages (1024 elements per page, one
    // contiguous allocation per page). All 1008 transforms fit in a single
    // page, which must be one cache-line-aligned, gap-free array.
    const auto& transformStorage = scene.registry().storage<engine::TransformComponent>();
    const engine::TransformComponent* const* pages = transformStorage.raw();
    const engine::TransformComponent* page = pages[0];
    check(page != nullptr &&
              transformStorage.size() == static_cast<size_t>(kEntities + kStaticTransforms),
          "transform storage holds every component densely");
    check(reinterpret_cast<uintptr_t>(page) % 64 == 0, "dense page starts on a cache line");
    check(engine::denseElementAt(transformStorage, 5) == page + 5,
          "dense index resolution walks the page as an array");
    bool allAligned = true;
    bool strideExact = true;
    for (int i = 0; i < kEntities + kStaticTransforms; ++i) {
        allAligned = allAligned && (reinterpret_cast<uintptr_t>(page + i) % 64 == 0);
        if (i > 0) {
            strideExact = strideExact && (reinterpret_cast<const char*>(page + i) -
                                          reinterpret_cast<const char*>(page + i - 1)) ==
                                             static_cast<std::ptrdiff_t>(sizeof(engine::TransformComponent));
        }
    }
    check(allAligned, "every transform in the page is cache-line aligned");
    check(strideExact, "consecutive transforms are exactly sizeof(T) apart (zero gaps)");

    // Owning-group pairing: after parallel rotation, each entity must have
    // rotated by *its own* spin — proof the two dense arrays stay
    // index-aligned under the group.
    engine::JobSystem jobs(engine::JobSystem::Config{4});
    engine::RotationSystem::update(scene, 1.0, jobs);
    bool pairingCorrect = true;
    for (int i = 0; i < kEntities; ++i) {
        const float expected = static_cast<float>(i % 8) * 10.0f;
        pairingCorrect = pairingCorrect &&
                         nearlyEqual(entities[i].getComponent<engine::TransformComponent>().rotationDegrees.y,
                                     expected, 1e-4);
    }
    check(pairingCorrect, "group-aligned arrays keep transform/spin pairs matched under parallel rotation");
    bool staticUntouched = true;
    for (engine::Entity entity : staticEntities) {
        staticUntouched = staticUntouched &&
                          nearlyEqual(entity.getComponent<engine::TransformComponent>().rotationDegrees.y,
                                      0.0, 1e-4);
    }
    check(staticUntouched, "owning group leaves non-spinning transforms untouched");

    // High-speed iteration stability: 5000 entities x 40 parallel fixed
    // steps must land every entity on exactly the same angle.
    engine::Scene bigScene;
    constexpr int kBig = 5000;
    for (int i = 0; i < kBig; ++i) {
        engine::Entity entity = bigScene.createEntity("Spinner");
        entity.addComponent<engine::TransformComponent>();
        entity.addComponent<engine::SpinComponent>(glm::vec3{0.0f, 90.0f, 0.0f});
    }
    const auto start = std::chrono::steady_clock::now();
    for (int step = 0; step < 40; ++step) {
        engine::RotationSystem::update(bigScene, 0.25, jobs); // +22.5 deg per step
    }
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    size_t visited = 0;
    bool allAt180 = true;
    bigScene.registry().view<engine::TransformComponent>().each(
        [&visited, &allAt180](const engine::TransformComponent& transform) {
            ++visited;
            // 40 steps * 22.5 deg = 900 deg == 180 deg (mod 360)
            allAt180 = allAt180 && nearlyEqual(transform.rotationDegrees.y, 180.0, 1e-2);
        });
    check(visited == static_cast<size_t>(kBig), "parallel iteration visits every entity");
    check(allAt180, "5000 entities x 40 parallel steps stay exactly in sync");
    std::cout << "  [INFO] 200k parallel component updates took " << elapsedMs << " ms\n";
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
    testCamera();
    testSceneAndSystems();
    testPoolAllocator();
    testHudOverlay();
    testWindowLoopIntegration();
    testHardwareProfile();
    testVramBudget();
    testAssetManagement();
    testLockFreeRingBuffer();
    testJobSystem();
    testAsyncLoggerConcurrency();
    testRenderQueue();
    testRenderSortingIntegration();
    testFrustumCulling();
    testParallelSystemsAndCulling();
    testDataOrientedLayout();
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
