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
#include "Scene/Systems/PhysicsSystem.h"
#include "Scene/Systems/PlayerSystem.h"
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
#include "Graphics/ShadowMap.h"
#include "Graphics/Renderer.h"
#include "Graphics/Swapchain.h"
#include "Core/Memory/PoolAllocator.h"
#include "Graphics/Frustum.h"

#include <atomic>

#include <chrono>
#include <cmath>
#include <cstddef>
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
             << "[Render]\nvsync = false\npresentMode = immediate\nsortDrawItems = false\n"
             << "[Logger]\nlevel = Warning\nconsole = false\n"
             << "[Loop]\nfixedDeltaSeconds = 0.01\nmaxFps = 144\nmaxFixedStepsPerFrame = 3\n"
             << "bogusKey = 1\n"
             << "[Loop]\nmaxFrameSeconds = not_a_number\n"
             << "[Camera]\nmovementSpeed = 7.5\nmouseSensitivity = 0.25\npositionZ = 5\n"
             << "lookSmoothingSeconds = 0.04\nmovementSmoothingSeconds = 0.09\n"
             << "[Input]\ncaptureMouseOnStart = false\n"
             << "[Physics]\ngravityY = -12.5\nsolverIterations = 7\nparallelMinBodies = 32\n"
             << "demoMode = space\nccdEnabled = false\ngravitationalParameter = 42\ngravitySoftening = 0.25\n"
             << "angularDamping = 0.1\ncontactLinearDamping = 12\ncontactAngularDamping = 30\n"
             << "maxPositionCorrection = 0.04\nsleepSpeedThreshold = 0.08\nsleepAngularThreshold = 5\n"
             << "sleepDelaySeconds = 0.75\nwakeSpeedThreshold = 0.3\n"
             << "[Render]\nfrustumCulling = false\n"
             << "[Performance]\njobWorkerCount = 2\nentityReserve = 4096\nparallelBatchSize = 512\n"
             << "[Render]\nvramBudgetMiB = 2048\nstagingBufferBudgetMiB = 128\nhudEnabled = false\n";
    }

    const engine::EngineConfig config = engine::EngineConfig::loadFromFile(path);
    check(config.window.width == 800 && config.window.height == 600, "window size parsed");
    check(config.window.title == "Test Title", "window title parsed");
    check(!config.window.resizable, "window resizable parsed");
    check(!config.render.vsync, "vsync parsed");
    check(config.render.presentMode == engine::PresentationMode::Immediate, "present mode parsed");
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
    check(nearlyEqual(config.physics.gravity.y, -12.5f, 1e-6), "physics gravity parsed");
    check(config.physics.solverIterations == 7, "physics solver iterations parsed");
    check(config.physics.parallelMinBodies == 32, "physics parallel threshold parsed");
    check(config.physics.demoMode == engine::PhysicsDemoMode::Space, "physics demo mode parsed");
    check(!config.physics.ccdEnabled, "physics CCD flag parsed");
    check(nearlyEqual(config.physics.gravitationalParameter, 42.0f, 1e-6), "physics radial gravity parsed");
    check(nearlyEqual(config.physics.gravitySoftening, 0.25f, 1e-6), "physics gravity softening parsed");
    check(nearlyEqual(config.physics.angularDamping, 0.1f, 1e-6), "physics angular damping parsed");
    check(nearlyEqual(config.physics.contactLinearDamping, 12.0f, 1e-6), "physics contact linear damping parsed");
    check(nearlyEqual(config.physics.contactAngularDamping, 30.0f, 1e-6), "physics contact angular damping parsed");
    check(nearlyEqual(config.physics.maxPositionCorrection, 0.04f, 1e-6), "physics max position correction parsed");
    check(nearlyEqual(config.physics.sleepSpeedThreshold, 0.08f, 1e-6), "physics sleep speed threshold parsed");
    check(nearlyEqual(config.physics.sleepAngularThreshold, 5.0f, 1e-6), "physics sleep angular threshold parsed");
    check(nearlyEqual(config.physics.sleepDelaySeconds, 0.75f, 1e-6), "physics sleep delay parsed");
    check(nearlyEqual(config.physics.wakeSpeedThreshold, 0.3f, 1e-6), "physics wake speed threshold parsed");
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

void testPresentationModes() {
    std::cout << "Presentation modes\n";
    using engine::PresentationMode;
    using engine::Swapchain;

    const std::vector<VkPresentModeKHR> allModes = {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
                                                    VK_PRESENT_MODE_IMMEDIATE_KHR,
                                                    VK_PRESENT_MODE_FIFO_RELAXED_KHR};
    check(Swapchain::selectPresentMode(PresentationMode::Auto, true, allModes) == VK_PRESENT_MODE_FIFO_KHR,
          "auto + vsync selects FIFO");
    check(Swapchain::selectPresentMode(PresentationMode::Auto, false, allModes) ==
              VK_PRESENT_MODE_IMMEDIATE_KHR,
          "auto without vsync selects immediate when available");
    check(Swapchain::selectPresentMode(PresentationMode::Mailbox, true, allModes) ==
              VK_PRESENT_MODE_MAILBOX_KHR,
          "explicit mailbox overrides vsync");
    check(Swapchain::selectPresentMode(PresentationMode::Immediate, false, allModes) ==
              VK_PRESENT_MODE_IMMEDIATE_KHR,
          "explicit immediate selects immediate");
    check(Swapchain::selectPresentMode(PresentationMode::FifoRelaxed, false, allModes) ==
              VK_PRESENT_MODE_FIFO_RELAXED_KHR,
          "explicit fifo_relaxed selects fifo_relaxed");

    const std::vector<VkPresentModeKHR> fifoOnly = {VK_PRESENT_MODE_FIFO_KHR};
    check(Swapchain::selectPresentMode(PresentationMode::Mailbox, false, fifoOnly) ==
              VK_PRESENT_MODE_FIFO_KHR,
          "mailbox falls back to guaranteed FIFO");
    check(Swapchain::selectPresentMode(PresentationMode::Immediate, false, fifoOnly) ==
              VK_PRESENT_MODE_FIFO_KHR,
          "immediate falls back to guaranteed FIFO");
    check(engine::Renderer::framesInFlightForPresentMode(VK_PRESENT_MODE_MAILBOX_KHR) == 1,
          "mailbox uses one frame in flight for low latency");
    check(engine::Renderer::framesInFlightForPresentMode(VK_PRESENT_MODE_IMMEDIATE_KHR) == 1,
          "immediate uses one frame in flight for low latency");
    check(engine::Renderer::framesInFlightForPresentMode(VK_PRESENT_MODE_FIFO_KHR) == 2,
          "FIFO retains two frames in flight");
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

bool nearlyVec4(const glm::vec4& value, const glm::vec4& expected, float epsilon = 1e-5f) {
    return std::abs(value.x - expected.x) <= epsilon && std::abs(value.y - expected.y) <= epsilon &&
           std::abs(value.z - expected.z) <= epsilon && std::abs(value.w - expected.w) <= epsilon;
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

void testPhysics() {
    std::cout << "Physics\n";
    using engine::ColliderComponent;
    using engine::PhysicsSettings;
    using engine::PhysicsSystem;
    using engine::RigidBodyComponent;
    using engine::TransformComponent;

    check(sizeof(RigidBodyComponent) == 208, "rigid body remains a dense 208-byte component");
    check(sizeof(ColliderComponent) == 160, "collider remains a dense 160-byte component");
    {
        RigidBodyComponent inertiaBody(2.0f);
        inertiaBody.setPrincipalInertia(glm::vec3{2.0f, 4.0f, 6.0f});
        check(nearlyEqual(inertiaBody.inertiaTensor[0][0], 2.0) &&
                  nearlyEqual(inertiaBody.inverseInertiaTensor[1][1], 0.25) &&
                  nearlyEqual(inertiaBody.inverseInertiaTensor[2][2], 1.0 / 6.0, 1e-6),
              "principal inertia tensor stores and inverts all axes");
        check(ColliderComponent::obb(glm::vec3{0.5f}).type == ColliderComponent::Type::Obb,
              "OBB collider factory selects the oriented-box type");
        const std::vector<glm::vec3> hullPoints = {{-0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}};
        check(ColliderComponent::convexHull(hullPoints).hullPointCount == 2,
              "convex hull collider stores supplied points");
    }

    // Component API and layout are part of the DOD contract.
    RigidBodyComponent body(2.0f);
    check(nearlyEqual(body.inverseMass, 0.5), "rigid body inverse mass follows mass");
    body.addForce(glm::vec3{4.0f, 0.0f, 0.0f});
    check(nearlyVec3(body.force, glm::vec3{4.0f, 0.0f, 0.0f}), "rigid body accumulates forces");

    // Semi-implicit Euler gravity integration: v += g*dt, then x += v*dt.
    {
        engine::Scene scene;
        auto entity = scene.createEntity("Falling");
        entity.addComponent<TransformComponent>(glm::vec3{0.0f, 10.0f, 0.0f});
        auto& fallingBody = entity.addComponent<RigidBodyComponent>(1.0f);
        fallingBody.angularVelocity = glm::vec3{0.0f, 90.0f, 0.0f};
        entity.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f, -10.0f, 0.0f};
        PhysicsSystem physics;
        const auto stats = physics.update(scene, 0.1, settings);
        const auto& transform = entity.getComponent<TransformComponent>();
        const auto& rigidBody = entity.getComponent<RigidBodyComponent>();
        check(stats.bodies == 1 && stats.dynamicBodies == 1, "physics snapshots one dynamic body");
        check(nearlyEqual(rigidBody.velocity.y, -1.0), "gravity integrates velocity by delta time");
        // Four internal sub-steps integrate gravity semi-implicitly at
        // dt/4, so the position is the sub-stepped sum (9.9375), not the
        // single-step 9.9 — sub-stepping is the documented default.
        check(nearlyEqual(transform.position.y, 9.9375, 1e-4),
              "velocity integrates position by delta time (sub-stepped)");
        check(nearlyEqual(rigidBody.acceleration.y, -10.0), "rigid body records integrated acceleration");
        check(nearlyEqual(entity.getComponent<TransformComponent>().rotationDegrees.y, 9.0, 1e-4),
              "angular velocity integrates rotation by delta time");
    }

    // Force / mass integration, and forces are consumed exactly once.
    {
        engine::Scene scene;
        auto entity = scene.createEntity("Forced");
        entity.addComponent<TransformComponent>(glm::vec3{0.0f});
        auto& rigidBody = entity.addComponent<RigidBodyComponent>(2.0f);
        rigidBody.addForce(glm::vec3{10.0f, 0.0f, 0.0f});
        entity.addComponent<ColliderComponent>(ColliderComponent::sphere(0.5f));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        physics.update(scene, 0.1, settings);
        check(nearlyEqual(rigidBody.velocity.x, 0.5), "force divided by mass integrates velocity");
        check(nearlyVec3(rigidBody.force, glm::vec3{0.0f}), "physics clears consumed forces");
    }

    // Box against a static plane: a 4 m/s impact with restitution 0.5
    // rebounds at 2 m/s and is positionally corrected out of the floor.
    {
        engine::Scene scene;
        auto floor = scene.createEntity("Floor");
        floor.addComponent<TransformComponent>(glm::vec3{0.0f, -0.05f, 0.0f});
        auto& floorBody = floor.addComponent<RigidBodyComponent>();
        floorBody.setStatic(true);
        floorBody.restitution = 0.5f;
        floor.addComponent<ColliderComponent>(ColliderComponent::plane(glm::vec3{0.0f, 1.0f, 0.0f}, 0.0f));

        auto cube = scene.createEntity("BouncingCube");
        cube.addComponent<TransformComponent>(glm::vec3{0.0f, 0.45f, 0.0f});
        auto& cubeBody = cube.addComponent<RigidBodyComponent>(1.0f);
        cubeBody.velocity = glm::vec3{0.0f, -4.0f, 0.0f};
        cubeBody.restitution = 0.5f;
        cube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));

        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        const auto stats = physics.update(scene, 1.0 / 60.0, settings);
        check(stats.contacts == 4, "box-plane narrow phase generates a four-point manifold");
        check(nearlyEqual(cubeBody.velocity.y, 2.0, 1e-4), "floor bounce applies restitution");
        check(cube.getComponent<TransformComponent>().position.y > 0.43f,
              "floor contact corrects penetration");
    }

    // Perfectly elastic equal-mass sphere collision swaps velocities and
    // therefore conserves both momentum and kinetic energy.
    {
        engine::Scene scene;
        auto left = scene.createEntity("LeftSphere");
        left.addComponent<TransformComponent>(glm::vec3{-0.49f, 0.0f, 0.0f});
        auto& leftBody = left.addComponent<RigidBodyComponent>(2.0f);
        leftBody.velocity = glm::vec3{2.0f, 0.0f, 0.0f};
        leftBody.restitution = 1.0f;
        leftBody.friction = 0.0f;
        left.addComponent<ColliderComponent>(ColliderComponent::sphere(0.5f));

        auto right = scene.createEntity("RightSphere");
        right.addComponent<TransformComponent>(glm::vec3{0.49f, 0.0f, 0.0f});
        auto& rightBody = right.addComponent<RigidBodyComponent>(2.0f);
        rightBody.velocity = glm::vec3{-1.0f, 0.0f, 0.0f};
        rightBody.restitution = 1.0f;
        rightBody.friction = 0.0f;
        right.addComponent<ColliderComponent>(ColliderComponent::sphere(0.5f));

        const float momentumBefore = 2.0f * leftBody.velocity.x + 2.0f * rightBody.velocity.x;
        const float energyBefore = leftBody.velocity.x * leftBody.velocity.x +
                                   rightBody.velocity.x * rightBody.velocity.x;
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        settings.restitutionVelocityThreshold = 0.0f;
        PhysicsSystem physics;
        physics.update(scene, 0.01, settings);
        const float momentumAfter = 2.0f * leftBody.velocity.x + 2.0f * rightBody.velocity.x;
        const float energyAfter = leftBody.velocity.x * leftBody.velocity.x +
                                  rightBody.velocity.x * rightBody.velocity.x;
        check(nearlyEqual(leftBody.velocity.x, -1.0, 1e-4) && nearlyEqual(rightBody.velocity.x, 2.0, 1e-4),
              "elastic collision swaps equal-mass velocities");
        check(nearlyEqual(momentumAfter, momentumBefore, 1e-4), "elastic collision conserves momentum");
        check(nearlyEqual(energyAfter, energyBefore, 1e-4), "elastic collision conserves kinetic energy factor");
    }

    // Inelastic box collision reaches the momentum-conserving common velocity.
    {
        engine::Scene scene;
        auto left = scene.createEntity("LeftBox");
        left.addComponent<TransformComponent>(glm::vec3{-0.49f, 0.0f, 0.0f});
        auto& leftBody = left.addComponent<RigidBodyComponent>(1.0f);
        leftBody.velocity = glm::vec3{3.0f, 0.0f, 0.0f};
        leftBody.restitution = 0.0f;
        leftBody.friction = 0.0f;
        left.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        auto right = scene.createEntity("RightBox");
        right.addComponent<TransformComponent>(glm::vec3{0.49f, 0.0f, 0.0f});
        auto& rightBody = right.addComponent<RigidBodyComponent>(1.0f);
        rightBody.velocity = glm::vec3{-1.0f, 0.0f, 0.0f};
        rightBody.restitution = 0.0f;
        rightBody.friction = 0.0f;
        right.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        physics.update(scene, 0.01, settings);
        check(nearlyEqual(leftBody.velocity.x, 1.0, 1e-4) && nearlyEqual(rightBody.velocity.x, 1.0, 1e-4),
              "inelastic box collision reaches common momentum velocity");
    }

    // Stacking: two unit boxes on a plane settle at their resting heights
    // with small residual velocity (no sinking, explosion, or jitter).
    {
        engine::Scene scene;
        auto floor = scene.createEntity("StackFloor");
        floor.addComponent<TransformComponent>(glm::vec3{0.0f, -0.05f, 0.0f});
        auto& floorBody = floor.addComponent<RigidBodyComponent>();
        floorBody.setStatic(true);
        floorBody.restitution = 0.0f;
        floorBody.friction = 0.6f;
        floor.addComponent<ColliderComponent>(ColliderComponent::plane(glm::vec3{0.0f, 1.0f, 0.0f}, 0.0f));

        auto lower = scene.createEntity("LowerBox");
        lower.addComponent<TransformComponent>(glm::vec3{0.0f, 0.5f, 0.0f});
        auto& lowerBody = lower.addComponent<RigidBodyComponent>(1.0f);
        lowerBody.restitution = 0.0f;
        lowerBody.friction = 0.6f;
        lower.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        auto upper = scene.createEntity("UpperBox");
        upper.addComponent<TransformComponent>(glm::vec3{0.0f, 1.5f, 0.0f});
        auto& upperBody = upper.addComponent<RigidBodyComponent>(1.0f);
        upperBody.restitution = 0.0f;
        upperBody.friction = 0.6f;
        upper.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));

        PhysicsSettings settings;
        PhysicsSystem physics;
        for (int step = 0; step < 180; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        const float lowerY = lower.getComponent<TransformComponent>().position.y;
        const float upperY = upper.getComponent<TransformComponent>().position.y;
        check(lowerY > 0.47f && lowerY < 0.53f, "stacked lower box rests on the floor");
        check(upperY > 1.45f && upperY < 1.55f, "stacked upper box rests on the lower box");
        check(glm::length(lowerBody.velocity) < 0.1f && glm::length(upperBody.velocity) < 0.1f,
              "stacked boxes settle without jitter");
    }

    // Broad-phase reporting and JobSystem-parallel integration.
    {
        engine::Scene scene;
        std::vector<engine::Entity> entities;
        for (int i = 0; i < 256; ++i) {
            auto entity = scene.createEntity("ParallelBody");
            entity.addComponent<TransformComponent>(glm::vec3{static_cast<float>(i) * 3.0f, 10.0f, 0.0f});
            entity.addComponent<RigidBodyComponent>(1.0f);
            entity.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
            entities.push_back(entity);
        }
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f, -10.0f, 0.0f};
        settings.parallelMinBodies = 32;
        engine::JobSystem jobs{engine::JobSystem::Config{3, 1024}};
        PhysicsSystem physics;
        const auto stats = physics.update(scene, 0.1, settings, jobs);
        bool integrated = stats.bodies == 256 && stats.candidatePairs == 0;
        for (auto& entity : entities) {
            integrated = integrated && nearlyEqual(entity.getComponent<RigidBodyComponent>().velocity.y, -1.0) &&
                         nearlyEqual(entity.getComponent<TransformComponent>().position.y, 9.9375, 1e-4);
        }
        check(integrated, "JobSystem-parallel physics integrates every body exactly once");
    }

    // Respawn/reset (the C key in Application): captured dynamic bodies
    // return to their staggered spawn transforms and initial velocities;
    // stale motion, rotation, forces, static bodies, and entity structure
    // are all left in a clean, reusable state.
    {
        engine::Scene scene;
        auto floor = scene.createEntity("RespawnFloor");
        floor.addComponent<TransformComponent>(glm::vec3{0.0f, -0.05f, 0.0f});
        auto& floorBody = floor.addComponent<RigidBodyComponent>();
        floorBody.setStatic(true);
        floor.addComponent<ColliderComponent>(ColliderComponent::plane(glm::vec3{0.0f, 1.0f, 0.0f}, 0.0f));

        auto cube = scene.createEntity("RespawnCube");
        auto& cubeTransform = cube.addComponent<TransformComponent>(glm::vec3{1.0f, 4.0f, -2.0f});
        cubeTransform.rotationDegrees = glm::vec3{0.0f, 15.0f, 0.0f};
        cubeTransform.scale = glm::vec3{0.5f};
        auto& cubeBody = cube.addComponent<RigidBodyComponent>(1.0f);
        cubeBody.velocity = glm::vec3{0.25f, 0.0f, 0.0f};
        cubeBody.angularVelocity = glm::vec3{0.0f, 30.0f, 0.0f};
        cube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        auto& cubeSpin = cube.addComponent<engine::SpinComponent>(glm::vec3{0.0f, 45.0f, 0.0f});

        PhysicsSystem physics;
        check(physics.captureSpawnStates(scene) == 1, "respawn capture counts dynamic bodies");
        check(physics.spawnStateCount() == 2, "respawn capture snapshots dynamic and static bodies");
        const size_t entityCountBefore = scene.entityCount();

        // Simulate a messy mid-game state, then reset twice. The second
        // reset proves the operation is idempotent and reusable.
        for (int resetIndex = 0; resetIndex < 2; ++resetIndex) {
            cubeTransform.position = glm::vec3{-8.0f, -4.0f, 9.0f};
            cubeTransform.rotationDegrees = glm::vec3{80.0f, 170.0f, 45.0f};
            cubeBody.velocity = glm::vec3{9.0f, -12.0f, 7.0f};
            cubeBody.angularVelocity = glm::vec3{99.0f, 88.0f, 77.0f};
            cubeBody.acceleration = glm::vec3{1.0f, 2.0f, 3.0f};
            cubeBody.addForce(glm::vec3{100.0f, 100.0f, 100.0f});
            cubeSpin.degreesPerSecond = glm::vec3{300.0f, 0.0f, 0.0f};

            check(physics.resetToSpawnStates(scene) == 1, "physics reset respawns one dynamic body");
            check(nearlyVec3(cubeTransform.position, glm::vec3{1.0f, 4.0f, -2.0f}),
                  "physics reset restores spawn position");
            check(nearlyVec3(cubeTransform.rotationDegrees, glm::vec3{0.0f, 15.0f, 0.0f}),
                  "physics reset restores spawn rotation");
            check(nearlyVec3(cubeBody.velocity, glm::vec3{0.25f, 0.0f, 0.0f}),
                  "physics reset restores initial linear velocity");
            check(nearlyVec3(cubeBody.angularVelocity, glm::vec3{0.0f, 30.0f, 0.0f}),
                  "physics reset restores initial angular velocity");
            check(nearlyVec3(cubeSpin.degreesPerSecond, glm::vec3{0.0f, 45.0f, 0.0f}),
                  "physics reset restores spin component");
            check(nearlyVec3(cubeBody.acceleration, glm::vec3{0.0f}) &&
                      nearlyVec3(cubeBody.force, glm::vec3{0.0f}),
                  "physics reset clears acceleration and forces");
        }
        check(scene.entityCount() == entityCountBefore && cube.isValid(),
              "physics reset preserves entity structure");

        // The respawned body is immediately simulation-ready.
        PhysicsSettings settings;
        const auto stats = physics.update(scene, 0.1, settings);
        check(stats.dynamicBodies == 1 && cubeBody.velocity.y < 0.0f &&
                  cubeTransform.position.y < 4.0f,
              "respawned body resumes falling under gravity");
    }

    // Quaternion rotational dynamics: with no torque, world angular
    // momentum is conserved while omega precesses through the changing
    // world inertia tensor of an asymmetric body.
    {
        engine::Scene scene;
        auto entity = scene.createEntity("Gyroscope");
        entity.addComponent<TransformComponent>(glm::vec3{0.0f});
        auto& rigidBody = entity.addComponent<RigidBodyComponent>(1.0f);
        rigidBody.setPrincipalInertia(glm::vec3{1.0f, 2.0f, 4.0f});
        rigidBody.angularVelocity = glm::vec3{90.0f, 0.0f, 90.0f};
        entity.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        for (int step = 0; step < 60; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        const float expectedMomentum = std::sqrt(1.5708f * 1.5708f + 6.2832f * 6.2832f);
        check(nearlyEqual(glm::length(rigidBody.angularMomentum), expectedMomentum, 0.01),
              "torque-free motion conserves angular momentum magnitude");
        check(nearlyEqual(glm::length(rigidBody.orientation), 1.0, 1e-5),
              "quaternion orientation remains normalized");
        check(glm::length(entity.getComponent<TransformComponent>().rotationDegrees) > 1.0f,
              "quaternion integration writes back Euler transform rotation");
    }

    // Radial gravity from a GravityFieldComponent points at the field
    // source; a finite radius localizes the same field to zero outside it.
    {
        engine::Scene scene;
        auto planet = scene.createEntity("Planet");
        planet.addComponent<TransformComponent>(glm::vec3{0.0f});
        auto& field = planet.addComponent<engine::GravityFieldComponent>();
        field.gravitationalParameter = 100.0f;
        auto satellite = scene.createEntity("Satellite");
        satellite.addComponent<TransformComponent>(glm::vec3{10.0f, 0.0f, 0.0f});
        auto& satelliteBody = satellite.addComponent<RigidBodyComponent>(1.0f);
        satellite.addComponent<ColliderComponent>(ColliderComponent::sphere(0.5f));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        physics.update(scene, 0.1, settings);
        // Sub-stepped integration re-evaluates the inverse-square field
        // as the satellite falls inward, so the value is slightly more
        // accurate than the single-step -0.1 (hence the wider band).
        check(nearlyEqual(satelliteBody.velocity.x, -0.1, 1e-3),
              "radial gravity field accelerates toward its source");
    }
    {
        engine::Scene scene;
        auto planet = scene.createEntity("LocalizedPlanet");
        planet.addComponent<TransformComponent>(glm::vec3{0.0f});
        auto& field = planet.addComponent<engine::GravityFieldComponent>();
        field.gravitationalParameter = 100.0f;
        field.radius = 5.0f;
        auto satellite = scene.createEntity("OutsideField");
        satellite.addComponent<TransformComponent>(glm::vec3{10.0f, 0.0f, 0.0f});
        auto& satelliteBody = satellite.addComponent<RigidBodyComponent>(1.0f);
        satellite.addComponent<ColliderComponent>(ColliderComponent::sphere(0.5f));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        physics.update(scene, 0.1, settings);
        check(nearlyEqual(satelliteBody.velocity.x, 0.0, 1e-6),
              "localized gravity field has no force outside its radius");
    }
    {
        engine::Scene scene;
        auto satellite = scene.createEntity("GlobalRadialSatellite");
        satellite.addComponent<TransformComponent>(glm::vec3{5.0f, 0.0f, 0.0f});
        auto& satelliteBody = satellite.addComponent<RigidBodyComponent>(1.0f);
        satellite.addComponent<ColliderComponent>(ColliderComponent::sphere(0.5f));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        settings.gravitationalParameter = 50.0f;
        settings.gravityCenter = glm::vec3{0.0f};
        PhysicsSystem physics;
        physics.update(scene, 0.1, settings);
        check(nearlyEqual(satelliteBody.velocity.x, -0.2, 1e-3),
              "global point-mass gravity uses inverse-square acceleration");
    }

    // Zero-G drift: no gravity means velocities and total momentum persist.
    {
        engine::Scene scene;
        auto left = scene.createEntity("ZeroGLeft");
        left.addComponent<TransformComponent>(glm::vec3{-10.0f, 0.0f, 0.0f});
        auto& leftBody = left.addComponent<RigidBodyComponent>(2.0f);
        leftBody.velocity = glm::vec3{1.0f, 2.0f, 3.0f};
        left.addComponent<ColliderComponent>(ColliderComponent::sphere(0.5f));
        auto right = scene.createEntity("ZeroGRight");
        right.addComponent<TransformComponent>(glm::vec3{10.0f, 0.0f, 0.0f});
        auto& rightBody = right.addComponent<RigidBodyComponent>(2.0f);
        rightBody.velocity = glm::vec3{-1.0f, -2.0f, -3.0f};
        right.addComponent<ColliderComponent>(ColliderComponent::sphere(0.5f));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        for (int step = 0; step < 10; ++step) {
            physics.update(scene, 0.1, settings);
        }
        const glm::vec3 momentum = leftBody.velocity * 2.0f + rightBody.velocity * 2.0f;
        check(nearlyVec3(momentum, glm::vec3{0.0f}), "zero-G motion conserves total momentum");
        check(left.getComponent<TransformComponent>().position.x > -10.0f,
              "zero-G bodies keep drifting without gravity");
    }

    // New narrow-phase shapes: rotated OBB, capsule/sphere, hull/box.
    {
        engine::Scene scene;
        auto a = scene.createEntity("ObbA");
        auto& aTransform = a.addComponent<TransformComponent>(glm::vec3{0.0f});
        aTransform.rotationDegrees = glm::vec3{0.0f, 0.0f, 45.0f};
        a.addComponent<RigidBodyComponent>(1.0f);
        a.addComponent<ColliderComponent>(ColliderComponent::obb(glm::vec3{0.5f}));
        auto b = scene.createEntity("ObbB");
        b.addComponent<TransformComponent>(glm::vec3{0.8f, 0.0f, 0.0f});
        b.addComponent<RigidBodyComponent>(1.0f);
        b.addComponent<ColliderComponent>(ColliderComponent::obb(glm::vec3{0.5f}));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        check(physics.update(scene, 0.01, settings).contacts == 2,
              "rotated OBB pair generates a two-point edge manifold");
    }
    {
        engine::Scene scene;
        auto capsule = scene.createEntity("Capsule");
        capsule.addComponent<TransformComponent>(glm::vec3{0.0f});
        capsule.addComponent<RigidBodyComponent>(1.0f);
        capsule.addComponent<ColliderComponent>(ColliderComponent::capsule(0.3f, 0.5f));
        auto sphere = scene.createEntity("CapsuleSphere");
        sphere.addComponent<TransformComponent>(glm::vec3{0.0f, 0.9f, 0.0f});
        sphere.addComponent<RigidBodyComponent>(1.0f);
        sphere.addComponent<ColliderComponent>(ColliderComponent::sphere(0.3f));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        check(physics.update(scene, 0.01, settings).contacts == 1, "capsule-sphere pair generates a contact");
    }
    {
        engine::Scene scene;
        const std::vector<glm::vec3> points = {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f},
                                               {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f},
                                               {-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f},
                                               {-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}};
        auto hull = scene.createEntity("Hull");
        hull.addComponent<TransformComponent>(glm::vec3{0.0f});
        hull.addComponent<RigidBodyComponent>(1.0f);
        hull.addComponent<ColliderComponent>(ColliderComponent::convexHull(points));
        auto box = scene.createEntity("HullBox");
        box.addComponent<TransformComponent>(glm::vec3{0.8f, 0.0f, 0.0f});
        box.addComponent<RigidBodyComponent>(1.0f);
        box.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        check(physics.update(scene, 0.01, settings).contacts == 1, "convex hull-box pair generates a contact");
    }

    // CCD: a 120 m/s projectile crosses a 0.1 m wall in one step. Swept
    // sphere/box collision stops it on the incoming side; disabling CCD
    // lets the same discrete step tunnel through.
    const auto runProjectile = [](bool ccdEnabled, float& outX, float& outVelocityX) {
        engine::Scene scene;
        auto wall = scene.createEntity("CcdWall");
        wall.addComponent<TransformComponent>(glm::vec3{0.0f});
        auto& wallBody = wall.addComponent<RigidBodyComponent>();
        wallBody.setStatic(true);
        wall.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.05f, 1.0f, 1.0f}));
        auto projectile = scene.createEntity("CcdProjectile");
        projectile.addComponent<TransformComponent>(glm::vec3{-3.0f, 0.0f, 0.0f});
        auto& projectileBody = projectile.addComponent<RigidBodyComponent>(1.0f);
        projectileBody.velocity = glm::vec3{120.0f, 0.0f, 0.0f};
        projectileBody.useGravity = false;
        projectile.addComponent<ColliderComponent>(ColliderComponent::sphere(0.1f));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        settings.ccdEnabled = ccdEnabled;
        // The control case also steps once per update: with sub-stepping
        // on, even CCD-less physics samples the wall crossing (that is
        // Phase 11 working as intended), so isolating CCD requires both.
        settings.subSteps = ccdEnabled ? 4u : 1u;
        PhysicsSystem physics;
        const auto stats = physics.update(scene, 1.0 / 30.0, settings);
        outX = projectile.getComponent<TransformComponent>().position.x;
        outVelocityX = projectileBody.velocity.x;
        return stats.contacts;
    };
    {
        float x = 0.0f;
        float velocityX = 0.0f;
        const size_t contacts = runProjectile(true, x, velocityX);
        check(contacts >= 1 && x < 0.0f && velocityX <= 0.1f,
              "CCD stops a fast projectile before a thin wall");
        const size_t tunneledContacts = runProjectile(false, x, velocityX);
        check(tunneledContacts == 0 && x > 0.0f, "disabled CCD permits the tunneling control case");
    }

    // Distance and hinge joints keep their anchor separation; a world-
    // anchored spring-damper pulls a stretched body back toward rest length.
    {
        engine::Scene scene;
        auto a = scene.createEntity("DistanceA");
        a.addComponent<TransformComponent>(glm::vec3{-1.0f, 0.0f, 0.0f});
        a.addComponent<RigidBodyComponent>(1.0f);
        a.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        auto b = scene.createEntity("DistanceB");
        b.addComponent<TransformComponent>(glm::vec3{1.0f, 0.0f, 0.0f});
        b.addComponent<RigidBodyComponent>(1.0f);
        b.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        auto& joint = a.addComponent<engine::JointComponent>();
        joint.other = b.handle();
        joint.type = engine::JointType::Distance;
        joint.restLength = 1.0f;
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        const auto stats = physics.update(scene, 1.0 / 60.0, settings);
        const float distance = glm::length(b.getComponent<TransformComponent>().position -
                                           a.getComponent<TransformComponent>().position);
        check(stats.joints == 1 && distance < 1.9f, "distance joint pulls bodies toward rest length");
    }
    {
        engine::Scene scene;
        auto pendulum = scene.createEntity("HingePendulum");
        pendulum.addComponent<TransformComponent>(glm::vec3{1.0f, 0.0f, 0.0f});
        auto& pendulumBody = pendulum.addComponent<RigidBodyComponent>(1.0f);
        pendulumBody.velocity = glm::vec3{0.0f, 1.0f, 0.0f};
        pendulum.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        auto& joint = pendulum.addComponent<engine::JointComponent>();
        joint.type = engine::JointType::Hinge;
        joint.anchorB = glm::vec3{0.0f};
        joint.restLength = 1.0f;
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        for (int step = 0; step < 60; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        const float radius = glm::length(pendulum.getComponent<TransformComponent>().position);
        check(radius > 0.8f && radius < 1.2f, "hinge joint preserves its swing radius");
    }
    {
        engine::Scene scene;
        auto bodyEntity = scene.createEntity("SpringBody");
        bodyEntity.addComponent<TransformComponent>(glm::vec3{2.0f, 0.0f, 0.0f});
        auto& springBody = bodyEntity.addComponent<RigidBodyComponent>(1.0f);
        bodyEntity.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        auto& joint = bodyEntity.addComponent<engine::JointComponent>();
        joint.type = engine::JointType::SpringDamper;
        joint.anchorB = glm::vec3{0.0f};
        joint.restLength = 1.0f;
        joint.stiffness = 50.0f;
        joint.damping = 1.0f;
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        physics.update(scene, 0.1, settings);
        check(springBody.velocity.x < -0.1f, "spring-damper applies a restoring force");
    }

    // An offset thruster accelerates the body and torques it around the
    // axis perpendicular to both offset and force.
    {
        engine::Scene scene;
        auto craft = scene.createEntity("ThrusterCraft");
        craft.addComponent<TransformComponent>(glm::vec3{0.0f});
        auto& craftBody = craft.addComponent<RigidBodyComponent>(2.0f);
        craft.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        auto& thruster = craft.addComponent<engine::ThrusterComponent>();
        thruster.force = glm::vec3{10.0f, 0.0f, 0.0f};
        thruster.localPosition = glm::vec3{0.0f, 1.0f, 0.0f};
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        physics.update(scene, 0.1, settings);
        // The thruster direction rotates with the craft as torque
        // builds, so the world-X component lands a hair under F/m * dt
        // while the total thrust magnitude is preserved.
        check(nearlyEqual(craftBody.velocity.x, 0.5, 2e-3) && glm::length(craftBody.velocity) < 0.51f,
              "thruster force accelerates by F/m");
        check(craftBody.angularVelocity.z < -1.0f, "offset thruster generates torque");
    }

    // Anisotropic friction can make one tangent direction slippery while
    // the orthogonal tangent retains the scalar Coulomb coefficient.
    {
        engine::Scene scene;
        auto floor = scene.createEntity("AnisotropicFloor");
        floor.addComponent<TransformComponent>(glm::vec3{0.0f, -0.05f, 0.0f});
        auto& floorBody = floor.addComponent<RigidBodyComponent>();
        floorBody.setStatic(true);
        floorBody.friction = 1.0f;
        floor.addComponent<ColliderComponent>(ColliderComponent::plane(glm::vec3{0.0f, 1.0f, 0.0f}, 0.0f));
        auto slider = scene.createEntity("AnisotropicSlider");
        slider.addComponent<TransformComponent>(glm::vec3{0.0f, 0.5f, 0.0f});
        auto& sliderBody = slider.addComponent<RigidBodyComponent>(1.0f);
        sliderBody.velocity = glm::vec3{2.0f, 0.0f, 0.0f};
        sliderBody.friction = 1.0f;
        sliderBody.anisotropicFriction = glm::vec2{0.0f, 1.0f};
        slider.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        PhysicsSettings settings;
        PhysicsSystem physics;
        physics.update(scene, 1.0 / 60.0, settings);
        check(sliderBody.velocity.x > 1.9f, "anisotropic friction leaves the slippery tangent direction moving");
    }

    // Islands split disconnected contact graphs, and parallel execution is
    // worker-count deterministic for separated bodies.
    {
        engine::Scene scene;
        for (int stackIndex = 0; stackIndex < 2; ++stackIndex) {
            const float x = stackIndex == 0 ? -5.0f : 5.0f;
            auto floor = scene.createEntity("IslandFloor");
            floor.addComponent<TransformComponent>(glm::vec3{x, -0.05f, 0.0f});
            auto& floorBody = floor.addComponent<RigidBodyComponent>();
            floorBody.setStatic(true);
            floor.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{2.0f, 0.05f, 2.0f}));
            auto box = scene.createEntity("IslandBox");
            box.addComponent<TransformComponent>(glm::vec3{x, 0.5f, 0.0f});
            box.addComponent<RigidBodyComponent>(1.0f);
            box.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        }
        PhysicsSettings settings;
        PhysicsSystem physics;
        const auto stats = physics.update(scene, 1.0 / 60.0, settings);
        check(stats.islands == 2 && stats.largestIsland == 2,
              "solver islands split disconnected contact graphs");
    }
    {
        const auto runWorkers = [](uint32_t workers) {
            engine::Scene scene;
            std::vector<engine::Entity> entities;
            for (int i = 0; i < 128; ++i) {
                auto entity = scene.createEntity("DeterministicBody");
                entity.addComponent<TransformComponent>(glm::vec3{static_cast<float>(i) * 3.0f, 10.0f, 0.0f});
                entity.addComponent<RigidBodyComponent>(1.0f);
                entity.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
                entities.push_back(entity);
            }
            PhysicsSettings settings;
            settings.gravity = glm::vec3{0.0f, -10.0f, 0.0f};
            settings.parallelMinBodies = 16;
            engine::JobSystem jobs{engine::JobSystem::Config{workers, 256}};
            PhysicsSystem physics;
            physics.update(scene, 0.05, settings, jobs);
            std::vector<glm::vec3> results;
            for (auto& entity : entities) {
                results.push_back(entity.getComponent<TransformComponent>().position);
                results.push_back(entity.getComponent<RigidBodyComponent>().velocity);
            }
            return results;
        };
        const std::vector<glm::vec3> oneWorker = runWorkers(1);
        const std::vector<glm::vec3> threeWorkers = runWorkers(3);
        bool identical = oneWorker.size() == threeWorkers.size();
        for (size_t i = 0; identical && i < oneWorker.size(); ++i) {
            identical = nearlyVec3(oneWorker[i], threeWorkers[i], 1e-6);
        }
        check(identical, "parallel physics is deterministic across worker counts");
    }
}

void testDynamicModulesAndMaterials() {
    std::cout << "Dynamic modules & materials\n";

    // This test executable links the shared engine modules directly; every
    // call below crosses a module boundary, so a missing DLL export fails
    // at link time rather than slipping through as a static-only build.
    check(engine::Window::cursorModeForCapture(true) == GLFW_CURSOR_DISABLED,
          "EnginePlatform exports window/input helpers");
    engine::LogLevel parsedLevel = engine::LogLevel::Info;
    check(engine::tryParseLogLevel("warning", parsedLevel) && parsedLevel == engine::LogLevel::Warning &&
              std::string_view(engine::toString(parsedLevel)) == "WARNING",
          "EngineFoundation exports Logger level helpers used by EngineCore");
    check(!engine::tryParseLogLevel("not-a-level", parsedLevel) && parsedLevel == engine::LogLevel::Warning,
          "exported Logger level parser rejects unknown levels without side effects");
    engine::Camera moduleCamera(engine::CameraConfig{});
    check(nearlyEqual(moduleCamera.movementSpeed(false), 3.0, 1e-6) &&
              nearlyEqual(moduleCamera.movementSpeed(true), 7.5, 1e-6),
          "EngineCore exports camera movement configuration");
    engine::PipelineStateCache modulePso;
    check(modulePso.intern(engine::PipelineStateDesc{}) == 1, "EngineGraphics exports the PSO cache");
    engine::RenderQueue moduleQueue;
    engine::DrawItem moduleItem;
    moduleItem.baseColor = glm::vec4{0.38f, 0.44f, 0.54f, 1.0f};
    moduleItem.specular = glm::vec4{1.0f, 0.7f, 0.4f, 56.0f};
    moduleItem.surface = glm::vec4{0.62f, 0.16f, 3.0f, 0.24f};
    moduleQueue.push(moduleItem);
    check(moduleQueue.size() == 1 && nearlyEqual(moduleQueue.items()[0].baseColor.x, 0.38f, 1e-6),
          "draw items carry the standardized material color across modules");
    check(nearlyVec4(moduleQueue.items()[0].specular, glm::vec4{1.0f, 0.7f, 0.4f, 56.0f}) &&
              nearlyVec4(moduleQueue.items()[0].surface, glm::vec4{0.62f, 0.16f, 3.0f, 0.24f}),
          "draw items carry specular and rim surface parameters across modules");
    check(sizeof(engine::DrawPushConstants) == 112 && offsetof(engine::DrawPushConstants, model) == 0 &&
              offsetof(engine::DrawPushConstants, baseColor) == 64 &&
              offsetof(engine::DrawPushConstants, specular) == 80 &&
              offsetof(engine::DrawPushConstants, surface) == 96,
          "push constants pack the 112-byte lighting payload inside the Vulkan minimum");

    // Solid-material pipeline contract: depth test + depth write on, Less
    // comparison, back-face culling. This is what prevents light/color
    // bleed and see-through geometry in the sandbox.
    const engine::PipelineStateDesc solidState;
    check(solidState.depthTestEnable && solidState.depthWriteEnable &&
              solidState.depthCompare == engine::DepthCompare::Less &&
              solidState.cullMode == engine::CullMode::Back && !solidState.blendEnable,
          "default pipeline state is a solid depth-writing back-culled material");

    // Uniform sandbox geometry: 24 vertices, one tint, exact unit normals.
    const std::vector<engine::Vertex> vertices = engine::Cube::vertices();
    const std::vector<uint32_t> indices = engine::Cube::indices();
    bool uniformTint = vertices.size() == 24 && indices.size() == 36;
    bool unitNormals = uniformTint;
    for (const engine::Vertex& vertex : vertices) {
        uniformTint = uniformTint && nearlyVec3(vertex.color, glm::vec3{1.0f}, 1e-6);
        unitNormals = unitNormals && nearlyEqual(glm::length(vertex.normal), 1.0, 1e-6) &&
                      nearlyEqual(glm::dot(vertex.normal, vertex.position), 0.5, 1e-6);
    }
    check(uniformTint, "cube geometry uses one uniform material tint on every face");
    check(unitNormals, "cube vertices carry exact outward face normals for diffuse shading");
    bool windingOutward = indices.size() == 36;
    for (size_t i = 0; i + 2 < indices.size() && windingOutward; i += 3) {
        const glm::vec3 p0 = vertices[indices[i]].position;
        const glm::vec3 p1 = vertices[indices[i + 1]].position;
        const glm::vec3 p2 = vertices[indices[i + 2]].position;
        const glm::vec3 centroid = (p0 + p1 + p2) / 3.0f;
        windingOutward = glm::dot(glm::cross(p1 - p0, p2 - p0), centroid) > 0.0f;
    }
    check(windingOutward, "cube triangles wind outward so back-face culling keeps visible faces");
    check(sizeof(engine::Vertex) == 44, "vertex format is position+normal+color+uv (44 bytes)");

    // Sandbox palette: every scene category gets its own solid material,
    // with distinct base colors and lighting responses.
    const engine::MaterialDesc floorMat = engine::makeSandboxFloorMaterial();
    const engine::MaterialDesc cubeMat = engine::makeSandboxCubeMaterial();
    const engine::MaterialDesc stackMat = engine::makeSandboxStackMaterial();
    const engine::MaterialDesc environmentMat = engine::makeSandboxEnvironmentMaterial();
    const engine::MaterialDesc playerMat = engine::makeSandboxPlayerMaterial();
    check(floorMat.name == "floor" && cubeMat.name == "cube" && stackMat.name == "stackCube" &&
              environmentMat.name == "environment" && playerMat.name == "player",
          "sandbox palette registers floor/cube/stack/environment/player materials");
    check(floorMat.baseColor != cubeMat.baseColor && cubeMat.baseColor != stackMat.baseColor &&
              stackMat.baseColor != environmentMat.baseColor &&
              environmentMat.baseColor != playerMat.baseColor && floorMat.baseColor != stackMat.baseColor &&
              floorMat.baseColor != playerMat.baseColor,
          "floor, falling cubes, stacking cubes, environment and player accent never share a color");
    check(floorMat.lighting.specularStrength < cubeMat.lighting.specularStrength &&
              floorMat.lighting.shininess < cubeMat.lighting.shininess,
          "dark floor material is matte while dynamic cubes carry a strong specular response");
    check(playerMat.lighting.rimStrength > environmentMat.lighting.rimStrength &&
              environmentMat.lighting.rimStrength > floorMat.lighting.rimStrength,
          "player/environment materials carry stronger rim light than the floor");
    check(cubeMat.baseColor.r > cubeMat.baseColor.b && floorMat.baseColor.b > floorMat.baseColor.r,
          "dynamic cubes read warm against the cool dark floor");

    engine::VramBudget paletteVram(1024 * 1024);
    engine::VramBudget paletteStaging(1024 * 1024);
    engine::AssetManager paletteAssets(paletteVram, paletteStaging);
    const engine::MaterialId floorMatId = paletteAssets.registerMaterial(floorMat);
    const engine::MaterialId cubeMatId = paletteAssets.registerMaterial(cubeMat);
    const engine::MaterialId stackMatId = paletteAssets.registerMaterial(stackMat);
    const engine::MaterialId environmentMatId = paletteAssets.registerMaterial(environmentMat);
    const engine::MaterialId playerMatId = paletteAssets.registerMaterial(playerMat);
    check(paletteAssets.materialCount() == 5 && floorMatId != cubeMatId && cubeMatId != stackMatId &&
              stackMatId != environmentMatId && environmentMatId != playerMatId,
          "palette materials register as five distinct material assets");
    check(nearlyVec4(paletteAssets.material(playerMatId)->desc.baseColor, playerMat.baseColor) &&
              nearlyEqual(paletteAssets.material(playerMatId)->desc.lighting.rimStrength,
                          playerMat.lighting.rimStrength, 1e-6) &&
              nearlyEqual(paletteAssets.material("floor")->desc.lighting.specularStrength,
                          floorMat.lighting.specularStrength, 1e-6),
          "registered palette materials preserve per-material lighting");

    // Material colors round-trip through EngineAssets unchanged.
    engine::VramBudget vram(1024 * 1024);
    engine::VramBudget staging(1024 * 1024);
    engine::AssetManager assets(vram, staging);
    engine::MaterialDesc sandbox;
    sandbox.name = "sandbox";
    sandbox.baseColor = glm::vec4{0.38f, 0.44f, 0.54f, 1.0f};
    const engine::MaterialId sandboxId = assets.registerMaterial(sandbox);
    const engine::MaterialAsset* sandboxAsset = assets.material(sandboxId);
    check(sandboxAsset != nullptr && nearlyEqual(sandboxAsset->desc.baseColor.x, 0.38f, 1e-6) &&
              nearlyEqual(sandboxAsset->desc.baseColor.z, 0.54f, 1e-6),
          "EngineAssets preserves the standardized sandbox material color");
}


void testShadowsDepthAndRotationSync() {
    std::cout << "Shadows, depth contract & rotation sync\n";

    // --- Directional shadow light frustum --------------------------------
    const glm::vec3 lightDir = engine::directionalLightDirection();
    const glm::vec3 center = engine::shadowSceneCenter();
    const glm::mat4 lightMatrix = engine::directionalLightViewProjection(
        lightDir, center, engine::kShadowSceneExtent, engine::kShadowLightDistance, engine::kShadowNearPlane,
        engine::kShadowFarPlane);
    const auto projectToLight = [&lightMatrix](const glm::vec3& point) {
        const glm::vec4 clip = lightMatrix * glm::vec4{point, 1.0f};
        return glm::vec3{clip} / clip.w;
    };
    const glm::vec3 centerNdc = projectToLight(center);
    check(nearlyEqual(centerNdc.x, 0.0f, 1e-4) && nearlyEqual(centerNdc.y, 0.0f, 1e-4) &&
              centerNdc.z > 0.0f && centerNdc.z < 1.0f,
          "shadow light frustum centers the scene in light space");
    const float depthTowardLight = projectToLight(center + lightDir * 2.0f).z;
    const float depthAwayFromLight = projectToLight(center - lightDir * 2.0f).z;
    check(depthTowardLight < depthAwayFromLight,
          "points closer to the light get smaller shadow depths (occluders win)");
    const glm::vec3 outsideNdc = projectToLight(center + glm::vec3{30.0f, 30.0f, 30.0f});
    check(std::abs(outsideNdc.x) > 1.0f || std::abs(outsideNdc.y) > 1.0f,
          "shadow frustum bounds are finite (outside points project out)");

    // --- Depth / winding contract -----------------------------------------
    // The pipeline declares VK_FRONT_FACE_CLOCKWISE with back-face
    // culling because the camera projection flips clip Y. Reproduce that
    // math on the CPU: every cube triangle that geometrically faces the
    // camera must have negative signed area in Vulkan NDC (clockwise),
    // and every away-facing triangle the opposite, so the rasterizer
    // keeps exactly the visible faces and the depth buffer (Less +
    // write) resolves the rest. If either side flipped, the sandbox
    // would render inside-out / see-through while rotating.
    {
        const std::vector<engine::Vertex> vertices = engine::Cube::vertices();
        const std::vector<uint32_t> indices = engine::Cube::indices();
        const glm::vec3 eye{3.0f, 2.0f, 4.0f};
        glm::mat4 projection = glm::perspective(glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f);
        projection[1][1] *= -1.0f; // GLM -> Vulkan Y flip (Camera::projectionMatrix)
        const glm::mat4 view = glm::lookAt(eye, glm::vec3{0.0f}, glm::vec3{0.0f, 1.0f, 0.0f});
        bool frontOk = true;
        bool backOk = true;
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            const glm::vec3 p0 = vertices[indices[i]].position;
            const glm::vec3 p1 = vertices[indices[i + 1]].position;
            const glm::vec3 p2 = vertices[indices[i + 2]].position;
            const glm::vec3 faceNormal = glm::cross(p1 - p0, p2 - p0);
            const auto ndc = [&projection, &view](const glm::vec3& point) {
                const glm::vec4 clip = projection * view * glm::vec4{point, 1.0f};
                return glm::vec2{clip.x, clip.y} / clip.w;
            };
            const glm::vec2 a = ndc(p0);
            const glm::vec2 b = ndc(p1);
            const glm::vec2 c = ndc(p2);
            const float signedArea = 0.5f * ((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y));
            if (glm::dot(faceNormal, eye - p0) > 0.0f) {
                frontOk = frontOk && signedArea < 0.0f; // clockwise in Vulkan NDC
            } else {
                backOk = backOk && signedArea > 0.0f;
            }
        }
        check(frontOk, "camera-facing triangles are clockwise under the Y-flip (kept by back-face culling)");
        check(backOk, "away-facing triangles are counter-clockwise under the Y-flip (culled, never see-through)");
    }

    // --- Quaternion -> render matrix 1:1 sync ------------------------------
    {
        engine::TransformComponent transform{glm::vec3{1.0f, 2.0f, 3.0f}};
        const glm::quat orientation = glm::normalize(
            glm::angleAxis(glm::radians(35.0f), glm::normalize(glm::vec3{1.0f, 2.0f, 3.0f})));
        transform.setOrientation(orientation);
        const glm::mat3 rendered = glm::mat3{transform.modelMatrix()};
        const glm::mat3 simulated = glm::mat3_cast(orientation);
        bool identical = true;
        for (int column = 0; column < 3; ++column) {
            identical = identical && nearlyVec3(rendered[column], simulated[column], 1e-5f);
        }
        check(identical, "transform model matrix matches the physics quaternion exactly");
        transform.setOrientation(-orientation); // quaternion double cover
        const glm::mat3 renderedNegated = glm::mat3{transform.modelMatrix()};
        bool sameAfterNegation = true;
        for (int column = 0; column < 3; ++column) {
            sameAfterNegation = sameAfterNegation && nearlyVec3(renderedNegated[column], simulated[column], 1e-5f);
        }
        check(sameAfterNegation, "negated quaternion (same rotation) renders the identical matrix");
        check(sizeof(engine::TransformComponent) == 64, "quaternion sync fits the 64-byte transform cache line");
    }

    // Tumbling body: after every fixed step, the rendered matrix must
    // equal the matrix of the body's simulated orientation — including
    // through orientations where Euler extraction is near gimbal lock.
    {
        engine::Scene scene;
        auto entity = scene.createEntity("Tumbler");
        entity.addComponent<engine::TransformComponent>(glm::vec3{0.0f, 10.0f, 0.0f});
        auto& body = entity.addComponent<engine::RigidBodyComponent>(1.0f);
        body.angularVelocity = glm::vec3{140.0f, 260.0f, 90.0f}; // fast tumble through gimbal regions
        entity.addComponent<engine::ColliderComponent>(engine::ColliderComponent::box(glm::vec3{0.5f}));
        engine::PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        engine::PhysicsSystem physics;
        bool inSync = true;
        for (int step = 0; step < 240; ++step) {
            physics.update(scene, 1.0 / 120.0, settings);
            const auto& transform = entity.getComponent<engine::TransformComponent>();
            const auto& rigidBody = entity.getComponent<engine::RigidBodyComponent>();
            const glm::mat3 rendered = glm::mat3{transform.modelMatrix()};
            const glm::mat3 simulated = glm::mat3_cast(glm::normalize(rigidBody.orientation));
            for (int column = 0; column < 3; ++column) {
                inSync = inSync && nearlyVec3(rendered[column], simulated[column], 1e-4f);
            }
        }
        check(inSync, "tumbling rigid body render matrix stays 1:1 with physics orientation for 240 steps");
    }
}


void testPhysicsPhase11() {
    std::cout << "Physics Phase 11 (sub-stepping, manifolds, sleep)\n";
    using engine::ColliderComponent;
    using engine::PhysicsSettings;
    using engine::PhysicsSystem;
    using engine::RigidBodyComponent;
    using engine::TransformComponent;

    // Sub-stepping is reported and configurable.
    {
        engine::Scene scene;
        auto body = scene.createEntity("Body");
        body.addComponent<TransformComponent>(glm::vec3{0.0f, 10.0f, 0.0f});
        body.addComponent<RigidBodyComponent>(1.0f);
        body.addComponent<ColliderComponent>(ColliderComponent::sphere(0.5f));
        PhysicsSettings settings;
        PhysicsSystem physics;
        check(physics.update(scene, 1.0 / 60.0, settings).subSteps == 4,
              "physics runs four internal sub-steps by default");
        settings.subSteps = 1;
        check(physics.update(scene, 1.0 / 60.0, settings).subSteps == 1,
              "physics sub-step count is configurable");
        settings.subSteps = 0;
        check(physics.update(scene, 1.0 / 60.0, settings).subSteps == 1,
              "physics sub-step count clamps to at least one");
    }

    // Anti-tunneling A/B: with CCD off, a single step lets a 60 m/s cube
    // jump clean over a thin floor slab; four sub-steps sample the fall
    // every 0.25 m and the cube is caught and lands on the slab.
    const auto fastFall = [](uint32_t subSteps) {
        engine::Scene scene;
        auto floor = scene.createEntity("ThinFloor");
        auto& floorTransform = floor.addComponent<TransformComponent>(glm::vec3{0.0f, -0.05f, 0.0f});
        floorTransform.scale = glm::vec3{9.0f, 0.1f, 9.0f};
        auto& floorBody = floor.addComponent<RigidBodyComponent>();
        floorBody.setStatic(true);
        floorBody.friction = 0.6f;
        floor.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        auto cube = scene.createEntity("FastCube");
        cube.addComponent<TransformComponent>(glm::vec3{0.0f, 1.6f, 0.0f});
        auto& cubeBody = cube.addComponent<RigidBodyComponent>(1.0f);
        cubeBody.velocity = glm::vec3{0.0f, -120.0f, 0.0f};
        cubeBody.restitution = 0.0f;
        cube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.25f}));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        settings.ccdEnabled = false; // isolate sub-stepping from swept CCD
        settings.subSteps = subSteps;
        PhysicsSystem physics;
        for (int step = 0; step < 3; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        return cube.getComponent<TransformComponent>().position.y;
    };
    check(fastFall(1) < -1.0f, "single-step physics tunnels through a thin floor at 120 m/s (CCD off)");
    check(fastFall(4) > 0.2f, "sub-stepped physics catches a 120 m/s cube on a thin floor (CCD off)");

    // Sleeping: a resting stack falls asleep (velocities exactly zero,
    // bodies counted in stats), then a dropped heavy ball wakes it.
    {
        engine::Scene scene;
        auto floor = scene.createEntity("SleepFloor");
        floor.addComponent<TransformComponent>(glm::vec3{0.0f, -0.05f, 0.0f});
        auto& floorBody = floor.addComponent<RigidBodyComponent>();
        floorBody.setStatic(true);
        floorBody.friction = 0.6f;
        floor.addComponent<ColliderComponent>(ColliderComponent::plane(glm::vec3{0.0f, 1.0f, 0.0f}, 0.0f));
        auto lower = scene.createEntity("SleepLower");
        lower.addComponent<TransformComponent>(glm::vec3{0.0f, 0.5f, 0.0f});
        auto& lowerBody = lower.addComponent<RigidBodyComponent>(1.0f);
        lowerBody.friction = 0.6f;
        lowerBody.restitution = 0.0f;
        lower.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        auto upper = scene.createEntity("SleepUpper");
        upper.addComponent<TransformComponent>(glm::vec3{0.0f, 1.5f, 0.0f});
        auto& upperBody = upper.addComponent<RigidBodyComponent>(1.0f);
        upperBody.friction = 0.6f;
        upperBody.restitution = 0.0f;
        upper.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));

        PhysicsSettings settings;
        PhysicsSystem physics;
        PhysicsSystem::Stats stats{};
        for (int step = 0; step < 240; ++step) {
            stats = physics.update(scene, 1.0 / 60.0, settings);
        }
        check(lowerBody.sleeping && upperBody.sleeping, "resting stack falls asleep");
        check(stats.sleepingBodies == 2, "sleeping bodies are counted in physics stats");
        check(glm::length(lowerBody.velocity) == 0.0f && glm::length(upperBody.velocity) == 0.0f,
              "sleeping bodies are exactly at rest (no micro-jitter)");

        auto ball = scene.createEntity("WakeBall");
        ball.addComponent<TransformComponent>(glm::vec3{0.0f, 3.2f, 0.0f});
        auto& ballBody = ball.addComponent<RigidBodyComponent>(5.0f);
        ballBody.restitution = 0.0f;
        ball.addComponent<ColliderComponent>(ColliderComponent::sphere(0.4f));
        bool wokeOnImpact = false;
        for (int step = 0; step < 120 && !wokeOnImpact; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
            wokeOnImpact = !upperBody.sleeping || glm::length(upperBody.velocity) > 0.05f;
        }
        check(wokeOnImpact, "a dropped heavy ball wakes the sleeping stack on impact");
    }

    // Regression (stacking stability): an offset stack built exactly
    // like the demo scene — 0.5 m cubes with restitution 0.38, each
    // level shifted 0.03 m — used to jitter, chain-launch and topple:
    // persistent contacts re-applied restitution against their
    // neighbours, warm starting handed one contact's impulse share to
    // every manifold point, and the shared manifold centroid gave a
    // leaning cube no pressure-shift righting moment. The stack must
    // now reach absolute rest (asleep, velocities exactly zero) well
    // inside two seconds and stay put.
    {
        engine::Scene scene;
        const float floorTop = -1.25f;
        auto floor = scene.createEntity("OffsetStackFloor");
        auto& floorTransform = floor.addComponent<TransformComponent>(glm::vec3{0.0f, floorTop - 0.05f, 0.0f});
        floorTransform.scale = glm::vec3{9.0f, 0.1f, 9.0f};
        auto& floorBody = floor.addComponent<RigidBodyComponent>();
        floorBody.setStatic(true);
        floorBody.restitution = 0.35f;
        floorBody.friction = 0.6f;
        floor.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));

        std::vector<engine::Entity> cubes;
        std::vector<glm::vec3> restPositions;
        for (int level = 0; level < 4; ++level) {
            for (int col = 0; col < 3; ++col) {
                auto cube = scene.createEntity("OffsetStackCube");
                const glm::vec3 position{-0.55f + col * 0.58f + level * 0.03f,
                                         floorTop + 0.26f + level * 0.53f, -3.6f};
                auto& transform = cube.addComponent<TransformComponent>(position);
                transform.scale = glm::vec3{0.5f};
                auto& body = cube.addComponent<RigidBodyComponent>(0.125f);
                body.restitution = 0.38f;
                body.friction = 0.5f;
                cube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
                cubes.push_back(cube);
                // Spawned 0.03 m per level above the slot below, but a
                // resting 0.5 m cube sits exactly 0.5 m above its
                // neighbour: the expected rest height, not the spawn
                // height, is the reference.
                restPositions.push_back(glm::vec3{position.x, floorTop + 0.25f + level * 0.5f, position.z});
            }
        }

        PhysicsSettings settings;
        PhysicsSystem physics;
        int asleepStep = -1;
        float peakSpeedAfterSettleWindow = 0.0f;
        for (int step = 0; step < 180; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
            bool allAsleep = true;
            for (auto& cube : cubes) {
                const auto& body = cube.getComponent<RigidBodyComponent>();
                allAsleep = allAsleep && body.sleeping;
                if (step >= 120) {
                    peakSpeedAfterSettleWindow =
                        std::max(peakSpeedAfterSettleWindow, glm::length(body.velocity));
                }
            }
            if (allAsleep && asleepStep < 0) {
                asleepStep = step;
            }
        }
        check(asleepStep >= 0 && asleepStep < 120,
              "offset stack falls asleep within two seconds");
        check(peakSpeedAfterSettleWindow == 0.0f,
              "settled stack has zero residual velocity (no micro-jitter)");
        bool stackIntact = true;
        for (size_t i = 0; i < cubes.size(); ++i) {
            const auto& transform = cubes[i].getComponent<TransformComponent>();
            const auto& body = cubes[i].getComponent<RigidBodyComponent>();
            stackIntact = stackIntact && body.sleeping &&
                          glm::length(body.velocity) == 0.0f &&
                          glm::length(body.angularVelocity) == 0.0f &&
                          std::abs(transform.position.y - restPositions[i].y) < 0.02f &&
                          std::abs(transform.position.x - restPositions[i].x) < 0.05f &&
                          std::abs(transform.position.z - restPositions[i].z) < 0.05f;
        }
        check(stackIntact, "offset stack stays stacked at absolute rest");
    }

    // Regression (player-vs-rigid-body feedback loop): the 80 kg
    // player capsule (640:1 against the 0.125 kg demo cubes) used to
    // trampoline off a single sleeping cube — Gauss-Seidel transfers
    // only ~invM_heavy/invM_light of its approach per sweep through
    // the floor-pinned cube, so it tunneled in, the accumulated
    // impulse grew chain-inflated, and warm-start redelivery fired
    // it back upward (+J/m every sub-step) once the approach was
    // consumed. With shock propagation the capsule must land dead
    // (combined restitution is zero), rest on top of the cube, and
    // leave the cube unsunk and asleep.
    {
        engine::Scene scene;
        const float floorTop = -1.25f;
        auto floor = scene.createEntity("PlayerDropFloor");
        auto& floorTransform = floor.addComponent<TransformComponent>(glm::vec3{0.0f, floorTop - 0.05f, 0.0f});
        floorTransform.scale = glm::vec3{60.0f, 0.1f, 60.0f};
        auto& floorBody = floor.addComponent<RigidBodyComponent>();
        floorBody.setStatic(true);
        floorBody.restitution = 0.35f;
        floorBody.friction = 0.6f;
        floor.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));

        auto cube = scene.createEntity("PlayerDropCube");
        auto& cubeTransform = cube.addComponent<TransformComponent>(glm::vec3{0.0f, floorTop + 0.25f, 0.0f});
        cubeTransform.scale = glm::vec3{0.5f};
        auto& cubeBody = cube.addComponent<RigidBodyComponent>(0.125f);
        cubeBody.restitution = 0.38f;
        cubeBody.friction = 0.5f;
        cube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));

        PhysicsSettings settings;
        PhysicsSystem physics;
        for (int step = 0; step < 120; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        check(cubeBody.sleeping, "drop-target cube settles asleep before the player lands");

        auto player = scene.createEntity("PlayerDropCapsule");
        auto& playerTransform = player.addComponent<TransformComponent>(
            glm::vec3{0.0f, floorTop + 0.5f + 0.75f + 1.5f, 0.0f});
        auto& playerBody = player.addComponent<RigidBodyComponent>(80.0f);
        playerBody.restitution = 0.0f;
        playerBody.friction = 0.25f;
        playerBody.setPrincipalInertia(glm::vec3{10000.0f});
        playerBody.allowSleep = false;
        player.addComponent<ColliderComponent>(ColliderComponent::capsule(0.30f, 0.45f));

        float peakUpwardVelocity = 0.0f;
        for (int step = 0; step < 240; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
            peakUpwardVelocity = std::max(peakUpwardVelocity, playerBody.velocity.y);
        }
        check(peakUpwardVelocity < 0.05f, "player capsule never rebounds off a resting cube (no trampoline)");
        check(std::abs(playerTransform.position.y - -0.003f) < 0.01f && glm::length(playerBody.velocity) < 0.01f,
              "player capsule comes to rest on top of the cube");
        check(std::abs(cubeTransform.position.y - -1.003f) < 0.005f && cubeBody.sleeping &&
                  glm::length(cubeBody.velocity) == 0.0f && glm::length(cubeBody.angularVelocity) == 0.0f,
              "cube stays unsunk and asleep under the landed player");
    }

    // Regression (player falls onto a sleeping stack): the same loop
    // used to eject stack cubes at ~2x the player's impact speed
    // with violent spin. The stack must absorb the landing without
    // a single cube measurably moving, and the player must end up
    // standing on top of the undisturbed, sleeping stack.
    {
        engine::Scene scene;
        const float floorTop = -1.25f;
        auto floor = scene.createEntity("StackDropFloor");
        auto& floorTransform = floor.addComponent<TransformComponent>(glm::vec3{0.0f, floorTop - 0.05f, 0.0f});
        floorTransform.scale = glm::vec3{60.0f, 0.1f, 60.0f};
        auto& floorBody = floor.addComponent<RigidBodyComponent>();
        floorBody.setStatic(true);
        floorBody.restitution = 0.35f;
        floorBody.friction = 0.6f;
        floor.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));

        std::vector<engine::Entity> cubes;
        std::vector<glm::vec3> restPositions;
        for (int level = 0; level < 4; ++level) {
            for (int col = 0; col < 3; ++col) {
                auto cube = scene.createEntity("StackDropCube");
                const glm::vec3 position{-0.55f + col * 0.58f + level * 0.03f,
                                         floorTop + 0.26f + level * 0.53f, 0.0f};
                auto& transform = cube.addComponent<TransformComponent>(position);
                transform.scale = glm::vec3{0.5f};
                auto& body = cube.addComponent<RigidBodyComponent>(0.125f);
                body.restitution = 0.38f;
                body.friction = 0.5f;
                cube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
                cubes.push_back(cube);
                restPositions.push_back(glm::vec3{position.x, floorTop + 0.25f + level * 0.5f, position.z});
            }
        }

        PhysicsSettings settings;
        PhysicsSystem physics;
        for (int step = 0; step < 180; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }

        auto player = scene.createEntity("StackDropCapsule");
        auto& playerTransform = player.addComponent<TransformComponent>(
            glm::vec3{0.03f, floorTop + 4 * 0.5f + 0.75f + 2.0f, 0.0f});
        auto& playerBody = player.addComponent<RigidBodyComponent>(80.0f);
        playerBody.restitution = 0.0f;
        playerBody.friction = 0.25f;
        playerBody.setPrincipalInertia(glm::vec3{10000.0f});
        playerBody.allowSleep = false;
        player.addComponent<ColliderComponent>(ColliderComponent::capsule(0.30f, 0.45f));

        float peakCubeSpeed = 0.0f;
        float peakCubeSpin = 0.0f;
        for (int step = 0; step < 300; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
            for (auto& cube : cubes) {
                const auto& body = cube.getComponent<RigidBodyComponent>();
                peakCubeSpeed = std::max(peakCubeSpeed, glm::length(body.velocity));
                peakCubeSpin = std::max(peakCubeSpin, glm::length(body.angularVelocity));
            }
        }
        check(peakCubeSpeed < 0.05f && peakCubeSpin < 5.0f,
              "stack cubes are never ejected or spun by the falling player");
        bool stackUndisturbed = true;
        for (size_t i = 0; i < cubes.size(); ++i) {
            const auto& transform = cubes[i].getComponent<TransformComponent>();
            const auto& body = cubes[i].getComponent<RigidBodyComponent>();
            stackUndisturbed = stackUndisturbed && body.sleeping &&
                               std::abs(transform.position.y - restPositions[i].y) < 0.01f &&
                               std::abs(transform.position.x - restPositions[i].x) < 0.05f;
        }
        check(stackUndisturbed, "slept-on stack stays asleep at its rest positions");
        check(std::abs(playerTransform.position.y - (floorTop + 4 * 0.5f + 0.75f)) < 0.02f &&
                  glm::length(playerBody.velocity) < 0.01f,
              "player ends up standing on the undisturbed stack");
    }

    // Regression (perpetual micro-bounce): an impact slower than the
    // restitution velocity threshold must never bounce, even with a
    // bouncy material — it lands, stays down, and falls asleep.
    {
        engine::Scene scene;
        auto floor = scene.createEntity("SoftLandingFloor");
        floor.addComponent<TransformComponent>(glm::vec3{0.0f, -0.05f, 0.0f});
        auto& floorBody = floor.addComponent<RigidBodyComponent>();
        floorBody.setStatic(true);
        floorBody.restitution = 0.9f;
        floorBody.friction = 0.5f;
        floor.addComponent<ColliderComponent>(ColliderComponent::plane(glm::vec3{0.0f, 1.0f, 0.0f}, 0.0f));
        auto cube = scene.createEntity("SoftLandingCube");
        cube.addComponent<TransformComponent>(glm::vec3{0.0f, 0.51f, 0.0f});
        auto& cubeBody = cube.addComponent<RigidBodyComponent>(1.0f);
        cubeBody.velocity = glm::vec3{0.0f, -0.5f, 0.0f}; // below the 1 m/s threshold
        cubeBody.restitution = 0.9f;
        cubeBody.friction = 0.5f;
        cube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));

        PhysicsSettings settings;
        PhysicsSystem physics;
        float maxRebound = 0.0f;
        bool sawContact = false;
        for (int step = 0; step < 120; ++step) {
            const auto stats = physics.update(scene, 1.0 / 60.0, settings);
            sawContact = sawContact || stats.contacts > 0;
            if (sawContact) {
                maxRebound = std::max(maxRebound, cubeBody.velocity.y);
            }
        }
        check(sawContact && maxRebound <= 0.05f,
              "sub-threshold impact never bounces (restitution clamped to zero)");
        check(cubeBody.sleeping && glm::length(cubeBody.velocity) == 0.0f,
              "soft-landed cube falls asleep at exact rest");
    }

    // Mass-ratio weighting: an inelastic 1 kg vs 100 kg collision leaves
    // the heavy body almost unmoved while the light body adopts nearly
    // the heavy body's velocity (total momentum conserved).
    {
        engine::Scene scene;
        auto light = scene.createEntity("LightBox");
        light.addComponent<TransformComponent>(glm::vec3{-0.49f, 0.0f, 0.0f});
        auto& lightBody = light.addComponent<RigidBodyComponent>(1.0f);
        lightBody.velocity = glm::vec3{4.0f, 0.0f, 0.0f};
        lightBody.restitution = 0.0f;
        lightBody.friction = 0.0f;
        light.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        auto heavy = scene.createEntity("HeavyBox");
        heavy.addComponent<TransformComponent>(glm::vec3{0.49f, 0.0f, 0.0f});
        auto& heavyBody = heavy.addComponent<RigidBodyComponent>(100.0f);
        heavyBody.restitution = 0.0f;
        heavyBody.friction = 0.0f;
        heavy.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        physics.update(scene, 0.01, settings);
        const float commonVelocity = 4.0f / 101.0f; // momentum / total mass
        check(nearlyEqual(heavyBody.velocity.x, commonVelocity, 2e-3) &&
                  nearlyEqual(lightBody.velocity.x, commonVelocity, 2e-3),
              "light-vs-heavy collision conserves momentum with mass-ratio weighting");
        check(std::abs(heavyBody.velocity.x) < std::abs(lightBody.velocity.x - 4.0f) / 50.0f,
              "heavy body barely responds to a light impact");
    }

    // Non-linear position correction: a deeply embedded box (0.2 m into
    // the floor) is projected out over a few passes without velocity
    // spikes or overshoot above its resting height.
    {
        engine::Scene scene;
        auto floor = scene.createEntity("ClampFloor");
        floor.addComponent<TransformComponent>(glm::vec3{0.0f, -0.05f, 0.0f});
        auto& floorBody = floor.addComponent<RigidBodyComponent>();
        floorBody.setStatic(true);
        floorBody.friction = 0.6f;
        floor.addComponent<ColliderComponent>(ColliderComponent::plane(glm::vec3{0.0f, 1.0f, 0.0f}, 0.0f));
        auto cube = scene.createEntity("EmbeddedCube");
        cube.addComponent<TransformComponent>(glm::vec3{0.0f, 0.3f, 0.0f}); // 0.2 m embedded
        auto& cubeBody = cube.addComponent<RigidBodyComponent>(1.0f);
        cubeBody.restitution = 0.0f;
        cube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        PhysicsSettings settings;
        PhysicsSystem physics;
        physics.update(scene, 1.0 / 60.0, settings);
        const float yAfterOne = cube.getComponent<TransformComponent>().position.y;
        check(yAfterOne > 0.32f && yAfterOne < 0.52f,
              "deep penetration is corrected firmly but clamped (no explosive pop-out)");
        for (int step = 0; step < 120; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        const float yRest = cube.getComponent<TransformComponent>().position.y;
        check(yRest > 0.47f && yRest < 0.53f && glm::length(cubeBody.velocity) < 0.15f,
              "embedded box settles at resting height without jitter");
    }

    // Player-jump regression: a capsule dropped from height lands on the
    // finite floor with the sub-stepped solver and stays grounded (no
    // bounce-out, no sink) across many steps.
    {
        engine::Scene scene;
        auto floor = scene.createEntity("JumpFloor");
        auto& floorTransform = floor.addComponent<TransformComponent>(glm::vec3{0.0f, -0.05f, 0.0f});
        floorTransform.scale = glm::vec3{9.0f, 0.1f, 9.0f};
        auto& floorBody = floor.addComponent<RigidBodyComponent>();
        floorBody.setStatic(true);
        floorBody.friction = 0.6f;
        floor.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        auto player = scene.createEntity("JumpPlayer");
        player.addComponent<TransformComponent>(glm::vec3{0.0f, 3.0f, 0.0f});
        auto& playerBody = player.addComponent<RigidBodyComponent>(80.0f);
        playerBody.restitution = 0.0f;
        playerBody.friction = 0.25f;
        playerBody.allowSleep = false;
        player.addComponent<ColliderComponent>(ColliderComponent::capsule(0.30f, 0.45f));
        PhysicsSettings settings;
        PhysicsSystem physics;
        float minY = 10.0f;
        for (int step = 0; step < 180; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
            minY = std::min(minY, player.getComponent<TransformComponent>().position.y);
        }
        const float restY = player.getComponent<TransformComponent>().position.y;
        check(minY > 0.70f, "falling player capsule never sinks into the floor while landing");
        check(restY > 0.70f && restY < 0.80f && std::abs(playerBody.velocity.y) < 0.2f,
              "player capsule rests grounded at capsule height after a jump landing");
    }
}

void testPlayerCollision() {
    std::cout << "Player collision (1:1 capsule hitbox)\n";
    using engine::ColliderComponent;
    using engine::PhysicsSettings;
    using engine::PhysicsSystem;
    using engine::RigidBodyComponent;
    using engine::TransformComponent;

    const auto addFloor = [](engine::Scene& scene) {
        auto floor = scene.createEntity("PlayerFloor");
        auto& floorTransform = floor.addComponent<TransformComponent>(glm::vec3{0.0f, -0.05f, 0.0f});
        floorTransform.scale = glm::vec3{9.0f, 0.1f, 9.0f};
        auto& floorBody = floor.addComponent<RigidBodyComponent>();
        floorBody.setStatic(true);
        floorBody.friction = 0.6f;
        floor.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        return floor;
    };
    const auto addPlayer = [](engine::Scene& scene, const glm::vec3& center) {
        auto player = scene.createEntity("Player");
        player.addComponent<TransformComponent>(center);
        auto& camera = player.addComponent<engine::CameraComponent>(engine::CameraConfig{});
        camera.camera.setOrientation(0.0f, 0.0f); // face +X
        auto& body = player.addComponent<RigidBodyComponent>(80.0f);
        body.restitution = 0.0f;
        body.friction = 0.25f;
        body.setPrincipalInertia(glm::vec3{10000.0f});
        player.addComponent<ColliderComponent>(
            ColliderComponent::capsule(engine::PlayerSystem::kCapsuleRadius,
                                       engine::PlayerSystem::kCapsuleHalfHeight));
        return player;
    };

    // Gravity landing: the capsule comes to rest with its bottom exactly on
    // the finite floor slab (center = radius + segment half-height = 0.75 m).
    {
        engine::Scene scene;
        addFloor(scene);
        auto player = addPlayer(scene, glm::vec3{0.0f, 3.0f, 0.0f});
        PhysicsSettings settings;
        PhysicsSystem physics;
        for (int step = 0; step < 180; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        const float centerY = player.getComponent<TransformComponent>().position.y;
        const float speedY = player.getComponent<RigidBodyComponent>().velocity.y;
        check(engine::PlayerSystem::hasPhysicalPlayer(scene), "physical player component set is detected");
        check(centerY > 0.68f && centerY < 0.82f, "player capsule lands and rests on the floor");
        check(std::abs(speedY) < 0.5f, "player landing velocity settles under gravity");
        check(engine::PlayerSystem::syncCamera(scene) &&
                  nearlyEqual(player.getComponent<engine::CameraComponent>().camera.position().y,
                              centerY + engine::PlayerSystem::kEyeHeight, 1e-4),
              "camera eye follows the simulated capsule center");
    }

    // Pushback: driving forward shoves a light cube ahead; the capsule
    // surface never crosses the cube's near face.
    {
        engine::Scene scene;
        addFloor(scene);
        auto player = addPlayer(scene, glm::vec3{-0.2f, 0.75f, 0.0f});
        auto cube = scene.createEntity("PushedCube");
        cube.addComponent<TransformComponent>(glm::vec3{0.8f, 0.5f, 0.0f});
        auto& cubeBody = cube.addComponent<RigidBodyComponent>(1.0f);
        cubeBody.friction = 0.5f;
        cubeBody.restitution = 0.0f;
        cube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));

        PhysicsSettings settings;
        PhysicsSystem physics;
        for (int step = 0; step < 90; ++step) {
            engine::PlayerSystem::driveMovement(scene, glm::vec3{0.0f, 0.0f, 1.0f}, 1.0 / 60.0);
            physics.update(scene, 1.0 / 60.0, settings);
        }
        const float playerX = player.getComponent<TransformComponent>().position.x;
        const float cubeX = cube.getComponent<TransformComponent>().position.x;
        check(cubeX > 0.9f, "player capsule pushes a dynamic cube forward");
        check(playerX < cubeX - 0.5f - engine::PlayerSystem::kCapsuleRadius + 0.08f,
              "player capsule never clips through the pushed cube");
    }

    // Sliding: diagonal motion into a static wall keeps tangential travel
    // while the normal component is stopped at the exact capsule offset.
    {
        engine::Scene scene;
        addFloor(scene);
        auto player = addPlayer(scene, glm::vec3{0.0f, 0.75f, 0.0f});
        auto wall = scene.createEntity("PlayerWall");
        wall.addComponent<TransformComponent>(glm::vec3{1.0f, 1.0f, 0.0f});
        auto& wallBody = wall.addComponent<RigidBodyComponent>();
        wallBody.setStatic(true);
        wall.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f, 1.0f, 3.0f}));

        PhysicsSettings settings;
        PhysicsSystem physics;
        auto& playerBody = player.getComponent<RigidBodyComponent>();
        for (int step = 0; step < 90; ++step) {
            playerBody.velocity.x = 2.0f;
            playerBody.velocity.z = 1.5f;
            physics.update(scene, 1.0 / 60.0, settings);
        }
        const glm::vec3 position = player.getComponent<TransformComponent>().position;
        check(position.x < 0.5f - engine::PlayerSystem::kCapsuleRadius + 0.06f,
              "player capsule stops at the wall face (no clipping)");
        check(position.z > 0.5f, "player slides along the wall tangentially");
    }
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
        return true;
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
        check(cube != nullptr && nearlyEqual(cube->bounds.extents.x, 0.5, 1e-4),
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
    matA.baseColor = glm::vec4{0.2f, 0.4f, 0.6f, 1.0f};
    matA.lighting.specularColor = glm::vec3{0.2f, 0.4f, 0.6f};
    matA.lighting.shininess = 0.4f;
    matA.lighting.specularStrength = 0.6f;
    matA.lighting.rimStrength = 0.8f;
    matA.lighting.rimPower = 1.0f;
    matA.lighting.ambientStrength = 0.25f;
    engine::MaterialDesc matB;
    matB.name = "matB";
    matB.baseColor = glm::vec4{0.8f, 0.6f, 0.4f, 1.0f};
    matB.lighting.specularColor = glm::vec3{0.8f, 0.6f, 0.4f};
    matB.lighting.shininess = 64.0f;
    matB.lighting.specularStrength = 0.5f;
    matB.lighting.rimStrength = 0.25f;
    matB.lighting.rimPower = 2.5f;
    matB.lighting.ambientStrength = 0.3f;
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
    bool lightingOk = frame.renderQueue.size() == 3;
    for (const engine::DrawItem& item : frame.renderQueue.items()) {
        const engine::MaterialAsset* material = assets.material(item.materialId);
        lightingOk = lightingOk && material != nullptr &&
                     nearlyVec4(item.baseColor, material->desc.baseColor) &&
                     nearlyVec3(item.specular, material->desc.lighting.specularColor) &&
                     nearlyEqual(item.specular.a, material->desc.lighting.shininess, 1e-6) &&
                     nearlyVec4(item.surface,
                                glm::vec4{material->desc.lighting.specularStrength,
                                          material->desc.lighting.rimStrength,
                                          material->desc.lighting.rimPower,
                                          material->desc.lighting.ambientStrength});
    }
    check(lightingOk, "render items carry packed material colors and lighting parameters");
}

void testImportedModelPipeline() {
    std::cout << "Phase 12: OBJ import, auto-scale normalization & 1:1 hitboxes\n";

    const auto dir = makeTempDir("engine_test_obj_import");
    const auto cratePath = dir / "millimetre_crate.obj";
    {
        std::ofstream file(cratePath);
        // A 2000 x 1000 x 500 crate exported in millimetres, origin at a
        // bottom corner's center plane — deliberately neither centred
        // nor metre-scale, like real DCC exports.
        file << "# millimetre crate\n"
             << "v -1000 0 -250\n"
             << "v 1000 0 -250\n"
             << "v 1000 1000 -250\n"
             << "v -1000 1000 -250\n"
             << "v -1000 0 250\n"
             << "v 1000 0 250\n"
             << "v 1000 1000 250\n"
             << "v -1000 1000 250\n"
             << "vt 0 0\n"
             << "vt 1 0\n"
             << "vt 1 1\n"
             << "vt 0 1\n"
             << "vn 0 0 1\n"
             << "vn 0 0 -1\n"
             << "vn 0 1 0\n"
             << "vn 0 -1 0\n"
             << "vn 1 0 0\n"
             << "vn -1 0 0\n"
             << "f 5/1/1 6/2/1 7/3/1 8/4/1\n"
             << "f 2/1/2 1/2/2 4/3/2 3/4/2\n"
             << "f 8/1/3 7/2/3 3/3/3 4/4/3\n"
             << "f 1/1/4 2/2/4 6/3/4 5/4/4\n"
             << "f 6/1/5 2/2/5 3/3/5 7/4/5\n"
             << "f 1/1/6 5/2/6 8/3/6 4/4/6\n";
    }
    const auto triPath = dir / "relative_triangle.obj";
    {
        std::ofstream file(triPath);
        // No vt/vn at all and negative (relative) face indices: the
        // importer must generate a flat normal and default UVs.
        file << "v 0 0 0\n"
             << "v 2 0 0\n"
             << "v 0 1 0\n"
             << "f -3 -2 -1\n";
    }
    const auto badPath = dir / "broken.obj";
    {
        std::ofstream file(badPath);
        file << "v 0 0 0\n"
             << "v 1 0 0\n"
             << "v 0 1 0\n"
             << "f 1 2 99\n"; // index 99 does not exist
    }

    engine::VramBudget vram(1024 * 1024);
    engine::VramBudget staging(1024 * 1024);
    engine::AssetManager assets(vram, staging);

    // --- OBJ parsing: buffers, triangulation, UVs/normals --------------
    const engine::MeshId crateId = assets.loadObj("millimetreCrate", cratePath);
    check(crateId != engine::kInvalidAssetId, "OBJ crate imports successfully");
    const engine::MeshAsset* crate = assets.mesh(crateId);
    check(crate != nullptr && crate->vertices.size() == 24 && crate->indices.size() == 36,
          "OBJ quads fan-triangulate into an indexed 24-vertex/12-triangle mesh");
    bool importedUvAndNormal = false;
    if (crate != nullptr) {
        for (const engine::Vertex& vertex : crate->vertices) {
            importedUvAndNormal = importedUvAndNormal ||
                                  (nearlyVec3(vertex.position, glm::vec3{1000.0f, 1000.0f, 250.0f}, 1e-4) &&
                                   nearlyEqual(vertex.texCoord.x, 1.0, 1e-6) &&
                                   nearlyEqual(vertex.texCoord.y, 1.0, 1e-6) &&
                                   nearlyEqual(glm::length(vertex.normal), 1.0, 1e-4));
        }
    }
    check(importedUvAndNormal, "OBJ vertices carry file positions, UVs and unit normals");
    check(assets.loadObj("millimetreCrate", cratePath) == crateId && assets.meshCount() == 1,
          "OBJ import is idempotent by name");

    const engine::MeshId triId = assets.loadObj("relativeTriangle", triPath);
    const engine::MeshAsset* tri = assets.mesh(triId);
    check(tri != nullptr && tri->vertices.size() == 3 && tri->indices.size() == 3,
          "OBJ negative relative indices resolve to the defined vertices");
    check(tri != nullptr && nearlyVec3(tri->vertices[0].normal, glm::vec3{0.0f, 0.0f, 1.0f}, 1e-5) &&
              nearlyEqual(tri->vertices[0].texCoord.x, 0.0, 1e-9) &&
              nearlyEqual(tri->vertices[0].texCoord.y, 0.0, 1e-9),
          "normal-less OBJ faces get a generated flat normal and default UVs");
    check(assets.loadObj("broken", badPath) == engine::kInvalidAssetId && assets.meshCount() == 2,
          "OBJ with an out-of-range face index is rejected without registering");
    check(assets.loadObj("missing", dir / "not_here.obj") == engine::kInvalidAssetId,
          "missing OBJ file yields an invalid id");

    // --- AABB + auto-scale normalization --------------------------------
    check(crate != nullptr && nearlyVec3(crate->bounds.min, glm::vec3{-1000.0f, 0.0f, -250.0f}, 1e-3) &&
              nearlyVec3(crate->bounds.max, glm::vec3{1000.0f, 1000.0f, 250.0f}, 1e-3),
          "mesh AABB is the min/max over all imported vertices");
    check(crate != nullptr && nearlyVec3(crate->bounds.center, glm::vec3{0.0f, 500.0f, 0.0f}, 1e-3) &&
              nearlyVec3(crate->bounds.extents, glm::vec3{1000.0f, 500.0f, 250.0f}, 1e-3) &&
              nearlyVec3(crate->bounds.size, glm::vec3{2000.0f, 1000.0f, 500.0f}, 1e-3),
          "mesh bounds derive center, half-extents and full size from the AABB");
    check(crate != nullptr && nearlyEqual(crate->bounds.autoScaleFactor, 1.0 / 2000.0, 1e-9),
          "auto-scale factor is 1/max full extent (millimetres normalize to a 1-unit longest side)");
    check(tri != nullptr && nearlyEqual(tri->bounds.autoScaleFactor, 0.5, 1e-6),
          "metre-scale meshes get the same normalization rule (no unit is special)");
    const engine::MeshBounds direct = engine::computeMeshBounds(crate != nullptr
                                                                    ? std::span<const engine::Vertex>{crate->vertices}
                                                                    : std::span<const engine::Vertex>{});
    check(nearlyVec3(direct.center, glm::vec3{0.0f, 500.0f, 0.0f}, 1e-3) &&
              nearlyEqual(direct.autoScaleFactor, 1.0 / 2000.0, 1e-9),
          "computeMeshBounds reproduces the registered bounds from raw vertices");

    // --- ECS spawn: transform, mesh, rigid body, 1:1 hitbox -------------
    engine::Scene scene;
    engine::Entity spawned = scene.spawnImportedModel(*crate, "ImportedCrate", glm::vec3{10.0f, 20.0f, 30.0f},
                                                      glm::vec3{2.0f, 3.0f, 4.0f}, 2.5f);
    check(spawned.isValid(), "spawnImportedModel returns a live entity");
    const auto& transform = spawned.getComponent<engine::TransformComponent>();
    check(nearlyVec3(transform.scale, glm::vec3{2.0f, 3.0f, 4.0f} * (1.0f / 2000.0f), 1e-7),
          "transform scale is userScale * autoScaleFactor");
    const auto& meshComponent = spawned.getComponent<engine::MeshComponent>();
    check(meshComponent.assetId == "millimetreCrate" &&
              nearlyVec3(meshComponent.localBounds.min, crate->bounds.min, 1e-3) &&
              nearlyVec3(meshComponent.localBounds.max, crate->bounds.max, 1e-3),
          "mesh component references the asset and its raw local bounds");
    check(spawned.hasComponent<engine::RigidBodyComponent>() &&
              nearlyEqual(spawned.getComponent<engine::RigidBodyComponent>().mass, 2.5, 1e-6),
          "spawned model carries a rigid body of the requested mass");
    const auto& collider = spawned.getComponent<engine::ColliderComponent>();
    check(collider.type == engine::ColliderComponent::Type::Box &&
              nearlyVec3(collider.halfExtents, crate->bounds.extents, 1e-3) &&
              nearlyVec3(collider.centerOffset, crate->bounds.center, 1e-3),
          "collider is a box with halfExtents = bounds.extents and centerOffset = bounds.center");
    // The rendered world AABB and the physics world box must coincide:
    // both are the raw bounds scaled by the same transform scale, the
    // collider additionally recentered by its (scaled) center offset.
    const engine::AABB worldVisual = meshComponent.localBounds.transformed(transform.modelMatrix());
    const glm::vec3 colliderWorldCenter = transform.position + transform.scale * collider.centerOffset;
    const glm::vec3 colliderWorldExtents = collider.halfExtents * transform.scale;
    check(nearlyVec3(worldVisual.center(), colliderWorldCenter, 1e-4) &&
              nearlyVec3(worldVisual.extents(), colliderWorldExtents, 1e-4),
          "rendered world bounds and collider world box are identical (1:1 hitbox)");

    engine::Entity scalarSpawned = scene.spawnImportedModel(*crate, "ScalarCrate", glm::vec3{0.0f}, 2.0f, 1.0f);
    check(nearlyVec3(scalarSpawned.getComponent<engine::TransformComponent>().scale,
                     glm::vec3{2.0f / 2000.0f}, 1e-7),
          "scalar userScale overload normalizes uniformly");
    const size_t entitiesBeforeInvalid = scene.entityCount();
    const engine::MeshAsset emptyAsset;
    check(!scene.spawnImportedModel(emptyAsset, "Empty", glm::vec3{0.0f}).isValid() &&
              scene.entityCount() == entitiesBeforeInvalid,
          "spawning an empty asset is rejected without creating an entity");

    // --- Physics: the offset hitbox lands exactly on the visual bottom --
    {
        engine::Scene physicsScene;
        auto floor = physicsScene.createEntity("ImportFloor");
        floor.addComponent<engine::TransformComponent>(glm::vec3{0.0f});
        auto& floorBody = floor.addComponent<engine::RigidBodyComponent>();
        floorBody.setStatic(true);
        floor.addComponent<engine::ColliderComponent>(
            engine::ColliderComponent::plane(glm::vec3{0.0f, 1.0f, 0.0f}, 0.0f));

        // userScale 2 => world crate is 2 x 1 x 0.5, its AABB center
        // riding 0.5 above the entity origin (raw center y=500 mm).
        engine::Entity landed = physicsScene.spawnImportedModel(*crate, "LandedCrate", glm::vec3{0.0f, 0.08f, 0.0f},
                                                                glm::vec3{2.0f}, 1.0f);
        engine::PhysicsSettings settings;
        engine::PhysicsSystem physics;
        engine::PhysicsSystem::Stats stats;
        for (int step = 0; step < 150; ++step) {
            stats = physics.update(physicsScene, 1.0 / 60.0, settings);
        }
        const auto& landedTransform = landed.getComponent<engine::TransformComponent>();
        const engine::AABB landedVisual =
            landed.getComponent<engine::MeshComponent>().localBounds.transformed(landedTransform.modelMatrix());
        check(stats.contacts > 0 && nearlyEqual(landedVisual.min.y, 0.0, 0.01),
              "imported model rests with its visual bottom exactly on the floor (offset hitbox)");
        check(glm::length(landed.getComponent<engine::RigidBodyComponent>().velocity) < 0.05f,
              "imported model settles after landing");
    }
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

    // Regression test for the resize FPS spike: many presented-frame
    // records inside one FPS window must not be converted into a bogus
    // instantaneous 1000+ FPS value before the window has elapsed.
    for (int i = 0; i < 1000; ++i) {
        time.recordPresentedFrame();
    }
    check(time.framesPerSecond() == 0.0, "FPS meter ignores sub-window frame bursts");
    std::this_thread::sleep_for(std::chrono::milliseconds(260));
    time.tick();
    time.recordPresentedFrame();
    check(time.framesPerSecond() > 0.0 && time.framesPerSecond() < 10000.0,
          "FPS meter reports a bounded windowed rate");
    time.resetFramesPerSecond();
    check(time.framesPerSecond() == 0.0, "FPS meter resets after a hitch or skipped frame");
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
    callbacks.render = [&renders](double) {
        ++renders;
        return true;
    };

    loop.run(callbacks);

    check(loop.stats().frames == 3, "loop runs exactly the requested frames");
    check(polls == 3 && updates == 3 && renders == 3, "poll/update/render fire once per frame");
    check(loop.stats().fixedSteps == static_cast<uint64_t>(fixedSteps),
          "fixed-step statistics match executed fixed updates");

    // Swapchain recreation during a resize returns false from render:
    // those iterations are real loop passes, but they presented nothing
    // and must not inflate frame/FPS statistics.
    engine::GameLoop skippedLoop(config);
    int skippedContinueChecks = 0;
    int skippedRenders = 0;
    engine::LoopCallbacks skippedCallbacks;
    skippedCallbacks.shouldContinue = [&skippedContinueChecks] { return ++skippedContinueChecks <= 3; };
    skippedCallbacks.render = [&skippedRenders](double) {
        ++skippedRenders;
        return false;
    };
    skippedLoop.run(skippedCallbacks);
    check(skippedRenders == 3 && skippedLoop.stats().frames == 0,
          "skipped presentations are not counted as frames");
    check(skippedLoop.time().framesPerSecond() == 0.0, "skipped presentations do not feed FPS");
}

} // namespace

int main() {
    testConfigLoading();
    testPresentationModes();
    testLogger();
    testCamera();
    testPhysics();
    testDynamicModulesAndMaterials();
    testShadowsDepthAndRotationSync();
    testPhysicsPhase11();
    testPlayerCollision();
    testSceneAndSystems();
    testPoolAllocator();
    testHudOverlay();
    testWindowLoopIntegration();
    testHardwareProfile();
    testVramBudget();
    testAssetManagement();
    testImportedModelPipeline();
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
