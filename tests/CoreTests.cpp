// Unit tests for the Phase 2 core infrastructure: configuration loading,
// logger filtering/rotation, fixed-timestep math, Time and GameLoop
// dispatch. These cover the parts of the engine that can be verified
// without a GPU or a window.

#include "Core/Camera.h"
#include "Core/EngineConfig.h"
#include "Core/EventBus.h"
#include "Physics/PhysicsEvents.h"
#include "Platform/Input.h"
#include "Platform/Window.h"
#include "Core/GameLoop.h"
#include "Core/Logger.h"
#include "Core/Time.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec4.hpp>

#include "Scene/DenseIteration.h"
#include "Scene/DynamicAABBTree.h"
#include "Scene/Scene.h"
#include "Scene/SceneBinarySerialization.h"
#include "Scene/PlaySession.h"
#include "Scene/ProjectExport.h"
#include "Scene/UIOutliner.h"
#include "Scene/UIAssetBrowser.h"
#include "Scene/Raycast.h"
#include "Scene/TransformGizmo.h"
#include "Scene/Systems/RenderSystem.h"
#include "Scene/Systems/PhysicsSystem.h"
#include "Scene/Systems/TransformSystem.h"
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
#include "Graphics/SdfFont.h"
#include "Graphics/UIContext.h"
#include "Graphics/UIInspector.h"
#include "Graphics/UIRenderer.h"
#include "Graphics/ShadowMap.h"
#include "Graphics/Renderer.h"
#include "Graphics/Swapchain.h"
#include "Core/Memory/PoolAllocator.h"
#include "Graphics/Frustum.h"
#include "Graphics/OcclusionCulling.h"
#include "Graphics/EnvironmentMap.h"
#include "Graphics/Pbr.h"
#include "Graphics/ToneMapping.h"
#include "Scene/Systems/LightingSystem.h"
#include "Core/ModuleManager.h"
#include "Scene/ComponentReflection.h"
#include "Scene/NativeScript.h"
#include "SpaceFlightModel.h"
#include "Platform/DynamicLibrary.h"

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
        joint.anchorA = glm::vec3{-1.0f, 0.0f, 0.0f}; // hinge point sits at the world origin
        joint.anchorB = glm::vec3{0.0f};
        joint.hingeAxis = glm::vec3{0.0f, 0.0f, 1.0f}; // swing in the XY plane
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
              offsetof(engine::DrawPushConstants, pbrParams) == 80 &&
              offsetof(engine::DrawPushConstants, textureIndices) == 96,
          "push constants pack the 112-byte PBR payload inside the Vulkan minimum");

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

void testPhysicsSettling() {
    std::cout << "Physics settling (friction lock, tumble death, static spawn)\n";
    using engine::ColliderComponent;
    using engine::PhysicsSettings;
    using engine::PhysicsSystem;
    using engine::RigidBodyComponent;
    using engine::TransformComponent;

    const float floorTop = -1.25f;
    const auto addFloor = [&floorTop](engine::Scene& scene, float extent) {
        auto floor = scene.createEntity("SettleFloor");
        auto& floorTransform = floor.addComponent<TransformComponent>(glm::vec3{0.0f, floorTop - 0.05f, 0.0f});
        floorTransform.scale = glm::vec3{extent, 0.1f, extent};
        auto& floorBody = floor.addComponent<RigidBodyComponent>();
        floorBody.setStatic(true);
        floorBody.restitution = 0.35f;
        floorBody.friction = 0.6f;
        floor.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        return floor;
    };
    const auto addCube = [](engine::Scene& scene, const glm::vec3& position, float scale, float mass) {
        auto cube = scene.createEntity("SettleCube");
        auto& transform = cube.addComponent<TransformComponent>(position);
        transform.scale = glm::vec3{scale};
        auto& body = cube.addComponent<RigidBodyComponent>(mass);
        body.restitution = 0.38f;
        body.friction = 0.5f;
        cube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        return cube;
    };
    const auto addPlayer = [](engine::Scene& scene, const glm::vec3& position) {
        auto player = scene.createEntity("SettlePlayer");
        player.addComponent<TransformComponent>(position);
        auto& body = player.addComponent<RigidBodyComponent>(80.0f);
        body.restitution = 0.0f;
        body.friction = 0.25f;
        body.setPrincipalInertia(glm::vec3{10000.0f});
        body.allowSleep = false;
        player.addComponent<ColliderComponent>(ColliderComponent::capsule(0.30f, 0.45f));
        return player;
    };
    const auto addStack = [&floorTop, &addCube](engine::Scene& scene, float z, float rise) {
        std::vector<engine::Entity> cubes;
        for (int level = 0; level < 4; ++level) {
            for (int col = 0; col < 3; ++col) {
                cubes.push_back(addCube(scene,
                                        glm::vec3{-0.55f + col * 0.58f + level * 0.03f,
                                                  floorTop + 0.26f + level * rise, z},
                                        0.5f, 0.125f));
            }
        }
        return cubes;
    };

    // Regression (perpetual tumbling): a dropped cube with spin used to
    // gain energy from the positional solver — every sub-step its
    // digging corner was lifted straight out of the floor, converting
    // solver work into spin — and tumbled forever at ~200 deg/s. With
    // rotation-aware positional correction it may bounce and tumble
    // briefly, then friction must kill the motion completely.
    {
        engine::Scene scene;
        addFloor(scene, 60.0f);
        auto cube = addCube(scene, glm::vec3{0.0f, 1.4f, 0.0f}, 0.5f, 0.125f);
        auto& cubeBody = cube.getComponent<RigidBodyComponent>();
        cubeBody.velocity = glm::vec3{0.15f, 0.0f, 0.0f};
        cubeBody.angularVelocity = glm::vec3{0.0f, 24.0f, 0.0f};
        PhysicsSettings settings;
        PhysicsSystem physics;
        float latePeakSpin = 0.0f;
        for (int step = 0; step < 360; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
            if (step >= 180) {
                latePeakSpin = std::max(latePeakSpin, glm::length(cubeBody.angularVelocity));
            }
        }
        const glm::vec3 position = cube.getComponent<TransformComponent>().position;
        check(cubeBody.sleeping && glm::length(cubeBody.velocity) == 0.0f &&
                  glm::length(cubeBody.angularVelocity) == 0.0f,
              "dropped spinning cube tumbles briefly then falls asleep (no perpetual tumble)");
        check(latePeakSpin < 45.0f, "tumbling cube's spin is dead in the second half of the drop test");
        check(std::abs(position.x) < 3.0f && std::abs(position.y - (floorTop + 0.25f)) < 0.01f,
              "tumbled cube comes to rest on the floor near its drop site");
    }

    // A kicked cube (slide + spin) must shed both and sleep instead of
    // rolling on forever.
    {
        engine::Scene scene;
        addFloor(scene, 60.0f);
        auto cube = addCube(scene, glm::vec3{0.0f, floorTop + 0.25f, 0.0f}, 0.5f, 0.125f);
        auto& cubeBody = cube.getComponent<RigidBodyComponent>();
        cubeBody.velocity = glm::vec3{1.2f, 0.0f, 0.0f};
        cubeBody.angularVelocity = glm::vec3{0.0f, 0.0f, -160.0f};
        PhysicsSettings settings;
        PhysicsSystem physics;
        for (int step = 0; step < 240; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        check(cubeBody.sleeping && glm::length(cubeBody.velocity) == 0.0f &&
                  glm::length(cubeBody.angularVelocity) == 0.0f,
              "kicked cube's slide and spin decay to sleep");
    }

    // Regression (standing friction lock): the player dropped onto a
    // sleeping stack, landing across the seam between two top cubes,
    // used to creep sideways ~0.3 mm/s forever — gravity re-seated
    // the capsule vertically each sub-step and the positional solver
    // lifted it back out along the tilted edge-contact normal, a
    // ratchet velocity friction cannot oppose. Standing must be
    // absolutely still: no drift, no residual velocity.
    {
        engine::Scene scene;
        addFloor(scene, 60.0f);
        auto cubes = addStack(scene, 0.0f, 0.53f);
        PhysicsSettings settings;
        PhysicsSystem physics;
        for (int step = 0; step < 180; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        auto player = addPlayer(scene, glm::vec3{-0.30f, floorTop + 2.0f + 0.75f + 0.35f, 0.0f});
        auto& playerTransform = player.getComponent<TransformComponent>();
        auto& playerBody = player.getComponent<RigidBodyComponent>();
        for (int step = 0; step < 120; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        const glm::vec3 settledPosition = playerTransform.position;
        for (int step = 0; step < 480; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        int stackAsleep = 0;
        for (auto& cube : cubes) {
            stackAsleep += cube.getComponent<RigidBodyComponent>().sleeping ? 1 : 0;
        }
        check(std::abs(playerTransform.position.x - settledPosition.x) < 0.001f &&
                  std::abs(playerTransform.position.z - settledPosition.z) < 0.001f,
              "player standing across a stack seam has zero lateral drift");
        check(glm::length(playerBody.velocity) < 0.001f,
              "standing player's residual velocity is zero (friction fully locks)");
        check(stackAsleep == 12, "stack slept on across the seam stays fully asleep");
    }

    // Regression (player-induced spin): pressing forward while hopping
    // against a cube may spin it while driven, but the cube must
    // settle dead the moment the player lets go — no spin loop.
    {
        engine::Scene scene;
        addFloor(scene, 60.0f);
        auto cube = addCube(scene, glm::vec3{0.8f, floorTop + 0.25f, 0.0f}, 0.5f, 0.125f);
        auto& cubeBody = cube.getComponent<RigidBodyComponent>();
        auto& cubeTransform = cube.getComponent<TransformComponent>();
        auto player = addPlayer(scene, glm::vec3{0.0f, floorTop + 0.75f, 0.0f});
        auto& playerBody = player.getComponent<RigidBodyComponent>();
        PhysicsSettings settings;
        PhysicsSystem physics;
        float peakSpinAfterRelease = 0.0f;
        float peakCubeSpeed = 0.0f;
        for (int step = 0; step < 720; ++step) {
            if (step < 480) {
                playerBody.velocity.x = 1.6f;
                playerBody.velocity.z = 0.0f;
                if (step % 60 == 30) {
                    playerBody.velocity.y = 4.5f; // hop while pressing
                }
            } else if (step < 500) {
                playerBody.velocity.x = -3.0f; // step away
                playerBody.velocity.z = 0.0f;
            } else {
                playerBody.velocity.x = 0.0f;
                playerBody.velocity.z = 0.0f;
            }
            physics.update(scene, 1.0 / 60.0, settings);
            peakCubeSpeed = std::max(peakCubeSpeed, glm::length(cubeBody.velocity));
            if (step >= 480) {
                peakSpinAfterRelease = std::max(peakSpinAfterRelease, glm::length(cubeBody.angularVelocity));
            }
        }
        check(peakSpinAfterRelease < 45.0f && cubeBody.sleeping &&
                  glm::length(cubeBody.velocity) == 0.0f && glm::length(cubeBody.angularVelocity) == 0.0f,
              "cube pressed and hopped against settles dead once the player lets go");
        check(peakCubeSpeed < 4.0f && std::abs(cubeTransform.position.y - (floorTop + 0.25f)) < 0.01f,
              "pressed cube is never launched or sunk");
    }

    // Regression (spawn overlap): bodies spawned overlapping with zero
    // velocity must separate positionally with no kinetic-energy
    // injection — no pop, no spin, straight to sleep.
    {
        engine::Scene scene;
        addFloor(scene, 60.0f);
        auto lower = addCube(scene, glm::vec3{0.0f, floorTop + 0.25f, 0.0f}, 0.5f, 0.125f);
        auto upper = addCube(scene, glm::vec3{0.0f, floorTop + 0.25f + 0.40f, 0.0f}, 0.5f, 0.125f);
        PhysicsSettings settings;
        PhysicsSystem physics;
        float peakSpeed = 0.0f;
        float peakSpin = 0.0f;
        for (int step = 0; step < 300; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
            peakSpeed = std::max({peakSpeed, glm::length(lower.getComponent<RigidBodyComponent>().velocity),
                                  glm::length(upper.getComponent<RigidBodyComponent>().velocity)});
            peakSpin = std::max({peakSpin, glm::length(lower.getComponent<RigidBodyComponent>().angularVelocity),
                                 glm::length(upper.getComponent<RigidBodyComponent>().angularVelocity)});
        }
        const float separation = upper.getComponent<TransformComponent>().position.y -
                                 lower.getComponent<TransformComponent>().position.y;
        check(peakSpeed < 0.01f && peakSpin < 1.0f,
              "overlapping spawn resolves with zero velocity injection");
        check(lower.getComponent<RigidBodyComponent>().sleeping &&
                  upper.getComponent<RigidBodyComponent>().sleeping && separation > 0.49f && separation < 0.51f,
              "overlapping spawn settles into a resting, sleeping pair");
    }

    // Regression (static spawn): a stack spawned exactly at rest
    // spacing with zero velocity must stay essentially motionless and
    // fall asleep immediately — no initial tumble or pop.
    {
        engine::Scene scene;
        addFloor(scene, 60.0f);
        std::vector<engine::Entity> cubes;
        std::vector<glm::vec3> spawnPositions;
        for (int level = 0; level < 4; ++level) {
            for (int col = 0; col < 3; ++col) {
                const glm::vec3 position{-0.5f + col * 0.5f, floorTop + 0.25f + level * 0.5f, 0.0f};
                cubes.push_back(addCube(scene, position, 0.5f, 0.125f));
                spawnPositions.push_back(position);
            }
        }
        PhysicsSettings settings;
        PhysicsSystem physics;
        float peakSpeed = 0.0f;
        int allAsleepStep = -1;
        for (int step = 0; step < 180; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
            bool allAsleep = true;
            for (auto& cube : cubes) {
                const auto& body = cube.getComponent<RigidBodyComponent>();
                peakSpeed = std::max(peakSpeed, glm::length(body.velocity));
                allAsleep = allAsleep && body.sleeping;
            }
            if (allAsleep && allAsleepStep < 0) {
                allAsleepStep = step;
            }
        }
        // Resting contacts sit a slop depth below the spawn height
        // (~3 mm per stacked level), so heights are compared against
        // the rest slot with the usual stack tolerance while lateral
        // positions must match the spawn almost exactly.
        bool unmoved = allAsleepStep >= 0;
        for (size_t i = 0; i < cubes.size(); ++i) {
            const glm::vec3& position = cubes[i].getComponent<TransformComponent>().position;
            unmoved = unmoved && std::abs(position.x - spawnPositions[i].x) < 0.005f &&
                      std::abs(position.y - spawnPositions[i].y) < 0.02f &&
                      std::abs(position.z - spawnPositions[i].z) < 0.005f;
        }
        check(peakSpeed < 0.06f, "exactly spawned stack never picks up speed");
        check(unmoved && allAsleepStep < 90, "exactly spawned stack is static and asleep immediately");
    }

    // Integration (demo scene replica): the demo's falling cubes,
    // stack and player spawn together. The stack must never tumble,
    // no cube may be ejected off the 9 m floor, and everything must
    // end asleep.
    {
        engine::Scene scene;
        addFloor(scene, 9.0f);
        struct Spawn {
            glm::vec3 position;
            float scale;
            glm::vec3 velocity;
            glm::vec3 spin;
        };
        const Spawn spawns[] = {
            {{0.0f, 1.4f, -2.4f}, 0.65f, {0.15f, 0, 0}, {0, 24, 0}},
            {{0.12f, 2.8f, -2.35f}, 0.65f, {0, 0, 0}, {0, -18, 0}},
            {{-1.25f, 1.9f, -2.2f}, 0.55f, {0.25f, 0, 0}, {0, 32, 0}},
            {{-1.18f, 3.1f, -2.15f}, 0.55f, {0, 0, 0}, {0, -26, 0}},
            {{1.3f, 1.6f, -2.1f}, 0.5f, {-0.2f, 0, 0}, {0, 20, 0}},
            {{1.22f, 2.75f, -2.05f}, 0.5f, {0, 0, 0}, {0, -30, 0}},
        };
        std::vector<engine::Entity> loose;
        for (const Spawn& spawn : spawns) {
            const float mass = spawn.scale * spawn.scale * spawn.scale;
            auto cube = addCube(scene, spawn.position, spawn.scale, mass);
            auto& body = cube.getComponent<RigidBodyComponent>();
            body.velocity = spawn.velocity;
            body.angularVelocity = spawn.spin;
            loose.push_back(cube);
        }
        auto stack = addStack(scene, -3.6f, 0.53f);
        addPlayer(scene, glm::vec3{2.6f, -0.5f, 1.5f});
        PhysicsSettings settings;
        PhysicsSystem physics;
        float peakStackSpin = 0.0f;
        for (int step = 0; step < 720; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
            if (step < 240) {
                for (auto& cube : stack) {
                    peakStackSpin =
                        std::max(peakStackSpin, glm::length(cube.getComponent<RigidBodyComponent>().angularVelocity));
                }
            }
        }
        int stackAsleep = 0;
        for (auto& cube : stack) {
            stackAsleep += cube.getComponent<RigidBodyComponent>().sleeping ? 1 : 0;
        }
        check(stackAsleep == 12 && peakStackSpin < 45.0f,
              "demo replica stack never tumbles and ends fully asleep");
        bool looseSettled = true;
        for (size_t i = 0; i < loose.size(); ++i) {
            const auto& body = loose[i].getComponent<RigidBodyComponent>();
            const glm::vec3& position = loose[i].getComponent<TransformComponent>().position;
            const float restY = floorTop + spawns[i].scale * 0.5f;
            // Flat on the floor, stacked on a sibling, or frozen at a
            // slow balance point all count as settled; ejected,
            // floating or still-moving do not.
            looseSettled = looseSettled && body.sleeping && glm::length(body.velocity) == 0.0f &&
                           std::abs(position.x) < 4.4f && std::abs(position.z) < 4.4f &&
                           position.y > restY - 0.03f && position.y < restY + 0.55f &&
                           position.y < spawns[i].position.y;
        }
        check(looseSettled, "demo replica falling cubes land on the floor and sleep (none ejected)");
    }
}

void testRaycasting() {
    std::cout << "Raycasting & mouse picking (Phase 13)\n";
    using engine::AABB;
    using engine::ColliderComponent;
    using engine::MeshComponent;
    using engine::Ray;
    using engine::RaycastHit;
    using engine::TransformComponent;

    const auto vecNear = [](const glm::vec3& a, const glm::vec3& b, float eps = 1e-4f) {
        return std::abs(a.x - b.x) < eps && std::abs(a.y - b.y) < eps && std::abs(a.z - b.z) < eps;
    };

    // --- Screen -> NDC (Vulkan y-down convention, same as the renderer) ----
    {
        check(vecNear(glm::vec3{engine::screenToNdc({400.0f, 300.0f}, {800.0f, 600.0f}), 0.0f},
                      glm::vec3{0.0f, 0.0f, 0.0f}),
              "screen centre maps to NDC origin");
        check(vecNear(glm::vec3{engine::screenToNdc({0.0f, 0.0f}, {800.0f, 600.0f}), 0.0f},
                      glm::vec3{-1.0f, -1.0f, 0.0f}),
              "screen top-left maps to NDC (-1,-1)");
        check(vecNear(glm::vec3{engine::screenToNdc({800.0f, 600.0f}, {800.0f, 600.0f}), 0.0f},
                      glm::vec3{1.0f, 1.0f, 0.0f}),
              "screen bottom-right maps to NDC (1,1)");
    }

    // --- Camera unprojection -------------------------------------------------
    {
        engine::Camera camera{engine::CameraConfig{}}; // at (0,0,3), looking down -Z
        const Ray center = engine::cameraRay(camera, glm::vec2{0.0f, 0.0f}, 1.0f);
        check(vecNear(center.origin, glm::vec3{0.0f, 0.0f, 3.0f}), "camera ray originates at the eye position");
        check(vecNear(center.direction, glm::vec3{0.0f, 0.0f, -1.0f}),
              "camera centre ray follows the camera front vector");
        const Ray top = engine::cameraRay(camera, glm::vec2{0.0f, -1.0f}, 1.0f);
        check(top.direction.y > 0.35f && top.direction.z < -0.85f && std::abs(top.direction.x) < 1e-4f,
              "NDC top-centre ray tilts upward by the half field of view");
        const Ray bottomRight = engine::cameraRay(camera, glm::vec2{1.0f, 1.0f}, 1.0f);
        check(bottomRight.direction.x > 0.35f && bottomRight.direction.y < -0.35f && bottomRight.direction.z < -0.7f,
              "NDC bottom-right ray tilts right and down");
        check(nearlyEqual(glm::length(bottomRight.direction), 1.0, 1e-5), "camera ray direction is normalized");
        const Ray identity = engine::rayThroughNdc(glm::vec2{0.0f, 0.0f}, glm::mat4{1.0f});
        check(vecNear(identity.origin, glm::vec3{0.0f}) && vecNear(identity.direction, glm::vec3{0.0f, 0.0f, 1.0f}),
              "unprojection through an identity matrix runs down +Z");
    }

    // --- Ray vs AABB (slab method) -------------------------------------------
    {
        const AABB box{glm::vec3{-0.5f}, glm::vec3{0.5f}};
        float distance = 0.0f;
        glm::vec3 normal{0.0f};
        const Ray front{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}};
        check(engine::rayIntersectsAABB(front, box, distance, normal) && nearlyEqual(distance, 4.5, 1e-5) &&
                  vecNear(normal, glm::vec3{0.0f, 0.0f, 1.0f}) && vecNear(front.at(distance), glm::vec3{0.0f, 0.0f, 0.5f}),
              "ray hits the front face of a box at the exact distance with the face normal");
        const Ray side{{5.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}};
        check(engine::rayIntersectsAABB(side, box, distance, normal) && nearlyEqual(distance, 4.5, 1e-5) &&
                  vecNear(normal, glm::vec3{1.0f, 0.0f, 0.0f}),
              "ray hits a box side face with that face's normal");
        check(!engine::rayIntersectsAABB(Ray{{2.0f, 0.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, box, distance, normal),
              "offset ray misses the box");
        check(!engine::rayIntersectsAABB(Ray{{0.0f, 0.0f, 5.0f}, {0.0f, 0.0f, 1.0f}}, box, distance, normal),
              "box behind the ray origin is not hit");
        check(!engine::rayIntersectsAABB(Ray{{0.0f, 2.0f, 5.0f}, {0.0f, 0.0f, -1.0f}}, box, distance, normal),
              "ray parallel to a slab and outside it misses");
        check(engine::rayIntersectsAABB(Ray{{0.0f, 0.2f, 5.0f}, {0.0f, 0.0f, -1.0f}}, box, distance, normal) &&
                  nearlyEqual(distance, 4.5, 1e-5),
              "ray parallel to a slab and inside it still hits");
        const Ray inside{{0.1f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};
        check(engine::rayIntersectsAABB(inside, box, distance, normal) && distance == 0.0f &&
                  vecNear(normal, glm::vec3{0.0f, 0.0f, 1.0f}),
              "ray starting inside a box hits at distance zero");
    }

    // --- Ray vs triangle (Moller-Trumbore) -------------------------------------
    {
        const glm::vec3 a{0.0f, 0.0f, 0.0f};
        const glm::vec3 b{2.0f, 0.0f, 0.0f};
        const glm::vec3 c{0.0f, 2.0f, 0.0f};
        float distance = 0.0f;
        glm::vec2 barycentric{0.0f};
        const Ray down{{0.25f, 0.25f, 1.0f}, {0.0f, 0.0f, -1.0f}};
        check(engine::rayIntersectsTriangle(down, a, b, c, distance, barycentric) &&
                  nearlyEqual(distance, 1.0, 1e-5) && nearlyEqual(barycentric.x, 0.125, 1e-5) &&
                  nearlyEqual(barycentric.y, 0.125, 1e-5) &&
                  vecNear(down.at(distance), glm::vec3{0.25f, 0.25f, 0.0f}),
              "ray hits a triangle at the exact distance with barycentric coordinates");
        check(!engine::rayIntersectsTriangle(Ray{{1.5f, 1.5f, 1.0f}, {0.0f, 0.0f, -1.0f}}, a, b, c, distance,
                                             barycentric),
              "ray past the triangle's hypotenuse misses");
        check(engine::rayIntersectsTriangle(Ray{{0.25f, 0.25f, -1.0f}, {0.0f, 0.0f, 1.0f}}, a, b, c, distance,
                                            barycentric) &&
                  nearlyEqual(distance, 1.0, 1e-5),
              "triangle test is two-sided (back face hits)");
        check(!engine::rayIntersectsTriangle(Ray{{0.25f, 0.25f, 1.0f}, {0.0f, 0.0f, 1.0f}}, a, b, c, distance,
                                             barycentric),
              "triangle behind the ray is not hit");
        check(!engine::rayIntersectsTriangle(Ray{{0.25f, 0.25f, 1.0f}, {1.0f, 0.0f, 0.0f}}, a, b, c, distance,
                                             barycentric),
              "ray parallel to the triangle plane misses");
    }

    // --- Scene raycast queries -------------------------------------------------
    const auto addBoxCube = [](engine::Scene& scene, const char* name, const glm::vec3& position,
                               float rotationYDegrees = 0.0f) {
        auto cube = scene.createEntity(name);
        auto& transform = cube.addComponent<TransformComponent>(position);
        transform.rotationDegrees.y = rotationYDegrees;
        cube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        return cube;
    };
    {
        engine::Scene scene;
        auto cube = addBoxCube(scene, "TargetCube", glm::vec3{0.0f, 0.0f, -5.0f});
        RaycastHit hit;
        const Ray ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};
        check(scene.raycast(ray, hit) && hit.entity == cube.handle() && nearlyEqual(hit.distance, 4.5, 1e-4) &&
                  vecNear(hit.position, glm::vec3{0.0f, 0.0f, -4.5f}) && vecNear(hit.normal, glm::vec3{0.0f, 0.0f, 1.0f}),
              "scene raycast hits a box collider with distance, point and normal");
        check(!scene.raycast(Ray{{0.0f, 3.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, hit),
              "scene raycast reports a miss above the cube");
        RaycastHit insideHit;
        check(scene.raycast(Ray{{0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, -1.0f}}, insideHit) && insideHit.distance == 0.0f,
              "scene raycast starting inside a collider hits at distance zero");
    }
    {
        engine::Scene scene;
        auto nearCube = addBoxCube(scene, "NearCube", glm::vec3{0.0f, 0.0f, -5.0f});
        auto farCube = addBoxCube(scene, "FarCube", glm::vec3{0.0f, 0.0f, -8.0f});
        const Ray ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};
        RaycastHit hit;
        check(scene.raycast(ray, hit) && hit.entity == nearCube.handle(),
              "scene raycast returns the closest of several hits");
        std::vector<RaycastHit> hits;
        check(scene.raycastAll(ray, hits) == 2 && hits[0].entity == nearCube.handle() &&
                  hits[1].entity == farCube.handle() && hits[0].distance < hits[1].distance,
              "raycastAll returns every hit sorted by distance");
        check(scene.raycastAll(ray, hits, 6.0f) == 1 && hits[0].entity == nearCube.handle(),
              "raycastAll honours maxDistance");
        check(!scene.raycast(ray, hit, 4.0f), "scene raycast honours maxDistance");
    }
    {
        // Rotated box: an exact local-space (OBB) test. A world-AABB
        // approximation would report 4.293 for both rays; the true
        // rotated-square cross-section gives 4.293 through the centre
        // vertex but 4.943 at x = 0.65, entering through the slanted
        // face whose normal is (sqrt(2)/2, 0, sqrt(2)/2).
        engine::Scene scene;
        auto cube = addBoxCube(scene, "RotatedCube", glm::vec3{0.0f, 0.0f, -5.0f}, 45.0f);
        RaycastHit hit;
        check(scene.raycast(Ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, hit) &&
                  nearlyEqual(hit.distance, 5.0 - std::sqrt(2.0) / 2.0, 1e-3),
              "raycast against a 45-degree box hits the leading vertex");
        check(scene.raycast(Ray{{0.65f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, hit) &&
                  nearlyEqual(hit.distance, 4.943, 1e-3) &&
                  vecNear(hit.normal, glm::vec3{std::sqrt(2.0f) / 2.0f, 0.0f, std::sqrt(2.0f) / 2.0f}, 1e-3f),
              "raycast against a rotated box is an exact OBB test, not a world AABB");
        (void)cube;
    }
    {
        // Non-box colliders are not raycast; the entity behind them is.
        engine::Scene scene;
        auto blocker = scene.createEntity("CapsuleBlocker");
        blocker.addComponent<TransformComponent>(glm::vec3{0.0f, 0.0f, -3.0f});
        blocker.addComponent<ColliderComponent>(ColliderComponent::capsule(0.30f, 0.45f));
        auto cube = addBoxCube(scene, "BehindCube", glm::vec3{0.0f, 0.0f, -5.0f});
        RaycastHit hit;
        check(scene.raycast(Ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, hit) && hit.entity == cube.handle() &&
                  nearlyEqual(hit.distance, 4.5, 1e-4),
              "raycast passes through a capsule-only entity and hits the box behind it");
    }
    {
        // Mesh bounds path (no collider): visible meshes are pickable
        // through their local bounds; invisible ones are not, but an
        // invisible mesh's collider still is.
        engine::Scene scene;
        auto meshOnly = scene.createEntity("MeshOnly");
        meshOnly.addComponent<TransformComponent>(glm::vec3{3.0f, 0.0f, -5.0f});
        meshOnly.addComponent<MeshComponent>();
        RaycastHit hit;
        check(scene.raycast(Ray{{3.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, hit) &&
                  hit.entity == meshOnly.handle() && nearlyEqual(hit.distance, 4.5, 1e-4),
              "raycast hits a mesh-only entity through its local bounds");
        check(!scene.raycast(Ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, hit),
              "raycast misses a mesh-only entity outside its bounds");
        meshOnly.getComponent<MeshComponent>().visible = false;
        check(!scene.raycast(Ray{{3.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, hit),
              "invisible mesh is not picked through its bounds");
        meshOnly.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        check(scene.raycast(Ray{{3.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, hit) &&
                  hit.entity == meshOnly.handle(),
              "invisible mesh's box collider is still picked");
    }

    // --- Precise (per-triangle) picking -----------------------------------------
    {
        engine::MeshAsset triangleAsset;
        triangleAsset.name = "tri";
        triangleAsset.vertices = {engine::Vertex{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f}},
                                  engine::Vertex{{2.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f}},
                                  engine::Vertex{{0.0f, 2.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, {0.0f, 0.0f}}};
        triangleAsset.indices = {0, 1, 2};
        triangleAsset.bounds = engine::computeMeshBounds(triangleAsset.vertices);

        engine::Scene scene;
        auto triangle = scene.createEntity("Triangle");
        triangle.addComponent<TransformComponent>(glm::vec3{0.0f, 0.0f, -5.0f});
        auto& mesh = triangle.addComponent<MeshComponent>();
        mesh.assetId = "tri";
        mesh.localBounds = triangleAsset.bounds.aabb();

        const engine::Scene::MeshResolver resolver = [&triangleAsset](std::string_view assetId) {
            return assetId == "tri" ? &triangleAsset : nullptr;
        };
        const engine::Scene::MeshResolver noResolver = [](std::string_view) { return nullptr; };

        RaycastHit hit;
        const Ray throughTriangle{{0.25f, 0.25f, 0.0f}, {0.0f, 0.0f, -1.0f}};
        check(scene.raycastPrecise(throughTriangle, hit, resolver) && hit.entity == triangle.handle() &&
                  nearlyEqual(hit.distance, 5.0, 1e-4) && vecNear(hit.position, glm::vec3{0.25f, 0.25f, -5.0f}) &&
                  vecNear(hit.normal, glm::vec3{0.0f, 0.0f, 1.0f}),
              "precise raycast hits the exact triangle surface");
        const Ray pastTriangle{{1.5f, 1.5f, 0.0f}, {0.0f, 0.0f, -1.0f}};
        check(scene.raycast(pastTriangle, hit) && nearlyEqual(hit.distance, 5.0, 1e-4),
              "bounds raycast hits where only the bounding box is present");
        check(!scene.raycastPrecise(pastTriangle, hit, resolver),
              "precise raycast misses where the ray only clips the bounding box");
        check(scene.raycastPrecise(pastTriangle, hit, noResolver) && nearlyEqual(hit.distance, 5.0, 1e-4),
              "precise raycast falls back to bounds when the asset cannot be resolved");
    }

    // --- End to end: camera ray into a scene --------------------------------------
    {
        engine::Scene scene;
        auto cube = addBoxCube(scene, "PickTarget", glm::vec3{0.0f, 0.0f, -5.0f});
        engine::Camera camera{engine::CameraConfig{}}; // eye at (0,0,3) looking down -Z
        RaycastHit hit;
        check(scene.raycast(engine::cameraRay(camera, glm::vec2{0.0f, 0.0f}, 1.0f), hit) &&
                  hit.entity == cube.handle() && nearlyEqual(hit.distance, 7.5, 1e-3),
              "camera centre ray picks the cube straight ahead at eye distance");
        check(!scene.raycast(engine::cameraRay(camera, glm::vec2{0.9f, 0.0f}, 1.0f), hit),
              "camera ray near the screen edge misses the cube");
    }
}

void testSpatialPartitioning() {
    std::cout << "Spatial partitioning (Phase 14: dynamic AABB tree / BVH)\n";
    using engine::AABB;
    using engine::ColliderComponent;
    using engine::DynamicAABBTree;
    using engine::Ray;
    using engine::RaycastHit;
    using engine::TransformComponent;

    const auto entityAt = [](uint32_t id) { return static_cast<entt::entity>(id); };
    const auto boxAround = [](const glm::vec3& center, float half) {
        return AABB{center - glm::vec3{half}, center + glm::vec3{half}};
    };

    // --- Insertion, fattening, basic queries -----------------------------------
    {
        DynamicAABBTree tree;
        check(tree.leafCount() == 0 && tree.nodeCount() == 0 && tree.height() == 0 && tree.validate(),
              "empty dynamic AABB tree is valid");
        const AABB tight = boxAround(glm::vec3{0.0f}, 0.5f);
        const DynamicAABBTree::ProxyId proxy = tree.insertLeaf(tight, entityAt(7));
        check(proxy != DynamicAABBTree::kNullProxy && tree.leafCount() == 1 && tree.nodeCount() == 1 &&
                  tree.height() == 0 && tree.entity(proxy) == entityAt(7),
              "first leaf registers its entity");
        check(tree.proxyAabb(proxy).contains(tight) && tree.proxyAabb(proxy).min.x < tight.min.x &&
                  tree.proxyAabb(proxy).max.x > tight.max.x && tree.validate(),
              "leaf box is fattened beyond the tight box on every side");

        std::vector<entt::entity> found;
        const auto collect = [&found](entt::entity entity) {
            found.push_back(entity);
            return true;
        };
        tree.query(boxAround(glm::vec3{0.0f}, 0.25f), collect);
        check(found.size() == 1 && found[0] == entityAt(7), "overlap query finds the leaf");
        found.clear();
        tree.query(boxAround(glm::vec3{10.0f, 0.0f, 0.0f}, 0.25f), collect);
        check(found.empty(), "overlap query away from the leaf finds nothing");
        tree.removeLeaf(proxy);
        check(tree.leafCount() == 0 && tree.height() == 0 && tree.validate(),
              "removing the only leaf empties the tree");
        tree.query(boxAround(glm::vec3{0.0f}, 5.0f), collect);
        check(found.empty(), "query on an emptied tree finds nothing");
    }

    // --- Query early exit ---------------------------------------------------------
    {
        DynamicAABBTree tree;
        for (uint32_t i = 0; i < 3; ++i) {
            tree.insertLeaf(boxAround(glm::vec3{static_cast<float>(i) * 0.25f, 0.0f, 0.0f}, 0.5f),
                            entityAt(20 + i));
        }
        size_t callbacks = 0;
        tree.query(boxAround(glm::vec3{0.0f}, 2.0f), [&callbacks](entt::entity) {
            ++callbacks;
            return false;
        });
        check(callbacks == 1 && tree.validate(), "query callback can stop the traversal early");
        callbacks = 0;
        tree.query(boxAround(glm::vec3{0.0f}, 2.0f), [&callbacks](entt::entity) {
            ++callbacks;
            return true;
        });
        check(callbacks == 3, "query visits every overlapping leaf");
    }

    // --- moveLeaf: fat box absorbs small motion, refits on escape ------------------
    {
        DynamicAABBTree tree;
        const DynamicAABBTree::ProxyId proxy = tree.insertLeaf(boxAround(glm::vec3{0.0f}, 0.5f), entityAt(1));
        const AABB fatBefore = tree.proxyAabb(proxy);
        check(!tree.moveLeaf(proxy, boxAround(glm::vec3{0.05f, 0.0f, 0.0f}, 0.5f),
                             glm::vec3{0.01f, 0.0f, 0.0f}),
              "movement inside the fat box does not refit the leaf");
        check(tree.proxyAabb(proxy).min == fatBefore.min && tree.proxyAabb(proxy).max == fatBefore.max &&
                  tree.validate(),
              "leaf keeps its fat box after an absorbed move");
        check(tree.moveLeaf(proxy, boxAround(glm::vec3{5.0f, 0.0f, 0.0f}, 0.5f), glm::vec3{5.0f, 0.0f, 0.0f}),
              "escaping the fat box refits the leaf");
        check(tree.proxyAabb(proxy).contains(boxAround(glm::vec3{5.0f, 0.0f, 0.0f}, 0.5f)) && tree.validate(),
              "refit leaf covers its new position");
        std::vector<entt::entity> found;
        tree.query(boxAround(glm::vec3{5.0f, 0.0f, 0.0f}, 0.25f), [&found](entt::entity entity) {
            found.push_back(entity);
            return true;
        });
        check(found.size() == 1 && found[0] == entityAt(1), "moved leaf is found at its new position");
        found.clear();
        tree.query(boxAround(glm::vec3{0.0f}, 0.25f), [&found](entt::entity entity) {
            found.push_back(entity);
            return true;
        });
        check(found.empty(), "moved leaf no longer overlaps its old position");
    }

    // --- Removal splices parents; clear resets --------------------------------------
    {
        DynamicAABBTree tree;
        std::vector<DynamicAABBTree::ProxyId> proxies;
        for (uint32_t i = 0; i < 5; ++i) {
            proxies.push_back(tree.insertLeaf(
                boxAround(glm::vec3{static_cast<float>(i) * 2.0f, 0.0f, 0.0f}, 0.5f), entityAt(i)));
        }
        check(tree.leafCount() == 5 && tree.nodeCount() == 9 && tree.validate(),
              "five leaves build four internal nodes");
        tree.removeLeaf(proxies[2]);
        check(tree.leafCount() == 4 && tree.nodeCount() == 7 && tree.validate(),
              "removal splices out the leaf and its parent");
        std::vector<uint32_t> found;
        tree.query(boxAround(glm::vec3{4.0f, 0.0f, 0.0f}, 100.0f), [&found](entt::entity entity) {
            found.push_back(entt::to_integral(entity));
            return true;
        });
        std::sort(found.begin(), found.end());
        check(found == std::vector<uint32_t>{0, 1, 3, 4}, "removed leaf is gone, siblings remain queryable");
        tree.clear();
        check(tree.leafCount() == 0 && tree.nodeCount() == 0 && tree.validate(), "clear resets the tree");
    }

    // --- Balancing: worst-case monotonic insertion, dense grid -----------------------
    {
        DynamicAABBTree tree;
        for (uint32_t i = 0; i < 256; ++i) {
            tree.insertLeaf(boxAround(glm::vec3{static_cast<float>(i), 0.0f, 0.0f}, 0.4f), entityAt(i));
        }
        check(tree.leafCount() == 256 && tree.height() <= 18 && tree.validate(),
              "monotonic insertions stay balanced (SAH sibling choice + rotations)");
        std::vector<uint32_t> found;
        const auto counting = [&found](entt::entity entity) {
            found.push_back(entt::to_integral(entity));
            return true;
        };
        tree.query(boxAround(glm::vec3{100.0f, 0.0f, 0.0f}, 0.9f), counting);
        std::sort(found.begin(), found.end());
        check(found == std::vector<uint32_t>{99, 100, 101},
              "narrow query over a long tree returns exactly the overlapping leaves");
        found.clear();
        tree.query(boxAround(glm::vec3{128.0f, 0.0f, 0.0f}, 200.0f), counting);
        check(found.size() == 256, "spanning query over a long tree reports every leaf");
    }
    {
        DynamicAABBTree tree;
        uint32_t id = 0;
        for (int x = 0; x < 8; ++x) {
            for (int y = 0; y < 8; ++y) {
                for (int z = 0; z < 8; ++z) {
                    tree.insertLeaf(boxAround(glm::vec3{static_cast<float>(x) * 2.0f,
                                                        static_cast<float>(y) * 2.0f,
                                                        static_cast<float>(z) * 2.0f},
                                              0.5f),
                                    entityAt(id++));
                }
            }
        }
        size_t found = 0;
        tree.query(boxAround(glm::vec3{7.0f}, 100.0f), [&found](entt::entity) {
            ++found;
            return true;
        });
        check(tree.leafCount() == 512 && found == 512 && tree.height() <= 20 && tree.validate(),
              "512-leaf grid tree is complete, balanced, and structurally valid");
        found = 0;
        tree.query(boxAround(glm::vec3{500.0f}, 1.0f), [&found](entt::entity) {
            ++found;
            return true;
        });
        check(found == 0, "query far outside the grid is rejected at the root");
    }

    // --- Randomized stress vs brute force ---------------------------------------------
    {
        DynamicAABBTree tree;
        uint32_t rngState = 0x01234567u;
        const auto nextUnit = [&rngState]() {
            rngState = rngState * 1664525u + 1013904223u;
            return static_cast<float>((rngState >> 9) & 0x7FFFFFu) / static_cast<float>(0x800000u);
        };
        const auto randomBox = [&nextUnit]() {
            const glm::vec3 center{(nextUnit() - 0.5f) * 40.0f, (nextUnit() - 0.5f) * 40.0f,
                                   (nextUnit() - 0.5f) * 40.0f};
            const glm::vec3 half{0.25f + nextUnit() * 0.75f};
            return AABB{center - half, center + half};
        };

        struct Leaf {
            DynamicAABBTree::ProxyId proxy = DynamicAABBTree::kNullProxy;
            entt::entity entity = entt::null;
            AABB tight{};
        };
        std::vector<Leaf> leaves;
        for (uint32_t i = 0; i < 300; ++i) {
            const AABB tight = randomBox();
            leaves.push_back(Leaf{tree.insertLeaf(tight, entityAt(1000 + i)), entityAt(1000 + i), tight});
        }
        check(tree.leafCount() == 300 && tree.validate(), "300 randomized insertions keep the tree valid");

        const auto fatContainsAll = [&tree, &leaves]() {
            for (const Leaf& leaf : leaves) {
                if (!tree.proxyAabb(leaf.proxy).contains(leaf.tight)) {
                    return false;
                }
            }
            return true;
        };
        const auto bruteForceQuery = [&tree, &leaves](const AABB& queryBox) {
            std::vector<uint32_t> expected;
            for (const Leaf& leaf : leaves) {
                if (tree.proxyAabb(leaf.proxy).overlaps(queryBox)) {
                    expected.push_back(entt::to_integral(leaf.entity));
                }
            }
            std::sort(expected.begin(), expected.end());
            return expected;
        };
        const auto treeQuery = [&tree](const AABB& queryBox) {
            std::vector<uint32_t> found;
            tree.query(queryBox, [&found](entt::entity entity) {
                found.push_back(entt::to_integral(entity));
                return true;
            });
            std::sort(found.begin(), found.end());
            return found;
        };
        bool queriesMatch = true;
        for (int q = 0; q < 25; ++q) {
            const AABB queryBox = randomBox();
            queriesMatch = queriesMatch && treeQuery(queryBox) == bruteForceQuery(queryBox);
        }
        check(queriesMatch && fatContainsAll(), "randomized overlap queries match brute force over fat boxes");

        for (int round = 0; round < 30; ++round) {
            for (int k = 0; k < 20; ++k) {
                Leaf& leaf = leaves[static_cast<size_t>(nextUnit() * static_cast<float>(leaves.size()))];
                const AABB moved = randomBox();
                tree.moveLeaf(leaf.proxy, moved, moved.center() - leaf.tight.center());
                leaf.tight = moved;
            }
        }
        queriesMatch = true;
        for (int q = 0; q < 25; ++q) {
            const AABB queryBox = randomBox();
            queriesMatch = queriesMatch && treeQuery(queryBox) == bruteForceQuery(queryBox);
        }
        check(tree.validate() && fatContainsAll() && queriesMatch,
              "randomized moves keep every fat box covering its leaf and queries exact");

        std::vector<Leaf> survivors;
        for (size_t i = 0; i < leaves.size(); ++i) {
            if (i % 3 == 0) {
                tree.removeLeaf(leaves[i].proxy);
            } else {
                survivors.push_back(leaves[i]);
            }
        }
        leaves = survivors;
        queriesMatch = true;
        for (int q = 0; q < 10; ++q) {
            const AABB queryBox = randomBox();
            queriesMatch = queriesMatch && treeQuery(queryBox) == bruteForceQuery(queryBox);
        }
        check(tree.validate() && tree.leafCount() == leaves.size() && queriesMatch,
              "bulk removals keep the tree valid and queries exact");
    }

    // --- Tree ray traversal -----------------------------------------------------------
    {
        DynamicAABBTree tree;
        std::vector<DynamicAABBTree::ProxyId> proxies;
        for (uint32_t i = 0; i < 3; ++i) {
            proxies.push_back(tree.insertLeaf(
                boxAround(glm::vec3{0.0f, 0.0f, -5.0f - static_cast<float>(i) * 5.0f}, 0.5f), entityAt(50 + i)));
        }
        for (uint32_t i = 0; i < 20; ++i) {
            proxies.push_back(tree.insertLeaf(
                boxAround(glm::vec3{-15.0f + static_cast<float>(i % 5) * 7.0f,
                                    4.0f + static_cast<float>(i / 5) * 3.0f,
                                    -6.0f - static_cast<float>(i % 4) * 6.0f},
                          0.5f),
                entityAt(100 + i)));
        }
        const Ray axisRay{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};

        std::vector<uint32_t> brute;
        float entryDistance = 0.0f;
        glm::vec3 entryNormal{0.0f};
        for (const DynamicAABBTree::ProxyId proxy : proxies) {
            if (engine::rayIntersectsAABB(axisRay, tree.proxyAabb(proxy), entryDistance, entryNormal)) {
                brute.push_back(entt::to_integral(tree.entity(proxy)));
            }
        }
        std::sort(brute.begin(), brute.end());

        std::vector<uint32_t> traversed;
        tree.raycast(axisRay, 1000.0f, [&traversed](entt::entity entity) {
            traversed.push_back(entt::to_integral(entity));
            return 1000.0f;
        });
        std::sort(traversed.begin(), traversed.end());
        check(traversed == brute && traversed.size() == 3, "tree raycast visits exactly the pierced leaves");

        traversed.clear();
        tree.raycast(axisRay, 6.0f, [&traversed](entt::entity entity) {
            traversed.push_back(entt::to_integral(entity));
            return 6.0f;
        });
        check(traversed.size() == 1 && traversed[0] == 50, "tree raycast honours the maximum distance");

        traversed.clear();
        tree.raycast(axisRay, 1000.0f, [&traversed](entt::entity entity) {
            traversed.push_back(entt::to_integral(entity));
            return 0.0f;
        });
        check(traversed.size() == 1, "raycast callback returning zero stops the traversal");

        // Clip-tightening: the callback measures each candidate's fat-box
        // entry and returns the running best, exactly what Scene picking
        // does with real hits.
        float best = 1000.0f;
        size_t visits = 0;
        tree.raycast(axisRay, 1000.0f, [&](entt::entity entity) {
            ++visits;
            for (const DynamicAABBTree::ProxyId proxy : proxies) {
                if (tree.entity(proxy) == entity &&
                    engine::rayIntersectsAABB(axisRay, tree.proxyAabb(proxy), entryDistance, entryNormal)) {
                    best = std::min(best, entryDistance);
                }
            }
            return best;
        });
        check(nearlyEqual(best, 4.4, 1e-4) && visits >= 1 && visits <= 3,
              "clip tightening prunes subtrees beyond the closest candidate");

        size_t misses = 0;
        tree.raycast(Ray{{0.0f, 50.0f, 0.0f}, {1.0f, 0.0f, 0.0f}}, 1000.0f, [&misses](entt::entity) {
            ++misses;
            return 1000.0f;
        });
        check(misses == 0 && tree.validate(), "ray missing the root box visits no leaves");
    }

    // --- Scene picking through the BVH --------------------------------------------------
    {
        engine::Scene scene;
        const auto addCube = [&scene](const char* name, const glm::vec3& position) {
            auto cube = scene.createEntity(name);
            cube.addComponent<TransformComponent>(position);
            cube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
            return cube;
        };
        auto nearCube = addCube("BVHNear", glm::vec3{0.0f, 0.0f, -5.0f});
        auto midCube = addCube("BVHMid", glm::vec3{0.0f, 0.0f, -10.0f});
        auto farCube = addCube("BVHFar", glm::vec3{0.0f, 0.0f, -15.0f});
        for (int i = 0; i < 24; ++i) {
            addCube("BVHClutter", glm::vec3{-22.0f + static_cast<float>(i % 6) * 4.0f,
                                            3.0f + static_cast<float>(i / 6) * 3.0f,
                                            -8.0f - static_cast<float>(i % 4) * 5.0f});
        }
        const Ray axisRay{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};
        RaycastHit hit;
        check(scene.raycast(axisRay, hit) && hit.entity == nearCube.handle() &&
                  nearlyEqual(hit.distance, 4.5, 1e-4),
              "BVH picking finds the closest cube in a cluttered scene");
        std::vector<RaycastHit> hits;
        check(scene.raycastAll(axisRay, hits) == 3 && hits[0].entity == nearCube.handle() &&
                  hits[1].entity == midCube.handle() && hits[2].entity == farCube.handle() &&
                  nearlyEqual(hits[0].distance, 4.5, 1e-4) && nearlyEqual(hits[1].distance, 9.5, 1e-4) &&
                  nearlyEqual(hits[2].distance, 14.5, 1e-4),
              "BVH raycastAll returns the row of cubes in distance order");
        check(!scene.raycast(axisRay, hit, 4.0f), "maxDistance still clips BVH picking");

        scene.destroyEntity(nearCube);
        check(scene.raycast(axisRay, hit) && hit.entity == midCube.handle() &&
                  nearlyEqual(hit.distance, 9.5, 1e-4),
              "destroyed cube is pruned from the BVH; the cube behind it is picked");
        midCube.getComponent<TransformComponent>().position = glm::vec3{10.0f, 0.0f, -10.0f};
        check(scene.raycast(axisRay, hit) && hit.entity == farCube.handle() &&
                  nearlyEqual(hit.distance, 14.5, 1e-4),
              "moved cube leaves the ray path in the BVH");
        check(scene.raycast(Ray{{10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, hit) &&
                  hit.entity == midCube.handle() && nearlyEqual(hit.distance, 9.5, 1e-4),
              "moved cube is picked at its new location");
        midCube.getComponent<TransformComponent>().position.x = 10.05f;
        check(scene.raycast(Ray{{10.05f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, hit) &&
                  hit.entity == midCube.handle(),
              "small moves inside the fat box stay pickable without a refit");
    }

    // --- Physics broadphase through the tree ----------------------------------------------
    {
        using engine::PhysicsSettings;
        using engine::PhysicsSystem;
        using engine::RigidBodyComponent;

        engine::Scene scene;
        auto floor = scene.createEntity("TreeFloor");
        floor.addComponent<TransformComponent>(glm::vec3{0.0f, -0.5f, 0.0f});
        floor.addComponent<RigidBodyComponent>().setStatic(true);
        floor.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{8.0f, 0.5f, 8.0f}));
        for (int i = -1; i <= 1; ++i) {
            auto cube = scene.createEntity("TreeCube");
            cube.addComponent<TransformComponent>(glm::vec3{static_cast<float>(i) * 4.0f, 0.55f, 0.0f});
            cube.addComponent<RigidBodyComponent>(1.0f);
            cube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        }
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f, -10.0f, 0.0f};
        PhysicsSystem physics;
        const PhysicsSystem::Stats stats = physics.update(scene, 1.0 / 60.0, settings);
        check(stats.bodies == 4 && stats.candidatePairs == 3,
              "tree broadphase pairs each resting-height cube with the floor only");
        for (int step = 0; step < 120; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        const PhysicsSystem::Stats settled = physics.lastStats();
        check(settled.candidatePairs == 3 && settled.contacts > 0,
              "resting cubes keep exactly their floor pairs (no cube-cube candidates)");
    }
    {
        using engine::PhysicsSettings;
        using engine::PhysicsSystem;
        using engine::RigidBodyComponent;

        engine::Scene scene;
        auto plane = scene.createEntity("TreePlane");
        plane.addComponent<TransformComponent>(glm::vec3{0.0f});
        plane.addComponent<RigidBodyComponent>().setStatic(true);
        plane.addComponent<ColliderComponent>(ColliderComponent::plane(glm::vec3{0.0f, 1.0f, 0.0f}, 0.0f));
        for (int i = 0; i < 2; ++i) {
            auto cube = scene.createEntity("TreePlaneCube");
            cube.addComponent<TransformComponent>(glm::vec3{i == 0 ? -10.0f : 10.0f, 3.0f, 0.0f});
            cube.addComponent<RigidBodyComponent>(1.0f);
            cube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        }
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f, -10.0f, 0.0f};
        PhysicsSystem physics;
        const PhysicsSystem::Stats stats = physics.update(scene, 1.0 / 60.0, settings);
        check(stats.candidatePairs == 2, "infinite planes bypass the tree and pair against every body");
    }
}

void testCollisionLayers() {
    std::cout << "Collision layers & masks (Phase 15)\n";
    using engine::ColliderComponent;
    using engine::Ray;
    using engine::RaycastHit;
    using engine::TransformComponent;
    using engine::shouldCollide;

    const uint32_t LNone = engine::CollisionLayer::None;
    const uint32_t LDefault = engine::CollisionLayer::Default;
    const uint32_t LPlayer = engine::CollisionLayer::Player;
    const uint32_t LEnemy = engine::CollisionLayer::Enemy;
    const uint32_t LEnvironment = engine::CollisionLayer::Environment;
    const uint32_t LTrigger = engine::CollisionLayer::Trigger;
    const uint32_t LAll = engine::CollisionLayer::All;

    // --- Named categories ----------------------------------------------------------
    {
        const uint32_t named = LDefault | LPlayer | LEnemy | LEnvironment | LTrigger;
        check(LNone == 0u && LAll == 0xFFFFFFFFu && LDefault == (1u << 0) &&
                  (named & (named - 1u)) != 0u && // combined, sanity
                  (LDefault & LPlayer) == 0u && (LPlayer & LEnemy) == 0u && (LEnemy & LEnvironment) == 0u &&
                  (LEnvironment & LTrigger) == 0u,
              "named layers are distinct single bits with None and All bookends");
    }

    // --- The mutual bitmask rule ------------------------------------------------------
    {
        check(shouldCollide(LDefault, LAll, LDefault, LAll), "defaults collide with defaults");
        check(shouldCollide(LPlayer, LAll, LEnemy, LAll), "full masks collide across layers");
        check(!shouldCollide(LPlayer, LAll & ~LEnemy, LEnemy, LAll),
              "excluding the other's layer from my mask blocks collision");
        check(!shouldCollide(LPlayer, LAll, LEnemy, LAll & ~LPlayer),
              "being excluded from the other's mask blocks collision");
        check(!shouldCollide(LPlayer, LAll & ~LEnemy, LEnemy, LAll & ~LPlayer),
              "mutual exclusion blocks collision");
        check(!shouldCollide(LPlayer, LNone, LEnemy, LAll), "an empty mask collides with nothing");
        check(!shouldCollide(LPlayer, LAll, LNone, LAll), "a collider on no layer never collides");
        check(shouldCollide(LPlayer, LEnemy, LEnemy, LPlayer), "cross-accepting layers collide");
        check(!shouldCollide(LPlayer, LEnemy, LEnemy, LTrigger),
              "one-way acceptance is not enough: the check is mutual");
        bool symmetric = true;
        const uint32_t layers[5] = {LDefault, LPlayer, LEnemy, LEnvironment, LTrigger};
        const uint32_t masks[4] = {LAll, LNone, LPlayer | LEnemy, LDefault | LTrigger};
        for (const uint32_t layerA : layers) {
            for (const uint32_t layerB : layers) {
                for (const uint32_t maskA : masks) {
                    for (const uint32_t maskB : masks) {
                        symmetric = symmetric &&
                                    shouldCollide(layerA, maskA, layerB, maskB) ==
                                        shouldCollide(layerB, maskB, layerA, maskA);
                    }
                }
            }
        }
        check(symmetric, "shouldCollide is symmetric over the layer/mask truth table");
    }

    // --- Component defaults ----------------------------------------------------------------
    {
        const ColliderComponent box = ColliderComponent::box(glm::vec3{0.5f});
        const ColliderComponent sphere = ColliderComponent::sphere(0.5f);
        const ColliderComponent capsule = ColliderComponent::capsule(0.3f, 0.4f);
        const ColliderComponent plane = ColliderComponent::plane(glm::vec3{0.0f, 1.0f, 0.0f}, 0.0f);
        check(box.layer == LDefault && box.mask == LAll && sphere.layer == LDefault && sphere.mask == LAll &&
                  capsule.layer == LDefault && capsule.mask == LAll && plane.layer == LDefault &&
                  plane.mask == LAll,
              "every collider factory defaults to layer Default, mask All");
        ColliderComponent a = ColliderComponent::box(glm::vec3{0.5f});
        a.layer = LPlayer;
        a.mask = LAll & ~LEnemy;
        ColliderComponent b = ColliderComponent::box(glm::vec3{0.5f});
        b.layer = LEnemy;
        check(!shouldCollide(a, b), "component overload applies the mutual rule");
        a.mask = LAll;
        check(shouldCollide(a, b), "component overload accepts once the mask re-includes the layer");
    }

    // --- Broadphase filter: overlapping bodies, excluded layers -------------------------------
    {
        using engine::PhysicsSettings;
        using engine::PhysicsSystem;
        using engine::RigidBodyComponent;

        engine::Scene scene;
        auto first = scene.createEntity("LayerA");
        first.addComponent<TransformComponent>(glm::vec3{0.0f});
        first.addComponent<RigidBodyComponent>().setStatic(true);
        auto& firstCollider = first.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{1.0f}));
        firstCollider.layer = LPlayer;
        firstCollider.mask = LAll & ~LEnemy;
        auto second = scene.createEntity("LayerB");
        second.addComponent<TransformComponent>(glm::vec3{1.0f, 0.0f, 0.0f});
        second.addComponent<RigidBodyComponent>().setStatic(true);
        auto& secondCollider = second.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{1.0f}));
        secondCollider.layer = LEnemy;

        PhysicsSettings settings;
        PhysicsSystem physics;
        const PhysicsSystem::Stats filtered = physics.update(scene, 1.0 / 60.0, settings);
        check(filtered.bodies == 2 && filtered.candidatePairs == 0,
              "overlapping bodies on excluded layers produce no broadphase pair");
        first.getComponent<ColliderComponent>().mask = LAll;
        const PhysicsSystem::Stats unfiltered = physics.update(scene, 1.0 / 60.0, settings);
        check(unfiltered.candidatePairs == 1,
              "re-including the layer restores the pair on the next step (filter reads live values)");
    }

    // --- Integration: pass-through vs landing ---------------------------------------------------
    {
        using engine::PhysicsSettings;
        using engine::PhysicsSystem;
        using engine::RigidBodyComponent;

        // A Player-layer cube drops toward an Enemy-layer platform over
        // an Environment-layer floor. Vary the two masks to prove that
        // filtering (either direction) makes the cube fall through the
        // platform and rest on the floor, while full masks land it on
        // the platform. Rest heights: floor top y=0 -> cube centre
        // ~0.50; platform top y=2.25 -> cube centre ~2.75.
        struct DropResult {
            float y = 0.0f;
            float speed = 0.0f;
            PhysicsSystem::Stats stats{};
        };
        const auto drop = [&](uint32_t cubeMask, uint32_t platformMask) -> DropResult {
            engine::Scene scene;
            auto floor = scene.createEntity("DropFloor");
            floor.addComponent<TransformComponent>(glm::vec3{0.0f, -0.5f, 0.0f});
            floor.addComponent<RigidBodyComponent>().setStatic(true);
            auto& floorCollider =
                floor.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{5.0f, 0.5f, 5.0f}));
            floorCollider.layer = LEnvironment;
            auto platform = scene.createEntity("DropPlatform");
            platform.addComponent<TransformComponent>(glm::vec3{0.0f, 2.0f, 0.0f});
            platform.addComponent<RigidBodyComponent>().setStatic(true);
            auto& platformCollider =
                platform.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{2.0f, 0.25f, 2.0f}));
            platformCollider.layer = LEnemy;
            platformCollider.mask = platformMask;
            auto cube = scene.createEntity("DropCube");
            cube.addComponent<TransformComponent>(glm::vec3{0.0f, 4.0f, 0.0f});
            auto& cubeBody = cube.addComponent<RigidBodyComponent>(1.0f);
            auto& cubeCollider =
                cube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
            cubeCollider.layer = LPlayer;
            cubeCollider.mask = cubeMask;

            PhysicsSettings settings;
            settings.gravity = glm::vec3{0.0f, -10.0f, 0.0f};
            PhysicsSystem physics;
            for (int step = 0; step < 150; ++step) {
                physics.update(scene, 1.0 / 60.0, settings);
            }
            return DropResult{cube.getComponent<TransformComponent>().position.y,
                              glm::length(cubeBody.velocity), physics.lastStats()};
        };

        const auto [filteredY, filteredSpeed, filteredStats] = drop(LAll & ~LEnemy, LAll);
        check(filteredY > 0.40f && filteredY < 0.58f && filteredSpeed < 0.1f &&
                  filteredStats.candidatePairs == 1 && filteredStats.contacts > 0,
              "cube masked against the platform's layer falls through it and rests on the floor");
        const auto [oneWayY, oneWaySpeed, oneWayStats] = drop(LAll, LAll & ~LPlayer);
        check(oneWayY > 0.40f && oneWayY < 0.58f && oneWaySpeed < 0.1f && oneWayStats.candidatePairs == 1,
              "platform masked against the cube's layer also lets it through (mutual rule)");
        const auto [controlY, controlSpeed, controlStats] = drop(LAll, LAll);
        check(controlY > 2.65f && controlY < 2.85f && controlSpeed < 0.1f &&
                  controlStats.candidatePairs == 1 && controlStats.contacts > 0,
              "with matching masks the same cube lands and rests on the platform");
    }

    // --- Raycast layer filtering ------------------------------------------------------------------
    {
        engine::Scene scene;
        auto nearCube = scene.createEntity("RayNear");
        nearCube.addComponent<TransformComponent>(glm::vec3{0.0f, 0.0f, -5.0f});
        auto& nearCollider =
            nearCube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        nearCollider.layer = LEnemy;
        auto farCube = scene.createEntity("RayFar");
        farCube.addComponent<TransformComponent>(glm::vec3{0.0f, 0.0f, -10.0f});
        farCube.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f})); // Default layer
        auto meshOnly = scene.createEntity("RayMeshOnly");
        meshOnly.addComponent<TransformComponent>(glm::vec3{0.0f, 0.0f, -15.0f});
        meshOnly.addComponent<engine::MeshComponent>(); // no collider: answers on the Default layer

        const Ray axis{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}};
        RaycastHit hit;
        check(scene.raycast(axis, hit, 1000.0f) && hit.entity == nearCube.handle(),
              "unfiltered raycast picks the closest cube as before");
        check(scene.raycast(axis, hit, 1000.0f, LEnemy) && hit.entity == nearCube.handle() &&
                  nearlyEqual(hit.distance, 4.5, 1e-4),
              "raycast restricted to the Enemy layer picks the enemy cube");
        check(scene.raycast(axis, hit, 1000.0f, LDefault) && hit.entity == farCube.handle() &&
                  nearlyEqual(hit.distance, 9.5, 1e-4),
              "raycast restricted to Default skips the enemy cube and picks the one behind");
        check(!scene.raycast(axis, hit, 1000.0f, LPlayer | LTrigger),
              "raycast matching no present layer hits nothing");
        std::vector<RaycastHit> hits;
        check(scene.raycastAll(axis, hits, 1000.0f, LDefault) == 2 && hits[0].entity == farCube.handle() &&
                  hits[1].entity == meshOnly.handle() && nearlyEqual(hits[1].distance, 14.5, 1e-4),
              "raycastAll filters by layer; collider-less meshes answer on Default");
        check(scene.raycastAll(axis, hits, 1000.0f, LNone) == 0, "an empty layer mask hits nothing");
        const engine::Scene::MeshResolver noResolver = [](std::string_view) { return nullptr; };
        check(scene.raycastPrecise(axis, hit, noResolver, 1000.0f, LDefault) &&
                  hit.entity == farCube.handle(),
              "precise raycast honours the same layer filter");
    }
}

void testJointConstraints() {
    std::cout << "Physics constraints & joints (Phase 16)\n";
    using engine::ColliderComponent;
    using engine::JointComponent;
    using engine::JointType;
    using engine::PhysicsSettings;
    using engine::PhysicsSystem;
    using engine::RigidBodyComponent;
    using engine::TransformComponent;

    // --- Factories and defaults -------------------------------------------------
    {
        const JointComponent distance =
            JointComponent::distance(static_cast<entt::entity>(3), glm::vec3{1.0f, 0.0f, 0.0f},
                                     glm::vec3{-1.0f, 0.0f, 0.0f}, 2.5f);
        const JointComponent hinge = JointComponent::hinge(entt::null, glm::vec3{0.0f}, glm::vec3{0.0f},
                                                           glm::vec3{0.0f, 0.0f, 1.0f});
        const JointComponent fixed = JointComponent::fixed(static_cast<entt::entity>(4), glm::vec3{0.5f},
                                                           glm::vec3{-0.5f});
        const JointComponent spring = JointComponent::springDamper(entt::null, glm::vec3{0.0f}, glm::vec3{0.0f},
                                                                   1.0f, 50.0f, 2.0f);
        check(distance.type == JointType::Distance && distance.restLength == 2.5f &&
                  hinge.type == JointType::Hinge && fixed.type == JointType::Fixed &&
                  spring.type == JointType::SpringDamper && spring.stiffness == 50.0f,
              "joint factories configure their type and parameters");
        check(distance.minDistance < 0.0f && distance.maxDistance < 0.0f && !distance.enableLimits &&
                  distance.compliance == 0.0f && distance.enabled,
              "joint defaults: unbounded, rigid, limits off");
    }

    // --- Distance joint: exact radius under gravity, explicit and captured ---------
    {
        engine::Scene scene;
        auto bob = scene.createEntity("DistanceBob");
        bob.addComponent<TransformComponent>(glm::vec3{2.0f, 0.0f, 0.0f});
        bob.addComponent<RigidBodyComponent>(1.0f);
        bob.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        bob.addComponent<JointComponent>(
            JointComponent::distance(entt::null, glm::vec3{0.0f}, glm::vec3{0.0f}, 2.0f));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f, -10.0f, 0.0f};
        PhysicsSystem physics;
        bool radiusHeld = true;
        for (int step = 0; step < 180; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
            if (step % 15 == 0) {
                const float distance = glm::length(bob.getComponent<TransformComponent>().position);
                radiusHeld = radiusHeld && distance > 1.98f && distance < 2.02f;
            }
        }
        check(radiusHeld, "distance joint holds an exact 2 m radius through a gravity swing");
    }
    {
        engine::Scene scene;
        auto bob = scene.createEntity("CaptureBob");
        bob.addComponent<TransformComponent>(glm::vec3{0.0f, -1.5f, 0.0f});
        bob.addComponent<RigidBodyComponent>(1.0f);
        bob.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        auto joint = JointComponent::distance(entt::null, glm::vec3{0.0f}, glm::vec3{0.0f}, -1.0f);
        bob.addComponent<JointComponent>(joint); // restLength captured at first sight: 1.5
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f, -10.0f, 0.0f};
        PhysicsSystem physics;
        for (int step = 0; step < 120; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        const float distance = glm::length(bob.getComponent<TransformComponent>().position);
        check(distance > 1.47f && distance < 1.53f,
              "negative rest length captures the initial separation once and holds it");
    }

    // --- Bounded distance: rope slack, then taut; strut push-apart -------------------
    {
        engine::Scene scene;
        auto bob = scene.createEntity("RopeBob");
        bob.addComponent<TransformComponent>(glm::vec3{0.0f, -0.5f, 0.0f});
        auto& body = bob.addComponent<RigidBodyComponent>(1.0f);
        bob.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        auto joint = JointComponent::distance(entt::null, glm::vec3{0.0f}, glm::vec3{0.0f}, -1.0f);
        joint.maxDistance = 2.0f;
        bob.addComponent<JointComponent>(joint);
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f, -10.0f, 0.0f};
        PhysicsSystem physics;
        for (int step = 0; step < 15; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        const float slackDistance = glm::length(bob.getComponent<TransformComponent>().position);
        for (int step = 0; step < 165; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        const auto& transform = bob.getComponent<TransformComponent>();
        const float tautDistance = glm::length(transform.position);
        check(slackDistance < 1.0f, "rope joint is slack while the body is inside max distance");
        check(tautDistance > 1.97f && tautDistance < 2.03f && transform.position.y < -1.9f &&
                  glm::length(body.velocity) < 0.3f,
              "rope joint catches at max distance and hangs");
    }
    {
        engine::Scene scene;
        auto a = scene.createEntity("StrutA");
        a.addComponent<TransformComponent>(glm::vec3{-0.25f, 0.0f, 0.0f});
        a.addComponent<RigidBodyComponent>(1.0f);
        a.addComponent<ColliderComponent>(ColliderComponent::sphere(0.2f));
        auto b = scene.createEntity("StrutB");
        b.addComponent<TransformComponent>(glm::vec3{0.25f, 0.0f, 0.0f});
        auto& bodyB = b.addComponent<RigidBodyComponent>(1.0f);
        b.addComponent<ColliderComponent>(ColliderComponent::sphere(0.2f));
        auto joint = JointComponent::distance(b.handle(), glm::vec3{0.0f}, glm::vec3{0.0f}, -1.0f);
        joint.minDistance = 1.5f;
        a.addComponent<JointComponent>(joint);
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        float minDistanceAfterEngage = 10.0f;
        for (int step = 0; step < 90; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
            if (step >= 5) {
                minDistanceAfterEngage =
                    std::min(minDistanceAfterEngage,
                             glm::length(b.getComponent<TransformComponent>().position -
                                         a.getComponent<TransformComponent>().position));
            }
        }
        const float distance = glm::length(b.getComponent<TransformComponent>().position -
                                           a.getComponent<TransformComponent>().position);
        // A min-distance bound is unilateral: it pushes the bodies
        // apart and never lets them re-approach under it, but in
        // zero-G they keep whatever modest separation speed remains.
        check(distance > 1.45f && minDistanceAfterEngage > 1.40f && glm::length(bodyB.velocity) < 2.5f,
              "min-distance strut pushes bodies apart to the bound and never lets them back under it");
    }

    // --- Hinge: revolute pendulum ------------------------------------------------------
    {
        engine::Scene scene;
        auto bob = scene.createEntity("HingeBob");
        bob.addComponent<TransformComponent>(glm::vec3{1.0f, 0.0f, 0.0f});
        auto& body = bob.addComponent<RigidBodyComponent>(1.0f);
        bob.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        bob.addComponent<JointComponent>(JointComponent::hinge(entt::null, glm::vec3{-1.0f, 0.0f, 0.0f},
                                                               glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f, -10.0f, 0.0f};
        PhysicsSystem physics;
        float minY = 0.0f;
        float maxGap = 0.0f;
        for (int step = 0; step < 240; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
            const auto& transform = bob.getComponent<TransformComponent>();
            minY = std::min(minY, transform.position.y);
            maxGap = std::max(maxGap, glm::length(transform.position +
                                                  body.orientation * glm::vec3{-1.0f, 0.0f, 0.0f}));
        }
        const auto& transform = bob.getComponent<TransformComponent>();
        const float radius = glm::length(transform.position);
        const float axisDot = (body.orientation * glm::vec3{0.0f, 0.0f, 1.0f}).z;
        check(maxGap < 0.01f && radius > 0.98f && radius < 1.02f,
              "hinge anchor stays coincident and the bob keeps its swing radius");
        check(minY < -0.95f && axisDot > 0.99f,
              "hinge pendulum swings through the bottom rotating only about the hinge axis");
    }

    // --- Hinge limits ---------------------------------------------------------------------
    {
        engine::Scene scene;
        auto bob = scene.createEntity("LimitBob");
        bob.addComponent<TransformComponent>(glm::vec3{0.0f, -1.0f, 0.0f});
        auto& body = bob.addComponent<RigidBodyComponent>(1.0f);
        bob.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        auto joint = JointComponent::hinge(entt::null, glm::vec3{0.0f, 1.0f, 0.0f}, glm::vec3{0.0f},
                                           glm::vec3{0.0f, 0.0f, 1.0f});
        joint.enableLimits = true;
        joint.minAngleDegrees = -30.0f;
        joint.maxAngleDegrees = 30.0f;
        bob.addComponent<JointComponent>(joint);
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f, -10.0f, 0.0f};
        PhysicsSystem physics;
        physics.update(scene, 1.0 / 60.0, settings); // captures the limit zero at the bottom
        // Teleport the bob to -80 degrees, rotated about the hinge point.
        auto& transform = bob.getComponent<TransformComponent>();
        transform.position = glm::vec3{-0.9848f, -0.17365f, 0.0f};
        body.orientation = glm::angleAxis(glm::radians(-80.0f), glm::vec3{0.0f, 0.0f, 1.0f});
        body.orientationInitialized = true;
        body.velocity = glm::vec3{0.0f};
        body.angularVelocity = glm::vec3{0.0f};
        body.angularMomentum = glm::vec3{0.0f};
        transform.setOrientation(body.orientation);
        float maxAbsTheta = 0.0f;
        float minTheta = 0.0f;
        for (int step = 0; step < 240; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
            if (step < 30) {
                continue; // the release correction transient is not the limit's steady behaviour
            }
            const float theta = glm::degrees(std::atan2(transform.position.x, -transform.position.y));
            maxAbsTheta = std::max(maxAbsTheta, std::abs(theta));
            minTheta = std::min(minTheta, theta);
        }
        check(maxAbsTheta < 36.0f && minTheta < -18.0f,
              "hinge limits confine the swing to +-30 degrees while letting it cross the bottom");
    }

    // --- Fixed joint: welded pair keeps zero relative transform ------------------------------
    {
        engine::Scene scene;
        auto floor = scene.createEntity("WeldFloor");
        floor.addComponent<TransformComponent>(glm::vec3{0.0f, -0.5f, 0.0f});
        floor.addComponent<RigidBodyComponent>().setStatic(true);
        floor.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{6.0f, 0.5f, 6.0f}));
        auto a = scene.createEntity("WeldA");
        a.addComponent<TransformComponent>(glm::vec3{0.0f, 4.0f, 0.0f});
        auto& bodyA = a.addComponent<RigidBodyComponent>(1.0f);
        a.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        auto b = scene.createEntity("WeldB");
        b.addComponent<TransformComponent>(glm::vec3{1.3f, 4.0f, 0.0f});
        auto& bodyB = b.addComponent<RigidBodyComponent>(1.0f);
        bodyB.angularVelocity = glm::vec3{0.0f, 0.0f, 120.0f}; // must be cancelled by the weld
        b.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        a.addComponent<JointComponent>(JointComponent::fixed(b.handle(), glm::vec3{0.65f, 0.0f, 0.0f},
                                                             glm::vec3{-0.65f, 0.0f, 0.0f}));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f, -10.0f, 0.0f};
        PhysicsSystem physics;
        for (int step = 0; step < 150; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        const auto& transformA = a.getComponent<TransformComponent>();
        const auto& transformB = b.getComponent<TransformComponent>();
        const glm::vec3 localOffset =
            glm::transpose(glm::mat3_cast(bodyA.orientation)) * (transformB.position - transformA.position);
        const glm::quat relativeOrientation = glm::inverse(bodyA.orientation) * bodyB.orientation;
        const float relativeAngle =
            glm::degrees(2.0f * std::acos(std::min(1.0f, std::abs(relativeOrientation.w))));
        check(std::abs(localOffset.x - 1.3f) < 0.02f && std::abs(localOffset.y) < 0.02f &&
                  std::abs(localOffset.z) < 0.02f && relativeAngle < 2.0f,
              "fixed joint holds zero relative transform through a drop, landing, and an initial spin");
        check(transformA.position.y > 0.44f && transformA.position.y < 0.56f &&
                  glm::length(bodyB.angularVelocity) < 5.0f && glm::length(bodyA.velocity) < 0.2f,
              "welded pair lands as one rigid unit and settles");
    }

    // --- Double pendulum: bounded energy error, links attached ---------------------------------
    {
        engine::Scene scene;
        auto link1 = scene.createEntity("DPLink1");
        link1.addComponent<TransformComponent>(glm::vec3{1.0f, 0.0f, 0.0f});
        auto& body1 = link1.addComponent<RigidBodyComponent>(1.0f);
        link1.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        link1.addComponent<JointComponent>(JointComponent::hinge(entt::null, glm::vec3{-1.0f, 0.0f, 0.0f},
                                                                 glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}));
        auto link2 = scene.createEntity("DPLink2");
        link2.addComponent<TransformComponent>(glm::vec3{3.0f, 0.0f, 0.0f});
        auto& body2 = link2.addComponent<RigidBodyComponent>(1.0f);
        link2.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        link2.addComponent<JointComponent>(JointComponent::hinge(link1.handle(), glm::vec3{-1.0f, 0.0f, 0.0f},
                                                                 glm::vec3{1.0f, 0.0f, 0.0f},
                                                                 glm::vec3{0.0f, 0.0f, 1.0f}));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f, -10.0f, 0.0f};
        settings.subSteps = 8;
        PhysicsSystem physics;
        // Point-mass energy (the small sphere spin term included via
        // I = 0.4 m r^2 = 0.025). Released horizontally: E0 = 0, and
        // the reachable potential range is 40 J.
        const auto energy = [](const RigidBodyComponent& body, const TransformComponent& transform) {
            const glm::vec3 omega = glm::radians(body.angularVelocity);
            return 0.5 * static_cast<double>(glm::dot(body.velocity, body.velocity)) +
                   0.5 * 0.025 * static_cast<double>(glm::dot(omega, omega)) +
                   10.0 * static_cast<double>(transform.position.y);
        };
        double maxDrift = 0.0;
        double maxGain = 0.0;
        double maxKinetic = 0.0;
        double maxGap = 0.0;
        bool finite = true;
        for (int step = 0; step < 180; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
            const auto& transform1 = link1.getComponent<TransformComponent>();
            const auto& transform2 = link2.getComponent<TransformComponent>();
            const double total = energy(body1, transform1) + energy(body2, transform2);
            maxDrift = std::max(maxDrift, std::abs(total));
            maxGain = std::max(maxGain, total);
            maxKinetic = std::max(maxKinetic, 0.5 * static_cast<double>(glm::dot(body1.velocity, body1.velocity) +
                                                                        glm::dot(body2.velocity, body2.velocity)));
            const glm::vec3 gap1 = transform1.position + body1.orientation * glm::vec3{-1.0f, 0.0f, 0.0f};
            const glm::vec3 gap2 = (transform2.position + body2.orientation * glm::vec3{-1.0f, 0.0f, 0.0f}) -
                                   (transform1.position + body1.orientation * glm::vec3{1.0f, 0.0f, 0.0f});
            maxGap = std::max({maxGap, static_cast<double>(glm::length(gap1)),
                               static_cast<double>(glm::length(gap2))});
            finite = finite && std::isfinite(transform1.position.x) && std::isfinite(transform2.position.x);
        }
        check(finite && maxGap < 0.01, "double pendulum hinges stay coincident (no stretch or blow-up)");
        check(maxKinetic > 8.0 && maxGain < 0.5 && maxDrift < 6.0,
              "double pendulum energy is conserved within impulse-solver tolerance (no pump, bounded loss)");
    }

    // --- Islands & waking through joints ---------------------------------------------------------
    {
        engine::Scene scene;
        auto a = scene.createEntity("IslandA");
        a.addComponent<TransformComponent>(glm::vec3{-1.0f, 0.0f, 0.0f});
        a.addComponent<RigidBodyComponent>(1.0f);
        a.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        a.addComponent<JointComponent>(
            JointComponent::hinge(entt::null, glm::vec3{1.0f, 0.0f, 0.0f}, glm::vec3{0.0f},
                                  glm::vec3{0.0f, 0.0f, 1.0f}));
        auto b = scene.createEntity("IslandB");
        b.addComponent<TransformComponent>(glm::vec3{1.0f, 0.0f, 0.0f});
        b.addComponent<RigidBodyComponent>(1.0f);
        b.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        b.addComponent<JointComponent>(
            JointComponent::distance(a.handle(), glm::vec3{-1.0f, 0.0f, 0.0f}, glm::vec3{1.0f, 0.0f, 0.0f}, 2.0f));
        auto lone = scene.createEntity("IslandLone");
        lone.addComponent<TransformComponent>(glm::vec3{20.0f, 0.0f, 0.0f});
        lone.addComponent<RigidBodyComponent>(1.0f);
        lone.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        const PhysicsSystem::Stats stats = physics.update(scene, 1.0 / 60.0, settings);
        check(stats.joints == 2 && stats.islands == 1 && stats.largestIsland == 2,
              "joint-linked bodies form one island; the unconnected body forms none");
    }
    {
        engine::Scene scene;
        auto a = scene.createEntity("WakeA");
        a.addComponent<TransformComponent>(glm::vec3{-1.0f, 0.0f, 0.0f});
        auto& bodyA = a.addComponent<RigidBodyComponent>(1.0f);
        bodyA.velocity = glm::vec3{1.5f, 0.0f, 0.0f};
        a.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        auto b = scene.createEntity("WakeB");
        b.addComponent<TransformComponent>(glm::vec3{1.0f, 0.0f, 0.0f});
        auto& bodyB = b.addComponent<RigidBodyComponent>(1.0f);
        bodyB.sleeping = true;
        b.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
        a.addComponent<JointComponent>(
            JointComponent::distance(b.handle(), glm::vec3{0.0f}, glm::vec3{0.0f}, -1.0f));
        PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        PhysicsSystem physics;
        physics.update(scene, 1.0 / 60.0, settings);
        const bool woke = !bodyB.sleeping;
        for (int step = 0; step < 60; ++step) {
            physics.update(scene, 1.0 / 60.0, settings);
        }
        check(woke && bodyB.velocity.x > 0.05f, "an awake joint partner wakes and drags a sleeping body");
    }

    // --- Compliance: soft joints stretch measurably more, still converge ------------------------
    {
        const auto dropStretch = [](float compliance) {
            engine::Scene scene;
            auto bob = scene.createEntity("ComplianceBob");
            bob.addComponent<TransformComponent>(glm::vec3{0.0f, 0.0f, 0.0f});
            bob.addComponent<RigidBodyComponent>(1.0f);
            bob.addComponent<ColliderComponent>(ColliderComponent::sphere(0.25f));
            auto joint = JointComponent::distance(entt::null, glm::vec3{0.0f}, glm::vec3{0.0f}, 2.0f);
            joint.compliance = compliance;
            bob.addComponent<JointComponent>(joint);
            PhysicsSettings settings;
            settings.gravity = glm::vec3{0.0f, -10.0f, 0.0f};
            PhysicsSystem physics;
            for (int step = 0; step < 300; ++step) {
                physics.update(scene, 1.0 / 60.0, settings);
            }
            return std::abs(glm::length(bob.getComponent<TransformComponent>().position) - 2.0f);
        };
        const float rigidError = dropStretch(0.0f);
        const float softError = dropStretch(0.95f);
        check(rigidError < 0.001f && softError > rigidError + 0.0005f && softError < 0.01f,
              "compliance softens the joint measurably while it still converges to rest length");
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

void testEventBus() {
    std::cout << "Event bus (Phase 17)\n";
    using engine::EventBus;
    struct AlphaEvent {
        int value = 0;
    };
    struct BetaEvent {
        float value = 0.0f;
    };

    {
        EventBus bus;
        std::vector<int> order;
        bus.subscribe<AlphaEvent>([&order](const AlphaEvent& event) { order.push_back(10 + event.value); });
        bus.subscribe<AlphaEvent>([&order](const AlphaEvent& event) { order.push_back(20 + event.value); });
        bus.publish(AlphaEvent{1});
        check(order == std::vector<int>({11, 21}), "listeners fire in subscription order with the payload");
        check(bus.listenerCount<AlphaEvent>() == 2 && bus.listenerCount<BetaEvent>() == 0,
              "listener counts are tracked per event type");
    }
    {
        EventBus bus;
        int alphaHits = 0;
        int betaHits = 0;
        bus.subscribe<AlphaEvent>([&alphaHits](const AlphaEvent&) { ++alphaHits; });
        bus.subscribe<BetaEvent>([&betaHits](const BetaEvent&) { ++betaHits; });
        bus.publish(AlphaEvent{});
        check(alphaHits == 1 && betaHits == 0, "event types are isolated from each other");
        bus.publish(BetaEvent{});
        check(alphaHits == 1 && betaHits == 1, "each type dispatches independently");
    }
    {
        EventBus bus;
        int hits = 0;
        const EventBus::SubscriptionId first =
            bus.subscribe<AlphaEvent>([&hits](const AlphaEvent&) { ++hits; });
        bus.subscribe<AlphaEvent>([&hits](const AlphaEvent&) { ++hits; });
        bus.publish(AlphaEvent{});
        bus.unsubscribe(first);
        bus.publish(AlphaEvent{});
        check(hits == 3 && bus.listenerCount<AlphaEvent>() == 1,
              "unsubscribe(id) removes exactly that listener");
        bus.unsubscribe(first);
        check(bus.listenerCount<AlphaEvent>() == 1, "unsubscribing a dead id is a no-op");
    }
    {
        EventBus bus;
        int hits = 0;
        {
            const EventBus::Subscription scoped =
                bus.subscribeScoped<AlphaEvent>([&hits](const AlphaEvent&) { ++hits; });
            bus.publish(AlphaEvent{});
        }
        bus.publish(AlphaEvent{});
        check(hits == 1 && bus.listenerCount<AlphaEvent>() == 0,
              "RAII subscription unsubscribes when its handle leaves scope");
    }
    {
        EventBus bus;
        bus.publish(AlphaEvent{5});
        int hits = 0;
        bus.subscribe<AlphaEvent>([&hits](const AlphaEvent&) { ++hits; });
        check(hits == 0, "late subscribers do not receive past events (no replay)");
        bus.clear();
        check(bus.listenerCount<AlphaEvent>() == 0, "clear() drops every subscription");
    }
    {
        EventBus bus;
        int first = 0;
        int second = 0;
        EventBus::SubscriptionId secondId = EventBus::kInvalidSubscription;
        bus.subscribe<AlphaEvent>([&bus, &first, &secondId](const AlphaEvent&) {
            ++first;
            bus.unsubscribe(secondId);
        });
        secondId = bus.subscribe<AlphaEvent>([&second](const AlphaEvent&) { ++second; });
        bus.publish(AlphaEvent{});
        bus.publish(AlphaEvent{});
        check(first == 2 && second == 1,
              "unsubscribing during dispatch is safe and applies from the next publish");
    }
}

void testInputSystem() {
    std::cout << "Input system: state transitions and mappings (Phase 17)\n";
    using engine::Input;
    using engine::Key;
    using engine::MouseButton;

    Input input; // headless instance driven through inject*()
    input.bindAxis("Horizontal", Key::A, Key::D);
    input.bindAction("Jump", Key::Space);
    input.bindActionButton("Jump", MouseButton::Right);

    // Press / held / repeat / release transitions for keys.
    input.beginFrame();
    input.injectKeyState(Key::Space, true);
    check(input.isKeyDown(Key::Space) && input.wasKeyPressed(Key::Space) && !input.wasKeyReleased(Key::Space),
          "injected press: held + pressed edge in the same frame");
    check(input.isActionDown("Jump") && input.wasActionPressed("Jump"),
          "action mirrors its key binding's press");
    input.beginFrame();
    check(input.isKeyDown(Key::Space) && !input.wasKeyPressed(Key::Space),
          "held key produces no new press edge on the next frame");
    input.injectKeyState(Key::Space, true);
    check(!input.wasKeyPressed(Key::Space), "repeated down (auto-repeat) does not re-fire the press edge");
    input.injectKeyState(Key::Space, false);
    check(!input.isKeyDown(Key::Space) && input.wasKeyReleased(Key::Space) && input.wasActionReleased("Jump"),
          "release: up + released edge, action released with it");
    input.beginFrame();
    check(!input.wasKeyReleased(Key::Space), "beginFrame clears per-frame transition edges");

    // Mouse buttons, cursor delta, scroll.
    input.beginFrame();
    input.injectMouseButtonState(MouseButton::Left, true);
    input.injectCursorPosition(glm::vec2{100.0f, 50.0f});
    check(input.mouseDelta() == glm::vec2{0.0f, 0.0f}, "first cursor sample only sets the baseline");
    input.injectCursorPosition(glm::vec2{110.0f, 40.0f});
    input.injectScrollDelta(glm::vec2{0.0f, 1.0f});
    input.injectMouseButtonState(MouseButton::Right, true);
    check(input.isMouseButtonDown(MouseButton::Left) && input.wasMouseButtonPressed(MouseButton::Left),
          "mouse button press edge tracked");
    check(input.mouseDelta() == glm::vec2{10.0f, -10.0f} && input.scrollDelta() == glm::vec2{0.0f, 1.0f},
          "cursor delta and scroll accumulate within the frame");
    check(input.isActionDown("Jump"), "action also answers to its mouse-button binding");
    input.beginFrame();
    check(input.mouseDelta() == glm::vec2{0.0f, 0.0f} && input.scrollDelta() == glm::vec2{0.0f, 0.0f} &&
              input.isMouseButtonDown(MouseButton::Left),
          "beginFrame clears deltas but keeps held state");

    // Axes: neutral, both directions, cancellation, rebinding.
    input.beginFrame();
    check(input.axisValue("Horizontal") == 0.0f, "axis reads 0 with neither key down");
    input.injectKeyState(Key::D, true);
    check(input.axisValue("Horizontal") == 1.0f, "axis reads +1 with the positive key down");
    input.injectKeyState(Key::A, true);
    check(input.axisValue("Horizontal") == 0.0f, "opposing axis keys cancel out");
    input.injectKeyState(Key::D, false);
    check(input.axisValue("Horizontal") == -1.0f, "axis reads -1 with only the negative key down");
    input.clearAxisBinding("Horizontal");
    check(input.axisValue("Horizontal") == 0.0f && input.axisValue("NeverBound") == 0.0f &&
              !input.isActionDown("NeverBound"),
          "cleared and never-bound mappings read as neutral");
}

void testPhysicsEvents() {
    std::cout << "Physics & scene events (Phase 17)\n";
    using engine::ColliderComponent;
    using engine::CollisionEvent;
    using engine::EntityDestroyedEvent;
    using engine::EventBus;
    using engine::PhysicsSettings;
    using engine::PhysicsSystem;
    using engine::RigidBodyComponent;
    using engine::TransformComponent;
    using engine::TriggerEvent;

    const auto step = [](PhysicsSystem& physics, engine::Scene& scene, const PhysicsSettings& settings,
                         int steps) {
        for (int i = 0; i < steps; ++i) {
            physics.update(scene, 1.0 / 60.0, settings, nullptr);
        }
    };

    // CollisionEvent: exactly once for a new contact pair, again after
    // a genuine separation + re-collision.
    {
        engine::Scene scene;
        PhysicsSystem physics;
        EventBus bus;
        physics.setEventBus(&bus);
        scene.setEventBus(&bus);
        PhysicsSettings settings;
        settings.subSteps = 4;
        settings.sleepEnabled = false;

        auto floor = scene.createEntity("Floor");
        floor.addComponent<TransformComponent>(glm::vec3{0.0f, 0.0f, 0.0f});
        floor.addComponent<RigidBodyComponent>(0.0f);
        floor.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{10.0f, 0.5f, 10.0f}));
        auto boxEntity = scene.createEntity("Box");
        auto& boxTransform = boxEntity.addComponent<TransformComponent>(glm::vec3{0.0f, 1.5f, 0.0f});
        auto& boxBody = boxEntity.addComponent<RigidBodyComponent>(1.0f);
        boxBody.restitution = 0.0f;
        boxEntity.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));

        int collisions = 0;
        float bestImpulse = 0.0f;
        bool routingOk = true;
        bus.subscribe<CollisionEvent>([&](const CollisionEvent& event) {
            ++collisions;
            bestImpulse = std::max(bestImpulse, event.impulse);
            routingOk &= (event.entityA == floor.handle() || event.entityB == floor.handle()) &&
                         (event.entityA == boxEntity.handle() || event.entityB == boxEntity.handle());
        });
        step(physics, scene, settings, 120);
        check(collisions == 1 && bestImpulse > 0.0f && routingOk,
              "collision-began fires exactly once for a pair that stays in contact, with impulse and entities");

        boxTransform.position.y = 3.0f;
        boxBody.velocity = glm::vec3{0.0f};
        step(physics, scene, settings, 120);
        check(collisions == 2, "collision-began fires again after separation and re-collision");
    }

    // TriggerEvent: enter/exit around a pass-through volume, and the
    // Phase 15 layer/mask filter decides which pairs ever see it.
    {
        engine::Scene scene;
        PhysicsSystem physics;
        EventBus bus;
        physics.setEventBus(&bus);
        PhysicsSettings settings;
        settings.subSteps = 4;
        settings.sleepEnabled = false;

        auto pad = scene.createEntity("Pad");
        pad.addComponent<TransformComponent>(glm::vec3{0.0f, 0.5f, 0.0f});
        pad.addComponent<RigidBodyComponent>(0.0f);
        auto& padCollider = pad.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{2.0f, 0.5f, 2.0f}));
        padCollider.isTrigger = true;
        auto falling = scene.createEntity("Falling");
        auto& fallingTransform = falling.addComponent<TransformComponent>(glm::vec3{0.0f, 3.0f, 0.0f});
        falling.addComponent<RigidBodyComponent>(1.0f);
        falling.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));

        int enters = 0;
        int exits = 0;
        int collisions = 0;
        bool routingOk = true;
        bus.subscribe<TriggerEvent>([&](const TriggerEvent& event) {
            routingOk &= event.triggerEntity == pad.handle() && event.otherEntity == falling.handle();
            if (event.entered) {
                ++enters;
            } else {
                ++exits;
            }
        });
        bus.subscribe<CollisionEvent>([&collisions](const CollisionEvent&) { ++collisions; });
        step(physics, scene, settings, 120);
        check(enters == 1 && exits == 1 && routingOk && collisions == 0 && fallingTransform.position.y < -2.0f,
              "trigger emits one enter + one exit, names the trigger, and bodies fall through it");

        // Mask the pair out (pad only sees layer 0x2): the falling box
        // is on the default layer, so no further trigger events.
        padCollider.layer = 0x2u;
        padCollider.mask = 0x2u;
        fallingTransform.position = glm::vec3{0.0f, 3.0f, 0.0f};
        scene.registry().get<RigidBodyComponent>(falling.handle()).velocity = glm::vec3{0.0f};
        step(physics, scene, settings, 60);
        check(enters == 1 && exits == 1, "trigger pairs filtered out by layer/mask emit no events");
    }

    // EntityDestroyedEvent: published while components still exist.
    {
        engine::Scene scene;
        EventBus bus;
        scene.setEventBus(&bus);
        auto victim = scene.createEntity("Victim");
        victim.addComponent<TransformComponent>(glm::vec3{1.0f, 2.0f, 3.0f});
        bool fired = false;
        bool readableInside = false;
        bus.subscribe<EntityDestroyedEvent>([&](const EntityDestroyedEvent& event) {
            fired = true;
            readableInside = scene.registry().valid(event.entity) &&
                             scene.registry().try_get<TransformComponent>(event.entity) != nullptr &&
                             event.entity == victim.handle();
        });
        scene.destroyEntity(victim);
        check(fired && readableInside && !scene.registry().valid(victim.handle()),
              "destroyed event fires before destruction with components still readable");
    }
}

void testSceneHierarchy() {
    std::cout << "Scene graph & transform hierarchy (Phase 18)\n";
    using engine::ColliderComponent;
    using engine::ParentComponent;
    using engine::PhysicsSettings;
    using engine::PhysicsSystem;
    using engine::RigidBodyComponent;
    using engine::TransformComponent;
    using engine::TransformSystem;
    using engine::WorldTransformComponent;

    const auto worldOf = [](engine::Scene& scene, engine::Entity entity) {
        return scene.registry().get<WorldTransformComponent>(entity.handle());
    };

    // --- Propagation: depth, rotation compounding, scale inheritance ----------
    {
        engine::Scene scene;
        auto root = scene.createEntity("root");
        auto& rootTransform = root.addComponent<TransformComponent>(glm::vec3{1.0f, 0.0f, 0.0f});
        rootTransform.rotationDegrees = glm::vec3{0.0f, 90.0f, 0.0f};
        rootTransform.scale = glm::vec3{2.0f};
        auto child = scene.createEntity("child");
        child.addComponent<TransformComponent>(glm::vec3{1.0f, 0.0f, 0.0f});
        auto grand = scene.createEntity("grand");
        auto& grandTransform = grand.addComponent<TransformComponent>(glm::vec3{0.0f, 1.0f, 0.0f});
        grandTransform.rotationDegrees = glm::vec3{0.0f, 90.0f, 0.0f};
        scene.setParent(child, root);
        scene.setParent(grand, child);
        TransformSystem::update(scene);

        // Child offset (1,0,0) rotated by the parent's 90° yaw and
        // doubled by its scale: (1,0,0) -> (0,0,-2), plus root at x=1.
        check(nearlyVec3(worldOf(scene, child).worldPosition, glm::vec3{1.0f, 0.0f, -2.0f}, 1e-4),
              "child world position composes parent rotation and scale");
        check(nearlyVec3(worldOf(scene, grand).worldPosition, glm::vec3{1.0f, 2.0f, -2.0f}, 1e-4),
              "grandchild world position composes the full chain");
        const glm::vec3 facing = worldOf(scene, grand).worldOrientation * glm::vec3{1.0f, 0.0f, 0.0f};
        check(nearlyVec3(facing, glm::vec3{-1.0f, 0.0f, 0.0f}, 1e-4),
              "two 90-degree yaws compound to a 180-degree world orientation (quaternions, no gimbal lock)");
        check(nearlyVec3(worldOf(scene, grand).worldScale, glm::vec3{2.0f}, 1e-4),
              "world scale inherits the parent's scale");
        const glm::vec3 mapped =
            glm::vec3{worldOf(scene, grand).worldMatrix * glm::vec4{1.0f, 0.0f, 0.0f, 1.0f}};
        check(nearlyVec3(mapped, glm::vec3{-1.0f, 2.0f, -2.0f}, 1e-4),
              "world matrix maps local points through rotation + scale + translation");
        check(nearlyVec3(worldOf(scene, root).worldPosition, glm::vec3{1.0f, 0.0f, 0.0f}, 1e-6) &&
                  root.getComponent<TransformComponent>().modelMatrix() == worldOf(scene, root).worldMatrix,
              "a root's world matrix is exactly its local model matrix");
    }
    {
        // Arbitrary depth: eight 1 m hops straight up.
        engine::Scene scene;
        engine::Entity parent = scene.createEntity("level0");
        parent.addComponent<TransformComponent>(glm::vec3{0.0f});
        engine::Entity deepest = parent;
        for (int level = 1; level <= 8; ++level) {
            engine::Entity next = scene.createEntity("level");
            next.addComponent<TransformComponent>(glm::vec3{0.0f, 1.0f, 0.0f});
            scene.setParent(next, parent);
            parent = next;
            deepest = next;
        }
        const size_t updated = TransformSystem::update(scene);
        check(updated == 9 && nearlyVec3(worldOf(scene, deepest).worldPosition, glm::vec3{0.0f, 8.0f, 0.0f}, 1e-4),
              "propagation is exact through eight hierarchy levels");
    }

    // --- Lifecycle: links, cycles, reparenting, keep-world ---------------------
    {
        engine::Scene scene;
        auto a = scene.createEntity("a");
        a.addComponent<TransformComponent>(glm::vec3{0.0f});
        auto b = scene.createEntity("b");
        b.addComponent<TransformComponent>(glm::vec3{0.0f});
        auto child = scene.createEntity("child");
        child.addComponent<TransformComponent>(glm::vec3{1.0f, 0.0f, 0.0f});

        check(scene.setParent(child, a) && child.hasComponent<ParentComponent>() &&
                  scene.parentOf(child) == a && scene.childrenOf(a).size() == 1 &&
                  scene.childrenOf(a).front() == child,
              "setParent links both sides of the relationship");
        check(scene.isDescendantOf(child, a) && !scene.isDescendantOf(a, child),
              "descendant queries follow the chain direction");

        check(scene.setParent(child, b) && scene.childrenOf(a).empty() && scene.childrenOf(b).size() == 1 &&
                  scene.parentOf(child) == b,
              "reparenting moves the child between child lists atomically");
        check(child.removeParent() && !child.hasComponent<ParentComponent>() && scene.childrenOf(b).empty() &&
                  !child.removeParent(),
              "removeParent detaches to root; a second detach is a no-op");

        scene.setParent(child, a);
        check(!scene.setParent(a, child) && !scene.setParent(a, a) && scene.parentOf(child) == a &&
                  scene.childrenOf(a).size() == 1,
              "cycle-creating and self-parenting links are rejected without side effects");

        // keepWorldTransform: reparent under a rotated, offset parent
        // and the world placement must not move.
        auto rig = scene.createEntity("rig");
        auto& rigTransform = rig.addComponent<TransformComponent>(glm::vec3{10.0f, 0.0f, 0.0f});
        rigTransform.rotationDegrees = glm::vec3{0.0f, 90.0f, 0.0f};
        TransformSystem::update(scene);
        const glm::vec3 worldBefore = worldOf(scene, child).worldPosition;
        scene.setParent(child, rig, true);
        TransformSystem::update(scene);
        check(nearlyVec3(worldOf(scene, child).worldPosition, worldBefore, 1e-4) &&
                  !nearlyVec3(child.getComponent<TransformComponent>().position, glm::vec3{1.0f, 0.0f, 0.0f}, 1e-6),
              "keepWorldTransform reparenting preserves world placement by rewriting the local transform");

        scene.destroyEntity(child);
        check(scene.childrenOf(rig).empty(), "destroying a child unlinks it from its parent's list");
    }

    // --- Lifecycle: destroy modes ----------------------------------------------
    {
        engine::Scene scene;
        engine::EventBus bus;
        scene.setEventBus(&bus);
        int destroyed = 0;
        bus.subscribe<engine::EntityDestroyedEvent>(
            [&destroyed](const engine::EntityDestroyedEvent&) { ++destroyed; });
        auto root = scene.createEntity("root");
        root.addComponent<TransformComponent>(glm::vec3{5.0f, 0.0f, 0.0f});
        auto child = scene.createEntity("child");
        child.addComponent<TransformComponent>(glm::vec3{1.0f, 0.0f, 0.0f});
        auto grand = scene.createEntity("grand");
        grand.addComponent<TransformComponent>(glm::vec3{1.0f, 0.0f, 0.0f});
        scene.setParent(child, root);
        scene.setParent(grand, child);

        scene.destroyEntity(root); // default mode: Orphan
        check(scene.isValid(child) && scene.isValid(grand) && destroyed == 1 &&
                  !child.hasComponent<ParentComponent>() &&
                  nearlyVec3(child.getComponent<TransformComponent>().position, glm::vec3{6.0f, 0.0f, 0.0f}, 1e-4),
              "orphan destroy: children survive at root with world placement preserved, one event");

        engine::Scene cascadeScene;
        engine::EventBus cascadeBus;
        cascadeScene.setEventBus(&cascadeBus);
        int cascadeDestroyed = 0;
        cascadeBus.subscribe<engine::EntityDestroyedEvent>(
            [&cascadeDestroyed](const engine::EntityDestroyedEvent&) { ++cascadeDestroyed; });
        cascadeScene.setHierarchyDestroyMode(engine::HierarchyDestroyMode::Cascade);
        auto cRoot = cascadeScene.createEntity("root");
        cRoot.addComponent<TransformComponent>(glm::vec3{0.0f});
        auto cChild = cascadeScene.createEntity("child");
        cChild.addComponent<TransformComponent>(glm::vec3{0.0f});
        auto cGrand = cascadeScene.createEntity("grand");
        cGrand.addComponent<TransformComponent>(glm::vec3{0.0f});
        cascadeScene.setParent(cChild, cRoot);
        cascadeScene.setParent(cGrand, cChild);
        cascadeScene.destroyEntity(cRoot);
        check(!cascadeScene.isValid(cChild) && !cascadeScene.isValid(cGrand) && cascadeDestroyed == 3 &&
                  cascadeScene.entityCount() == 0,
              "cascade destroy: the whole subtree is destroyed, one event per entity");
    }

    // --- Physics integration ------------------------------------------------------
    const auto stepPhysics = [](PhysicsSystem& physics, engine::Scene& scene, const PhysicsSettings& settings,
                                int steps) {
        for (int i = 0; i < steps; ++i) {
            physics.update(scene, 1.0 / 60.0, settings, nullptr);
        }
    };
    {
        // A static collider carried by a transform-only rig at x=5:
        // a root body dropped over the rig lands on the CHILD's
        // world position, proving colliders inherit world placement.
        engine::Scene scene;
        PhysicsSystem physics;
        PhysicsSettings settings;
        settings.subSteps = 4;
        settings.sleepEnabled = false;

        auto rig = scene.createEntity("rig");
        rig.addComponent<TransformComponent>(glm::vec3{5.0f, 0.0f, 0.0f});
        auto shelf = scene.createEntity("shelf");
        shelf.addComponent<TransformComponent>(glm::vec3{0.0f, 0.5f, 0.0f});
        auto& shelfBody = shelf.addComponent<RigidBodyComponent>(0.0f);
        shelfBody.setStatic(true);
        shelf.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{1.0f, 0.5f, 1.0f}));
        scene.setParent(shelf, rig);

        auto boxEntity = scene.createEntity("box");
        auto& boxTransform = boxEntity.addComponent<TransformComponent>(glm::vec3{5.0f, 3.0f, 0.0f});
        auto& boxBody = boxEntity.addComponent<RigidBodyComponent>(1.0f);
        boxBody.restitution = 0.0f;
        boxEntity.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));

        stepPhysics(physics, scene, settings, 150);
        check(nearlyVec3(boxTransform.position, glm::vec3{5.0f, 1.5f, 0.0f}, 0.05f),
              "a collider child of an offset rig collides at its world position");
    }
    {
        // A dynamic body under a rotated rig: physics converts it to
        // world space, solves, and writes the result back to local.
        engine::Scene scene;
        PhysicsSystem physics;
        PhysicsSettings settings;
        settings.subSteps = 4;
        settings.sleepEnabled = false;

        auto floorEntity = scene.createEntity("floor");
        floorEntity.addComponent<TransformComponent>(glm::vec3{0.0f, -0.5f, 0.0f});
        floorEntity.addComponent<RigidBodyComponent>(0.0f);
        floorEntity.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{10.0f, 0.5f, 10.0f}));

        auto rig = scene.createEntity("rig");
        auto& rigTransform = rig.addComponent<TransformComponent>(glm::vec3{0.0f});
        rigTransform.rotationDegrees = glm::vec3{0.0f, 90.0f, 0.0f};
        auto boxEntity = scene.createEntity("box");
        auto& boxTransform = boxEntity.addComponent<TransformComponent>(glm::vec3{1.0f, 3.0f, 0.0f});
        auto& boxBody = boxEntity.addComponent<RigidBodyComponent>(1.0f);
        boxBody.restitution = 0.0f;
        boxEntity.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        scene.setParent(boxEntity, rig);

        TransformSystem::update(scene);
        check(nearlyVec3(worldOf(scene, boxEntity).worldPosition, glm::vec3{0.0f, 3.0f, -1.0f}, 1e-4),
              "parented body starts at the rotated rig's world position");
        stepPhysics(physics, scene, settings, 150);
        TransformSystem::update(scene);
        check(nearlyVec3(worldOf(scene, boxEntity).worldPosition, glm::vec3{0.0f, 0.5f, -1.0f}, 0.05f) &&
                  nearlyVec3(boxTransform.position, glm::vec3{1.0f, 0.5f, 0.0f}, 0.05f),
              "parented body falls in world space and is written back to exact local placement");

        // Moving the parent carries the resting body with it.
        rigTransform.position = glm::vec3{3.0f, 0.0f, 0.0f};
        stepPhysics(physics, scene, settings, 30);
        TransformSystem::update(scene);
        check(nearlyVec3(worldOf(scene, boxEntity).worldPosition, glm::vec3{3.0f, 0.5f, -1.0f}, 0.05f),
              "moving a parent rig carries its child bodies along");
    }

    // --- Picking integration -------------------------------------------------------
    {
        engine::Scene scene;
        auto parent = scene.createEntity("parent");
        parent.addComponent<TransformComponent>(glm::vec3{10.0f, 0.0f, 0.0f});
        auto child = scene.createEntity("childMesh");
        child.addComponent<TransformComponent>(glm::vec3{0.0f, 0.0f, -5.0f});
        auto& mesh = child.addComponent<engine::MeshComponent>();
        mesh.localBounds = engine::AABB{glm::vec3{-0.5f}, glm::vec3{0.5f}};
        scene.setParent(child, parent);
        TransformSystem::update(scene);

        engine::RaycastHit hit;
        check(scene.raycast(engine::Ray{{10.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, hit) &&
                  hit.entity == child.handle() && nearlyEqual(hit.distance, 4.5, 1e-3),
              "raycast picks a parented mesh at its world position");
        check(!scene.raycast(engine::Ray{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f}}, hit),
              "raycast does not pick the mesh at its local position");
    }
}

void testSceneSerialization() {
    std::cout << "Scene serialization (Phase 19)\n";
    using engine::CameraComponent;
    using engine::ColliderComponent;
    using engine::GravityFieldComponent;
    using engine::JointComponent;
    using engine::JointType;
    using engine::MeshComponent;
    using engine::ParentComponent;
    using engine::PhysicsSettings;
    using engine::PhysicsSystem;
    using engine::RigidBodyComponent;
    using engine::SpinComponent;
    using engine::ThrusterComponent;
    using engine::TransformComponent;

    const auto findByTag = [](engine::Scene& scene, const std::string& tag) -> engine::Entity {
        auto& registry = scene.registry();
        for (const entt::entity entity : registry.view<engine::TagComponent>()) {
            if (registry.get<engine::TagComponent>(entity).tag == tag) {
                return engine::Entity{entity, &scene};
            }
        }
        return engine::Entity{};
    };
    const auto exactVec3 = [](const glm::vec3& a, const glm::vec3& b) { return a == b; };

    // --- Build a scene exercising every serialized component -------------------
    engine::Scene scene;
    scene.setHierarchyDestroyMode(engine::HierarchyDestroyMode::Cascade);

    auto rig = scene.createEntity("Rig");
    auto& rigTransform = rig.addComponent<TransformComponent>(glm::vec3{1.5f, -2.25f, 3.75f});
    rigTransform.rotationDegrees = glm::vec3{10.0f, 45.0f, -30.0f};
    rigTransform.scale = glm::vec3{1.5f, 0.5f, 2.0f};

    auto crate = scene.createEntity("Crate");
    auto& crateTransform = crate.addComponent<TransformComponent>(glm::vec3{0.1f, 2.0f, -0.3f});
    crateTransform.setOrientation(glm::quat{0.9239f, 0.1f, 0.2f, 0.3f});
    auto& crateBody = crate.addComponent<RigidBodyComponent>(2.5f);
    crateBody.velocity = glm::vec3{1.0f, -2.0f, 3.5f};
    crateBody.restitution = 0.123f;
    crateBody.friction = 0.987f;
    crateBody.anisotropicFriction = glm::vec2{0.11f, 0.22f};
    crateBody.gravityScale = 0.5f;
    crateBody.allowSleep = false;
    crateBody.setPrincipalInertia(glm::vec3{3.0f, 4.0f, 5.0f});
    auto& crateCollider =
        crate.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.4f, 0.5f, 0.6f},
                                                                    glm::vec3{0.01f, 0.02f, 0.03f}));
    crateCollider.layer = 0x4u;
    crateCollider.mask = 0x9u;
    auto& crateMesh = crate.addComponent<MeshComponent>();
    crateMesh.assetId = "crateMesh";
    crateMesh.materialName = "woodMat";
    crateMesh.visible = false;
    crateMesh.localBounds = engine::AABB{glm::vec3{-1.0f, -2.0f, -3.0f}, glm::vec3{4.0f, 5.0f, 6.0f}};
    scene.setParent(crate, rig);

    auto ball = scene.createEntity("Ball");
    ball.addComponent<TransformComponent>(glm::vec3{4.0f, 2.0f, 0.0f});
    ball.addComponent<RigidBodyComponent>(1.0f);
    ball.addComponent<ColliderComponent>(ColliderComponent::sphere(0.75f));

    auto linkA = scene.createEntity("LinkA");
    linkA.addComponent<TransformComponent>(glm::vec3{0.0f, 5.0f, 0.0f});
    linkA.addComponent<RigidBodyComponent>(1.0f);
    linkA.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
    auto linkB = scene.createEntity("LinkB");
    linkB.addComponent<TransformComponent>(glm::vec3{2.0f, 5.0f, 0.0f});
    linkB.addComponent<RigidBodyComponent>(1.0f);
    linkB.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
    linkA.addComponent<JointComponent>(
        JointComponent::distance(linkB.handle(), glm::vec3{0.0f}, glm::vec3{0.0f}, 2.0f));

    auto hingeLink = scene.createEntity("HingeLink");
    hingeLink.addComponent<TransformComponent>(glm::vec3{0.0f});
    auto& hinge = hingeLink.addComponent<JointComponent>(JointComponent::hinge(
        crate.handle(), glm::vec3{0.5f, 0.0f, 0.0f}, glm::vec3{-0.5f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}));
    hinge.enableLimits = true;
    hinge.minAngleDegrees = -30.0f;
    hinge.maxAngleDegrees = 45.0f;
    hinge.compliance = 0.25f;

    auto springLink = scene.createEntity("SpringLink");
    springLink.addComponent<TransformComponent>(glm::vec3{0.0f});
    springLink.addComponent<JointComponent>(JointComponent::springDamper(
        entt::null, glm::vec3{0.0f}, glm::vec3{1.0f, 5.0f, 1.0f}, 2.0f, 42.0f, 3.5f));

    auto pad = scene.createEntity("Pad");
    pad.addComponent<TransformComponent>(glm::vec3{0.0f});
    auto& padCollider = pad.addComponent<ColliderComponent>(ColliderComponent::capsule(0.3f, 0.7f));
    padCollider.isTrigger = true;
    padCollider.layer = 0x10u;

    auto hullBody = scene.createEntity("HullBody");
    hullBody.addComponent<TransformComponent>(glm::vec3{-3.0f, 1.0f, 0.0f});
    hullBody.addComponent<RigidBodyComponent>(1.5f);
    auto& hull = hullBody.addComponent<ColliderComponent>();
    hull.type = ColliderComponent::Type::ConvexHull;
    hull.hullPoints = {glm::vec3{0.0f, 0.0f, 0.0f}, glm::vec3{1.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 1.0f, 0.0f},
                       glm::vec3{0.0f, 0.0f, 1.0f}};
    hull.hullPointCount = 4;

    auto ground = scene.createEntity("Ground");
    ground.addComponent<TransformComponent>(glm::vec3{0.0f, -1.0f, 0.0f});
    auto& groundBody = ground.addComponent<RigidBodyComponent>(0.0f);
    groundBody.setStatic(true);
    auto& groundCollider = ground.addComponent<ColliderComponent>();
    groundCollider.type = ColliderComponent::Type::Plane;
    groundCollider.planeNormal = glm::vec3{0.0f, 1.0f, 0.0f};
    groundCollider.planeOffset = -1.0f;

    auto cam = scene.createEntity("Cam");
    cam.addComponent<TransformComponent>(glm::vec3{0.0f});
    engine::CameraConfig cameraConfig;
    cameraConfig.movementSpeed = 7.5f;
    cameraConfig.fieldOfViewDegrees = 60.0f;
    auto& cameraComponent = cam.addComponent<CameraComponent>(cameraConfig);
    cameraComponent.camera.setPosition(glm::vec3{9.0f, 8.0f, 7.0f});
    cameraComponent.camera.setOrientation(-45.0f, 15.0f);

    auto spinner = scene.createEntity("Spinner");
    spinner.addComponent<TransformComponent>(glm::vec3{0.0f});
    spinner.addComponent<SpinComponent>(glm::vec3{0.0f, 90.0f, 0.0f});

    auto planet = scene.createEntity("Planet");
    planet.addComponent<TransformComponent>(glm::vec3{0.0f, 0.0f, -20.0f});
    auto& gravity = planet.addComponent<GravityFieldComponent>();
    gravity.gravitationalParameter = 123.5f;
    gravity.radius = 50.0f;

    auto drone = scene.createEntity("Drone");
    drone.addComponent<TransformComponent>(glm::vec3{1.0f, 1.0f, 1.0f});
    drone.addComponent<RigidBodyComponent>(3.0f);
    auto& thruster = drone.addComponent<ThrusterComponent>();
    thruster.force = glm::vec3{0.0f, 25.0f, 0.0f};
    thruster.localPosition = glm::vec3{0.0f, -0.5f, 0.0f};
    thruster.throttle = 0.75f;

    const std::string document = scene.saveToString();
    check(document.find("\"format\": \"engine-scene\"") != std::string::npos &&
              document.find("\"Crate\"") != std::string::npos,
          "saved document is tagged JSON carrying the scene's entities");

    // --- Round-trip into a scene that must be cleared first -----------------------
    engine::Scene loaded;
    loaded.createEntity("Junk");
    check(loaded.loadFromString(document) && loaded.entityCount() == scene.entityCount(),
          "load replaces the previous scene wholesale and restores every entity");
    check(loaded.hierarchyDestroyMode() == engine::HierarchyDestroyMode::Cascade,
          "hierarchy destroy mode round-trips");

    engine::Entity lRig = findByTag(loaded, "Rig");
    engine::Entity lCrate = findByTag(loaded, "Crate");
    const TransformComponent& lCrateTransform = lCrate.getComponent<TransformComponent>();
    check(exactVec3(lCrateTransform.position, crateTransform.position) &&
              exactVec3(lCrateTransform.scale, crateTransform.scale) &&
              lCrateTransform.orientation == crateTransform.orientation &&
              lCrateTransform.useQuaternionOrientation == crateTransform.useQuaternionOrientation,
          "transform (position, scale, quaternion orientation) is bit-exact after round-trip");
    check(lCrate.hasComponent<ParentComponent>() &&
              lCrate.getComponent<ParentComponent>().parent == lRig.handle() &&
              loaded.childrenOf(lRig).size() == 1,
          "hierarchy is rebuilt with both link sides consistent");

    const RigidBodyComponent& lBody = lCrate.getComponent<RigidBodyComponent>();
    check(lBody.mass == 2.5f && lBody.inverseMass == crateBody.inverseMass && lBody.velocity == crateBody.velocity &&
              lBody.restitution == 0.123f && lBody.friction == 0.987f &&
              lBody.anisotropicFriction == glm::vec2{0.11f, 0.22f} && lBody.gravityScale == 0.5f &&
              !lBody.autoInertia && lBody.inertiaTensor[1][1] == 4.0f && !lBody.allowSleep,
          "rigid body state and material fields are bit-exact after round-trip");

    const ColliderComponent& lCollider = lCrate.getComponent<ColliderComponent>();
    check(lCollider.type == ColliderComponent::Type::Box && lCollider.halfExtents == glm::vec3{0.4f, 0.5f, 0.6f} &&
              lCollider.centerOffset == glm::vec3{0.01f, 0.02f, 0.03f} && lCollider.layer == 0x4u &&
              lCollider.mask == 0x9u && !lCollider.isTrigger,
          "collider shape, offset, layer and mask are bit-exact after round-trip");

    const MeshComponent& lMesh = lCrate.getComponent<MeshComponent>();
    check(lMesh.assetId == "crateMesh" && lMesh.materialName == "woodMat" && !lMesh.visible &&
              lMesh.localBounds.min == glm::vec3{-1.0f, -2.0f, -3.0f} &&
              lMesh.localBounds.max == glm::vec3{4.0f, 5.0f, 6.0f},
          "mesh asset/material references and bounds round-trip by name");

    const ColliderComponent& lPad = findByTag(loaded, "Pad").getComponent<ColliderComponent>();
    check(lPad.type == ColliderComponent::Type::Capsule && lPad.isTrigger && lPad.radius == 0.3f &&
              lPad.capsuleHalfHeight == 0.7f && lPad.layer == 0x10u,
          "trigger capsule (type, isTrigger, dimensions, layer) round-trips");

    const ColliderComponent& lHull = findByTag(loaded, "HullBody").getComponent<ColliderComponent>();
    check(lHull.type == ColliderComponent::Type::ConvexHull && lHull.hullPointCount == 4 &&
              lHull.hullPoints[2] == glm::vec3{0.0f, 1.0f, 0.0f},
          "convex hull point cloud round-trips");

    const ColliderComponent& lGround = findByTag(loaded, "Ground").getComponent<ColliderComponent>();
    const RigidBodyComponent& lGroundBody = findByTag(loaded, "Ground").getComponent<RigidBodyComponent>();
    check(lGround.type == ColliderComponent::Type::Plane && lGround.planeOffset == -1.0f && lGroundBody.isStatic,
          "static plane collider round-trips");

    engine::Entity lLinkA = findByTag(loaded, "LinkA");
    engine::Entity lLinkB = findByTag(loaded, "LinkB");
    const JointComponent& lDistance = lLinkA.getComponent<JointComponent>();
    check(lDistance.type == JointType::Distance && lDistance.other == lLinkB.handle() &&
              lDistance.restLength == 2.0f,
          "distance joint re-links to the remapped partner handle");

    const JointComponent& lHinge = findByTag(loaded, "HingeLink").getComponent<JointComponent>();
    check(lHinge.type == JointType::Hinge && lHinge.other == lCrate.handle() && lHinge.enableLimits &&
              lHinge.minAngleDegrees == -30.0f && lHinge.maxAngleDegrees == 45.0f && lHinge.compliance == 0.25f &&
              lHinge.hingeAxis == glm::vec3{0.0f, 0.0f, 1.0f},
          "hinge joint (partner, limits, compliance, axis) round-trips");

    const JointComponent& lSpring = findByTag(loaded, "SpringLink").getComponent<JointComponent>();
    check(lSpring.type == JointType::SpringDamper && lSpring.other == entt::null && lSpring.stiffness == 42.0f &&
              lSpring.damping == 3.5f && lSpring.anchorB == glm::vec3{1.0f, 5.0f, 1.0f},
          "world-anchored spring keeps its null partner and parameters");

    const CameraComponent& lCamera = findByTag(loaded, "Cam").getComponent<CameraComponent>();
    check(lCamera.camera.position() == glm::vec3{9.0f, 8.0f, 7.0f} && lCamera.camera.yawDegrees() == -45.0f &&
              lCamera.camera.pitchDegrees() == 15.0f && lCamera.camera.config().movementSpeed == 7.5f &&
              lCamera.camera.config().fieldOfViewDegrees == 60.0f && lCamera.isActive,
          "camera pose and full config round-trip");

    check(findByTag(loaded, "Spinner").getComponent<SpinComponent>().degreesPerSecond == glm::vec3{0.0f, 90.0f, 0.0f} &&
              findByTag(loaded, "Planet").getComponent<GravityFieldComponent>().gravitationalParameter == 123.5f &&
              findByTag(loaded, "Planet").getComponent<GravityFieldComponent>().radius == 50.0f &&
              findByTag(loaded, "Drone").getComponent<ThrusterComponent>().force == glm::vec3{0.0f, 25.0f, 0.0f} &&
              findByTag(loaded, "Drone").getComponent<ThrusterComponent>().throttle == 0.75f,
          "spin, gravity-field and thruster components round-trip");

    // --- A loaded scene is fully functional -----------------------------------------
    {
        engine::Scene simScene;
        auto floorEntity = simScene.createEntity("Floor");
        floorEntity.addComponent<TransformComponent>(glm::vec3{0.0f, -0.5f, 0.0f});
        floorEntity.addComponent<RigidBodyComponent>(0.0f);
        floorEntity.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{10.0f, 0.5f, 10.0f}));
        auto simA = simScene.createEntity("SimA");
        simA.addComponent<TransformComponent>(glm::vec3{0.0f, 3.0f, 0.0f});
        auto& simBodyA = simA.addComponent<RigidBodyComponent>(1.0f);
        simBodyA.restitution = 0.0f;
        simA.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        auto simB = simScene.createEntity("SimB");
        simB.addComponent<TransformComponent>(glm::vec3{2.0f, 3.0f, 0.0f});
        auto& simBodyB = simB.addComponent<RigidBodyComponent>(1.0f);
        simBodyB.restitution = 0.0f;
        simB.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        simA.addComponent<JointComponent>(
            JointComponent::distance(simB.handle(), glm::vec3{0.0f}, glm::vec3{0.0f}, 2.0f));

        engine::Scene loadedSim;
        check(loadedSim.loadFromString(simScene.saveToString()), "simulation scene loads");
        PhysicsSystem physics;
        PhysicsSettings settings;
        settings.subSteps = 4;
        settings.sleepEnabled = false;
        for (int i = 0; i < 150; ++i) {
            physics.update(loadedSim, 1.0 / 60.0, settings, nullptr);
        }
        const glm::vec3 posA = findByTag(loadedSim, "SimA").getComponent<TransformComponent>().position;
        const glm::vec3 posB = findByTag(loadedSim, "SimB").getComponent<TransformComponent>().position;
        const float separation = glm::length(posB - posA);
        check(std::isfinite(posA.x) && std::isfinite(posA.y) && nearlyEqual(separation, 2.0, 0.05) &&
                  nearlyEqual(posA.y, 0.5, 0.05),
              "loaded scene simulates: bodies land and the remapped distance joint holds its 2 m separation");
    }

    // --- Files and failure handling --------------------------------------------------
    {
        const std::filesystem::path path = std::filesystem::temp_directory_path() / "engine_scene_phase19.json";
        check(scene.saveToFile(path.string()), "saveToFile writes the document");
        engine::Scene fromFile;
        check(fromFile.loadFromFile(path.string()) && fromFile.entityCount() == scene.entityCount() &&
                  findByTag(fromFile, "Crate").getComponent<ColliderComponent>().layer == 0x4u,
              "loadFromFile restores the scene from disk");
        std::filesystem::remove(path);

        const size_t countBefore = loaded.entityCount();
        check(!loaded.loadFromString("{ not json") && loaded.entityCount() == countBefore,
              "malformed JSON is rejected and leaves the scene untouched");
        check(!loaded.loadFromString("{\"format\":\"engine-scene\",\"version\":99,\"entities\":[]}") &&
                  loaded.entityCount() == countBefore,
              "an unsupported format version is rejected and leaves the scene untouched");
        check(!loaded.loadFromFile("/tmp/engine-scene-definitely-missing-12345.json"),
              "a missing scene file is rejected");
    }
}

void testPhase30Gbq() {
    std::cout << "Phase 30: binary scene serialization (.gbq)\n";
    using engine::CameraComponent;
    using engine::ColliderComponent;
    using engine::GravityFieldComponent;
    using engine::JointComponent;
    using engine::MeshComponent;
    using engine::NativeScriptComponent;
    using engine::ParentComponent;
    using engine::PointLightComponent;
    using engine::RigidBodyComponent;
    using engine::SpotLightComponent;
    using engine::SpinComponent;
    using engine::ThrusterComponent;
    using engine::TransformComponent;

    const auto findByTag = [](engine::Scene& scene, const std::string& tag) -> engine::Entity {
        auto& registry = scene.registry();
        for (const entt::entity entity : registry.view<engine::TagComponent>()) {
            if (registry.get<engine::TagComponent>(entity).tag == tag) {
                return engine::Entity{entity, &scene};
            }
        }
        return engine::Entity{};
    };
    const auto bitsOf = [](float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, 4);
        return bits;
    };
    const auto exactVec3 = [](const glm::vec3& a, const glm::vec3& b) { return a == b; };

    // --- Build a component-rich nested scene -----------------------------------
    engine::Scene scene;
    scene.setHierarchyDestroyMode(engine::HierarchyDestroyMode::Cascade);

    auto rig = scene.createEntity("Rig");
    auto& rigTransform = rig.addComponent<TransformComponent>(glm::vec3{1.5f, -2.25f, 3.75f});
    rigTransform.rotationDegrees = glm::vec3{10.0f, 20.0f, 30.0f};
    rigTransform.orientation = glm::normalize(glm::quat{0.9238795f, 0.0f, 0.3826834f, 0.0f});
    rigTransform.useQuaternionOrientation = true;

    auto crate = scene.createEntity("Crate");
    auto& crateTransform = crate.addComponent<TransformComponent>(glm::vec3{-4.5f, 0.125f, 8.0f});
    crateTransform.scale = glm::vec3{2.0f, 0.5f, 1.25f};
    auto& crateMesh = crate.addComponent<MeshComponent>(MeshComponent{std::string{"models/cube.obj"}});
    crateMesh.materialName = "wood";
    crateMesh.visible = false;
    crateMesh.localBounds = engine::AABB{glm::vec3{-1.0f}, glm::vec3{1.0f}};
    auto& crateBody = crate.addComponent<RigidBodyComponent>(2.5f);
    crateBody.velocity = glm::vec3{1.0f, 2.0f, 3.0f};
    crateBody.angularVelocity = glm::vec3{0.1f, 0.2f, 0.3f};
    crateBody.restitution = 0.42f;
    crateBody.friction = 0.77f;
    crateBody.gravityScale = 0.5f;
    crateBody.sleeping = true;
    crateBody.allowSleep = false;
    auto& crateCollider = crate.addComponent<ColliderComponent>();
    crateCollider.type = ColliderComponent::Type::ConvexHull;
    crateCollider.hullPoints = {glm::vec3{0.0f}, glm::vec3{1.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 1.0f, 0.0f},
                                glm::vec3{0.0f, 0.0f, -1.0f}};
    crateCollider.hullPointCount = 4;
    crateCollider.isTrigger = true;
    crateCollider.layer = 0x8u;
    crateCollider.mask = 0xF0u;
    crate.addComponent<SpinComponent>(glm::vec3{15.0f, -30.0f, 45.0f});
    scene.setParent(crate, rig);

    auto lamp = scene.createEntity("Lamp");
    lamp.addComponent<TransformComponent>(glm::vec3{0.0f, 2.0f, 0.0f});
    auto& lampLight = lamp.addComponent<PointLightComponent>();
    lampLight.color = glm::vec3{1.0f, 0.5f, 0.25f};
    lampLight.intensity = 42.5f;
    lampLight.range = 17.5f;
    lampLight.enabled = false;
    scene.setParent(lamp, crate);

    auto cam = scene.createEntity("Cam");
    cam.addComponent<TransformComponent>(glm::vec3{0.0f});
    engine::CameraConfig cameraConfig;
    cameraConfig.movementSpeed = 7.5f;
    cameraConfig.fieldOfViewDegrees = 60.0f;
    cameraConfig.invertY = true;
    auto& cameraComponent = cam.addComponent<CameraComponent>(cameraConfig);
    cameraComponent.camera.setPosition(glm::vec3{9.0f, 8.0f, 7.0f});
    cameraComponent.camera.setOrientation(-45.0f, 15.0f);

    auto spot = scene.createEntity("Spot");
    spot.addComponent<TransformComponent>(glm::vec3{5.0f, 6.0f, 5.0f});
    auto& spotLight = spot.addComponent<SpotLightComponent>();
    spotLight.color = glm::vec3{0.25f, 0.5f, 1.0f};
    spotLight.intensity = 99.0f;
    spotLight.innerConeRadians = 0.31f;
    spotLight.outerConeRadians = 0.77f;

    auto drone = scene.createEntity("Drone");
    drone.addComponent<TransformComponent>(glm::vec3{1.0f, 1.0f, 1.0f});
    drone.addComponent<RigidBodyComponent>(3.0f);
    auto& thruster = drone.addComponent<ThrusterComponent>();
    thruster.force = glm::vec3{0.0f, 25.0f, 0.0f};
    thruster.localPosition = glm::vec3{0.0f, -0.5f, 0.0f};
    thruster.throttle = 0.75f;
    drone.addComponent<NativeScriptComponent>(std::string{"DroneAI"});

    auto planet = scene.createEntity("Planet");
    planet.addComponent<TransformComponent>(glm::vec3{0.0f, 0.0f, -20.0f});
    auto& gravity = planet.addComponent<GravityFieldComponent>();
    gravity.gravitationalParameter = 123.5f;
    gravity.radius = 50.0f;
    gravity.enabled = false;

    auto linkA = scene.createEntity("LinkA");
    linkA.addComponent<TransformComponent>(glm::vec3{0.0f, 4.0f, 0.0f});
    auto linkB = scene.createEntity("LinkB");
    linkB.addComponent<TransformComponent>(glm::vec3{2.0f, 4.0f, 0.0f});
    auto& distance = linkA.addComponent<JointComponent>(
        JointComponent::distance(linkB.handle(), glm::vec3{0.25f}, glm::vec3{-0.25f}, 2.0f));
    distance.minDistance = 1.0f;
    distance.maxDistance = 3.0f;
    auto& hinge = linkB.addComponent<JointComponent>(
        JointComponent::hinge(linkA.handle(), glm::vec3{0.5f, 0.0f, 0.0f}, glm::vec3{-0.5f, 0.0f, 0.0f},
                              glm::vec3{0.0f, 0.0f, 1.0f}));
    hinge.enableLimits = true;
    hinge.minAngleDegrees = -30.0f;
    hinge.maxAngleDegrees = 45.0f;
    hinge.compliance = 0.25f;

    const std::filesystem::path gbqPath = std::filesystem::temp_directory_path() / "engine_scene_phase30.gbq";
    const std::filesystem::path gbqPath2 = std::filesystem::temp_directory_path() / "engine_scene_phase30_b.gbq";

    // --- Container structure ------------------------------------------------------
    check(engine::serializeScene(scene, gbqPath), "serializeScene writes the .gbq container");
    engine::GbqFileInfo info;
    check(engine::inspectGbqFile(gbqPath, info) && info.version == engine::kGbqVersion &&
              info.entityCount == scene.entityCount() && !info.sections.empty(),
          "the table of contents reports version, entity count and sections");
    const auto hasSection = [&info](const char* type) {
        for (const engine::GbqSectionInfo& section : info.sections) {
            if (section.type == type) {
                return true;
            }
        }
        return false;
    };
    check(hasSection("TRNF") && hasSection("JONT") && hasSection("PLGT") && hasSection("SLGT") &&
              hasSection("NSCR"),
          "the index lists transform, joint, light and script sections");
    check(info.fileSize < scene.saveToString().size(),
          "the binary container is smaller than the same scene as JSON text");

    // --- Round trip: bit-for-bit --------------------------------------------------
    engine::Scene loaded;
    loaded.createEntity("Junk");
    check(engine::deserializeScene(loaded, gbqPath) && loaded.entityCount() == scene.entityCount() &&
              !findByTag(loaded, "Junk").isValid(),
          "deserializeScene clears the previous scene and restores every entity");
    check(loaded.hierarchyDestroyMode() == engine::HierarchyDestroyMode::Cascade,
          "hierarchy destroy mode round-trips through .gbq");

    engine::Entity lRig = findByTag(loaded, "Rig");
    engine::Entity lCrate = findByTag(loaded, "Crate");
    engine::Entity lLamp = findByTag(loaded, "Lamp");
    const TransformComponent& lCrateTransform = lCrate.getComponent<TransformComponent>();
    check(bitsOf(lCrateTransform.position.x) == bitsOf(crateTransform.position.x) &&
              exactVec3(lCrateTransform.position, crateTransform.position) &&
              exactVec3(lCrateTransform.scale, crateTransform.scale),
          "transform floats survive bit-for-bit");
    const TransformComponent& lRigTransform = lRig.getComponent<TransformComponent>();
    check(lRigTransform.useQuaternionOrientation && lRigTransform.orientation == rigTransform.orientation &&
              lRigTransform.orientation.w == rigTransform.orientation.w,
          "quaternion orientation mode and value round-trip");
    check(lCrate.getComponent<ParentComponent>().parent == lRig.handle() &&
              lLamp.getComponent<ParentComponent>().parent == lCrate.handle() &&
              !loaded.registry().all_of<ParentComponent>(lRig.handle()),
          "the nested hierarchy is rebuilt through parent links");

    const MeshComponent& lMesh = lCrate.getComponent<MeshComponent>();
    check(lMesh.assetId == "models/cube.obj" && lMesh.materialName == "wood" && !lMesh.visible &&
              lMesh.localBounds.min == glm::vec3{-1.0f} && lMesh.localBounds.max == glm::vec3{1.0f},
          "mesh asset reference, material, visibility and bounds round-trip");
    const RigidBodyComponent& lBody = lCrate.getComponent<RigidBodyComponent>();
    check(lBody.mass == 2.5f && exactVec3(lBody.velocity, glm::vec3{1.0f, 2.0f, 3.0f}) &&
              bitsOf(lBody.restitution) == bitsOf(0.42f) && lBody.friction == 0.77f && lBody.gravityScale == 0.5f &&
              lBody.sleeping && !lBody.allowSleep && lBody.inertiaTensor[0][0] == crateBody.inertiaTensor[0][0] &&
              lBody.inverseInertiaTensor[2][2] == crateBody.inverseInertiaTensor[2][2],
          "rigid body dynamics state, inertia tensors and flags round-trip");
    const ColliderComponent& lCollider = lCrate.getComponent<ColliderComponent>();
    check(lCollider.type == ColliderComponent::Type::ConvexHull && lCollider.hullPointCount == 4 &&
              exactVec3(lCollider.hullPoints[3], glm::vec3{0.0f, 0.0f, -1.0f}) && lCollider.isTrigger &&
              lCollider.layer == 0x8u && lCollider.mask == 0xF0u,
          "convex hull points, trigger flag and collision filter round-trip");
    check(exactVec3(lCrate.getComponent<SpinComponent>().degreesPerSecond, glm::vec3{15.0f, -30.0f, 45.0f}),
          "spin rates round-trip");

    const CameraComponent& lCamera = findByTag(loaded, "Cam").getComponent<CameraComponent>();
    check(lCamera.camera.position() == glm::vec3{9.0f, 8.0f, 7.0f} && lCamera.camera.yawDegrees() == -45.0f &&
              lCamera.camera.pitchDegrees() == 15.0f && lCamera.camera.config().movementSpeed == 7.5f &&
              lCamera.camera.config().fieldOfViewDegrees == 60.0f && lCamera.camera.config().invertY,
          "camera config and live view state round-trip");

    const PointLightComponent& lPoint = lLamp.getComponent<PointLightComponent>();
    check(exactVec3(lPoint.color, glm::vec3{1.0f, 0.5f, 0.25f}) && lPoint.intensity == 42.5f &&
              lPoint.range == 17.5f && !lPoint.enabled,
          "point light round-trips (a component the JSON format never stored)");
    const SpotLightComponent& lSpot = findByTag(loaded, "Spot").getComponent<SpotLightComponent>();
    check(lSpot.intensity == 99.0f && bitsOf(lSpot.innerConeRadians) == bitsOf(0.31f) &&
              bitsOf(lSpot.outerConeRadians) == bitsOf(0.77f),
          "spot light cones round-trip bit-for-bit");

    engine::Entity lDrone = findByTag(loaded, "Drone");
    check(exactVec3(lDrone.getComponent<ThrusterComponent>().force, glm::vec3{0.0f, 25.0f, 0.0f}) &&
              lDrone.getComponent<ThrusterComponent>().throttle == 0.75f,
          "thruster state round-trips");
    check(lDrone.getComponent<NativeScriptComponent>().scriptName == "DroneAI",
          "the native script name round-trips (the instance is recreated by the script system)");
    const GravityFieldComponent& lGravity = findByTag(loaded, "Planet").getComponent<GravityFieldComponent>();
    check(lGravity.gravitationalParameter == 123.5f && lGravity.radius == 50.0f && !lGravity.enabled,
          "gravity field round-trips");

    engine::Entity lLinkA = findByTag(loaded, "LinkA");
    engine::Entity lLinkB = findByTag(loaded, "LinkB");
    const JointComponent& lDistance = lLinkA.getComponent<JointComponent>();
    const JointComponent& lHinge = lLinkB.getComponent<JointComponent>();

    check(lDistance.other == lLinkB.handle() && lDistance.restLength == 2.0f && lDistance.minDistance == 1.0f &&
              lDistance.maxDistance == 3.0f,
          "the distance joint's entity link is remapped to the new handle");
    check(lHinge.other == lLinkA.handle() && lHinge.type == engine::JointType::Hinge && lHinge.enableLimits &&
              lHinge.minAngleDegrees == -30.0f && lHinge.maxAngleDegrees == 45.0f && lHinge.compliance == 0.25f,
          "the hinge joint, its limits and its back-link round-trip");

    // A second save -> load cycle must land on the same scene again
    // (record order follows EnTT storage order, so the bytes may be
    // permuted; the restored data must not drift).
    check(engine::serializeScene(loaded, gbqPath2), "the restored scene re-serializes");
    const auto readBytes = [](const std::filesystem::path& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    };
    engine::Scene loadedTwice;
    check(engine::deserializeScene(loadedTwice, gbqPath2) && loadedTwice.entityCount() == scene.entityCount() &&
              exactVec3(findByTag(loadedTwice, "Crate").getComponent<TransformComponent>().position,
                        crateTransform.position) &&
              findByTag(loadedTwice, "LinkB").getComponent<JointComponent>().other ==
                  findByTag(loadedTwice, "LinkA").handle(),
          "save -> load -> save -> load is stable");

    // Both formats describe the same scene (JSON parity on shared fields).
    engine::Scene fromJson;
    check(fromJson.loadFromString(scene.saveToString()) && fromJson.entityCount() == loaded.entityCount() &&
              exactVec3(findByTag(fromJson, "Crate").getComponent<TransformComponent>().position,
                        lCrateTransform.position),
          "the JSON scene and the .gbq scene agree on the shared data");

    // --- Malformed input is rejected without touching the scene -------------------
    engine::Scene guarded;
    guarded.createEntity("Keeper");
    const auto writeBytes = [](const std::filesystem::path& path, const std::string& bytes) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << bytes;
    };
    const std::string good = readBytes(gbqPath);
    std::string truncated = good.substr(0, good.size() / 2);
    writeBytes(gbqPath2, truncated);
    check(!engine::deserializeScene(guarded, gbqPath2) && guarded.entityCount() == 1 &&
              findByTag(guarded, "Keeper").isValid(),
          "a truncated .gbq is rejected and leaves the scene untouched");
    std::string badMagic = good;
    badMagic[0] = 'X';
    writeBytes(gbqPath2, badMagic);
    check(!engine::deserializeScene(guarded, gbqPath2) && guarded.entityCount() == 1,
          "a wrong magic is rejected");
    std::string badVersion = good;
    badVersion[4] = static_cast<char>(99); // format version, little-endian low byte
    writeBytes(gbqPath2, badVersion);
    check(!engine::deserializeScene(guarded, gbqPath2) && guarded.entityCount() == 1,
          "an unsupported format version is rejected");
    writeBytes(gbqPath2, "");
    check(!engine::deserializeScene(guarded, gbqPath2) && guarded.entityCount() == 1, "an empty file is rejected");
    check(!engine::deserializeScene(guarded, "/tmp/engine-scene-definitely-missing-12345.gbq") &&
              guarded.entityCount() == 1,
          "a missing .gbq file is rejected");
    engine::GbqFileInfo badInfo;
    check(!engine::inspectGbqFile("/tmp/engine-scene-definitely-missing-12345.gbq", badInfo),
          "inspectGbqFile rejects a missing file");

    std::filesystem::remove(gbqPath);
    std::filesystem::remove(gbqPath2);
}

void testPhase31PlayMode() {
    std::cout << "Phase 31: play/stop mode (runtime simulation)\n";
    using engine::CameraComponent;
    using engine::ColliderComponent;
    using engine::JointComponent;
    using engine::NativeScriptComponent;
    using engine::ParentComponent;
    using engine::PhysicsSettings;
    using engine::PhysicsSystem;
    using engine::PointLightComponent;
    using engine::RigidBodyComponent;
    using engine::SpinComponent;
    using engine::ThrusterComponent;
    using engine::TransformComponent;

    const auto findByTag = [](engine::Scene& scene, const std::string& tag) -> engine::Entity {
        auto& registry = scene.registry();
        for (const entt::entity entity : registry.view<engine::TagComponent>()) {
            if (registry.get<engine::TagComponent>(entity).tag == tag) {
                return engine::Entity{entity, &scene};
            }
        }
        return engine::Entity{};
    };
    const auto bitsOf = [](float value) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, 4);
        return bits;
    };

    // --- Authored scene -----------------------------------------------------------
    engine::Scene scene;
    auto ground = scene.createEntity("Ground");
    ground.addComponent<TransformComponent>(glm::vec3{0.0f, -0.5f, 0.0f});
    auto& groundBody = ground.addComponent<RigidBodyComponent>(0.0f);
    groundBody.setStatic(true);
    ground.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{10.0f, 0.5f, 10.0f}));

    auto rig = scene.createEntity("Rig");
    rig.addComponent<TransformComponent>(glm::vec3{1.0f, 2.0f, 3.0f});
    auto crate = scene.createEntity("Crate");
    crate.addComponent<TransformComponent>(glm::vec3{0.0f, 1.0f, 0.0f});
    crate.addComponent<RigidBodyComponent>(1.0f);
    crate.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
    crate.addComponent<SpinComponent>(glm::vec3{0.0f, 90.0f, 0.0f});
    scene.setParent(crate, rig);
    auto lamp = scene.createEntity("Lamp");
    lamp.addComponent<TransformComponent>(glm::vec3{0.0f, 2.0f, 0.0f});
    auto& lampLight = lamp.addComponent<PointLightComponent>();
    lampLight.intensity = 10.0f;
    lampLight.range = 5.0f;
    scene.setParent(lamp, crate);

    auto faller = scene.createEntity("Faller");
    faller.addComponent<TransformComponent>(glm::vec3{3.0f, 10.0f, 0.0f});
    faller.addComponent<RigidBodyComponent>(1.0f);
    faller.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));

    auto drone = scene.createEntity("Drone");
    drone.addComponent<TransformComponent>(glm::vec3{-3.0f, 3.0f, 0.0f});
    drone.addComponent<NativeScriptComponent>(std::string{"DroneAI"});
    auto& droneThruster = drone.addComponent<ThrusterComponent>();
    droneThruster.throttle = 0.75f;

    auto anchorA = scene.createEntity("AnchorA");
    anchorA.addComponent<TransformComponent>(glm::vec3{0.0f, 4.0f, 0.0f});
    auto anchorB = scene.createEntity("AnchorB");
    anchorB.addComponent<TransformComponent>(glm::vec3{2.0f, 4.0f, 0.0f});
    anchorA.addComponent<JointComponent>(
        JointComponent::distance(anchorB.handle(), glm::vec3{0.0f}, glm::vec3{0.0f}, 2.0f));

    auto cam = scene.createEntity("Cam");
    cam.addComponent<TransformComponent>(glm::vec3{0.0f});
    auto& camComponent = cam.addComponent<CameraComponent>(engine::CameraConfig{});
    camComponent.camera.setPosition(glm::vec3{1.0f, 2.0f, 3.0f});

    const size_t authoredCount = scene.entityCount();
    check(authoredCount == 9, "the authored scene has nine entities");

    // --- Snapshot rides the serialization core --------------------------------------
    const std::vector<std::byte> snapshotBytes = scene.saveToGbqBytes();
    const std::filesystem::path diskPath = std::filesystem::temp_directory_path() / "engine_scene_phase31.gbq";
    check(!snapshotBytes.empty() && scene.saveToGbqFile(diskPath.string()),
          "the in-memory snapshot is the GBQ serialization core, no file required");
    {
        std::ifstream in(diskPath, std::ios::binary);
        const std::string diskBytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        check(diskBytes.size() == snapshotBytes.size() &&
                  std::memcmp(diskBytes.data(), snapshotBytes.data(), diskBytes.size()) == 0,
              "snapshot bytes are identical to the .gbq file bytes for the same scene");
        std::filesystem::remove(diskPath);
    }
    check(engine::serializeSceneToBytes(scene) == snapshotBytes, "the free-function serializer agrees byte-for-byte");

    // --- State machine guards ---------------------------------------------------------
    engine::PlaySession session;
    check(session.mode() == engine::EditorMode::Edit && !session.isPlaying() && !session.hasSnapshot(),
          "a fresh session starts in Edit mode with no snapshot");
    check(!session.exitPlay(scene) && scene.entityCount() == authoredCount,
          "Stop while editing is a harmless no-op");

    // --- Enter Play, run the runtime, mutate everything ------------------------------
    check(session.enterPlay(scene) && session.isPlaying() && session.mode() == engine::EditorMode::Play &&
              session.hasSnapshot() && session.snapshotBytes() == snapshotBytes.size(),
          "enterPlay captures the pre-play snapshot and switches to Play");
    check(!session.enterPlay(scene), "entering Play twice is refused");

    PhysicsSystem physics;
    PhysicsSettings settings;
    settings.subSteps = 4;
    settings.sleepEnabled = false;
    for (int i = 0; i < 150; ++i) {
        physics.update(scene, 1.0f / 60.0f, settings, nullptr);
    }
    const float fallenY = findByTag(scene, "Faller").getComponent<TransformComponent>().position.y;
    check(fallenY < 5.0f && fallenY > -1.0f, "runtime physics ran: the falling body dropped and landed");

    findByTag(scene, "Crate").getComponent<TransformComponent>().position = glm::vec3{9.0f, 9.0f, 9.0f};
    findByTag(scene, "Crate").getComponent<SpinComponent>().degreesPerSecond = glm::vec3{1.0f, 2.0f, 3.0f};
    findByTag(scene, "Drone").getComponent<ThrusterComponent>().throttle = 0.1f;
    scene.destroyEntity(findByTag(scene, "Lamp"));
    scene.createEntity("RuntimeJunk");
    check(scene.entityCount() == authoredCount && !findByTag(scene, "Lamp").isValid() &&
              findByTag(scene, "RuntimeJunk").isValid(),
          "the runtime mutated the scene: one entity destroyed, one spawned (count unchanged)");

    // --- Stop: exact restoration -------------------------------------------------------
    check(session.exitPlay(scene) && !session.isPlaying() && session.mode() == engine::EditorMode::Edit &&
              !session.hasSnapshot(),
          "Stop restores the snapshot, frees it, and returns to Edit mode");
    check(scene.entityCount() == authoredCount && !findByTag(scene, "RuntimeJunk").isValid(),
          "runtime-spawned entities are gone after Stop");

    engine::Entity rFaller = findByTag(scene, "Faller");
    const TransformComponent& rFallerTransform = rFaller.getComponent<TransformComponent>();
    check(bitsOf(rFallerTransform.position.x) == bitsOf(3.0f) && bitsOf(rFallerTransform.position.y) == bitsOf(10.0f) &&
              rFaller.getComponent<RigidBodyComponent>().velocity == glm::vec3{0.0f},
          "the fallen body is back at its authored position, bit-for-bit, at rest");
    engine::Entity rCrate = findByTag(scene, "Crate");
    engine::Entity rRig = findByTag(scene, "Rig");
    engine::Entity rLamp = findByTag(scene, "Lamp");
    check(rCrate.getComponent<TransformComponent>().position == glm::vec3{0.0f, 1.0f, 0.0f} &&
              rCrate.getComponent<SpinComponent>().degreesPerSecond == glm::vec3{0.0f, 90.0f, 0.0f},
          "runtime transform and spin edits are rolled back");
    check(rLamp.isValid() && rLamp.getComponent<PointLightComponent>().intensity == 10.0f &&
              rLamp.getComponent<ParentComponent>().parent == rCrate.handle() &&
              rCrate.getComponent<ParentComponent>().parent == rRig.handle(),
          "the runtime-destroyed entity returns with its components and hierarchy links");
    engine::Entity rDrone = findByTag(scene, "Drone");
    check(rDrone.getComponent<NativeScriptComponent>().scriptName == "DroneAI" &&
              rDrone.getComponent<ThrusterComponent>().throttle == 0.75f,
          "script binding and thruster state are restored");
    check(findByTag(scene, "AnchorA").getComponent<JointComponent>().other ==
              findByTag(scene, "AnchorB").handle(),
          "joint links remap onto the restored handles");
    check(findByTag(scene, "Cam").getComponent<CameraComponent>().camera.position() == glm::vec3{1.0f, 2.0f, 3.0f},
          "the camera returns to its authored position");

    // --- A second cycle snapshots the restored scene, not stale state -------------------
    check(session.enterPlay(scene), "a second Play entry is allowed");
    findByTag(scene, "Crate").getComponent<TransformComponent>().position = glm::vec3{99.0f, 99.0f, 99.0f};
    scene.destroyEntity(findByTag(scene, "Faller"));
    check(session.exitPlay(scene) && scene.entityCount() == authoredCount &&
              findByTag(scene, "Crate").getComponent<TransformComponent>().position == glm::vec3{0.0f, 1.0f, 0.0f} &&
              findByTag(scene, "Faller").getComponent<TransformComponent>().position.y == 10.0f,
          "the second cycle restores the same authored state again");
    check(!session.exitPlay(scene) && scene.entityCount() == authoredCount,
          "Stop after Stop stays a no-op");

    // --- Empty scene ----------------------------------------------------------------------
    engine::Scene emptyScene;
    engine::PlaySession emptySession;
    check(emptySession.enterPlay(emptyScene) && emptySession.exitPlay(emptyScene) &&
              emptyScene.entityCount() == 0 && emptySession.mode() == engine::EditorMode::Edit,
          "Play/Stop over an empty scene round-trips cleanly");
}

void testPhase32Pbr() {
    std::cout << "Phase 32: advanced PBR materials, directional & spot shadows\n";
    using engine::CameraComponent;
    using engine::DirectionalLightComponent;
    using engine::PointLightComponent;
    using engine::SpotLightComponent;
    using engine::TransformComponent;

    const auto addCamera = [](engine::Scene& scene) {
        auto cam = scene.createEntity("Cam");
        cam.addComponent<TransformComponent>(glm::vec3{0.0f});
        auto& camComponent = cam.addComponent<CameraComponent>(engine::CameraConfig{});
        camComponent.camera.setPosition(glm::vec3{0.0f});
        camComponent.camera.setOrientation(-90.0f, 0.0f);
    };

    // --- Directional key light: legacy fallback vs entity sun ---------------------
    {
        engine::Scene scene;
        addCamera(scene);
        const engine::SceneLightData data = engine::LightingSystem::gather(scene);
        check(!data.directionalFromEntity && data.directionalRadiance == 1.0f &&
                  data.directionalColor == glm::vec3{1.0f, 0.96f, 0.88f} && data.directionalCastShadow &&
                  data.cascadeCount == engine::kMaxShadowCascades && data.cascadesFilled,
              "no Sun entity: the legacy fixed sun answers (tint, radiance, shadows, 4 cascades)");
        check(nearlyVec3(data.directionalDirection, glm::normalize(glm::vec3{0.35f, 0.8f, 0.45f})),
              "legacy sun direction matches the historical fixed direction");
    }
    {
        engine::Scene scene;
        addCamera(scene);
        auto sun = scene.createEntity("Sun");
        auto& sunLight = sun.addComponent<DirectionalLightComponent>();
        sunLight.color = glm::vec3{0.5f, 0.6f, 0.7f};
        sunLight.intensity = 2.5f;
        sunLight.direction = glm::vec3{0.0f, 3.0f, 4.0f}; // normalized on gather
        sunLight.castShadow = false;
        sunLight.cascadeCount = 2;
        auto dimSun = scene.createEntity("DimSun");
        auto& dim = dimSun.addComponent<DirectionalLightComponent>();
        dim.color = glm::vec3{1.0f, 0.0f, 0.0f};
        dim.intensity = 0.5f;
        auto offSun = scene.createEntity("OffSun");
        auto& off = offSun.addComponent<DirectionalLightComponent>();
        off.intensity = 99.0f;
        off.enabled = false;

        const engine::SceneLightData data = engine::LightingSystem::gather(scene);
        check(data.directionalFromEntity && data.directionalColor == glm::vec3{0.5f, 0.6f, 0.7f} &&
                  data.directionalRadiance == 2.5f && !data.directionalCastShadow,
              "the brightest enabled Sun drives color, radiance and shadow toggle");
        check(nearlyVec3(data.directionalDirection, glm::vec3{0.0f, 0.6f, 0.8f}),
              "the sun direction is normalized on gather");
        check(data.cascadeCount == 2 && data.cascadeSplits.x < data.cascadeSplits.y &&
                  data.cascadeSplits.z == data.cascadeSplits.y && data.cascadeSplits.w == data.cascadeSplits.y &&
                  data.cascadeMatrices[3] == data.cascadeMatrices[1],
              "a 2-cascade sun pins the unused splits and matrices to the last slice");
    }

    // --- Spot shadow selection and light-space math ---------------------------------
    {
        engine::Scene scene;
        addCamera(scene);
        auto point = scene.createEntity("Point");
        point.addComponent<TransformComponent>(glm::vec3{1.0f, 0.0f, 0.0f});
        point.addComponent<PointLightComponent>();
        auto nearSpot = scene.createEntity("NearSpot");
        auto& nearTransform = nearSpot.addComponent<TransformComponent>(glm::vec3{0.0f, 5.0f, 0.0f});
        nearTransform.rotationDegrees = glm::vec3{-90.0f, 0.0f, 0.0f}; // aim down
        auto& nearLight = nearSpot.addComponent<SpotLightComponent>();
        nearLight.castShadow = true;
        nearLight.range = 18.0f;
        nearLight.outerConeRadians = 0.6f;
        auto farSpot = scene.createEntity("FarSpot");
        auto& farTransform = farSpot.addComponent<TransformComponent>(glm::vec3{0.0f, 20.0f, 0.0f});
        farTransform.rotationDegrees = glm::vec3{-90.0f, 0.0f, 0.0f};
        auto& farLight = farSpot.addComponent<SpotLightComponent>();
        farLight.castShadow = true;
        farLight.range = 18.0f;

        const engine::SceneLightData data = engine::LightingSystem::gather(scene);
        check(data.spotShadowEnabled && data.spotShadowLightIndex == 1,
              "the nearest packed shadow-casting spot owns the shadow map (index into the light arrays)");
        const glm::vec4 hitClip = data.spotShadowViewProj * glm::vec4{0.0f, 3.0f, 0.0f, 1.0f};
        const glm::vec3 hitNdc = glm::vec3{hitClip} / hitClip.w;
        check(hitClip.w > 0.0f && std::abs(hitNdc.x) <= 1.0f && std::abs(hitNdc.y) <= 1.0f && hitNdc.z > 0.0f &&
                  hitNdc.z < 1.0f,
              "a point under the shadow spot projects inside the spot shadow frustum");
        const glm::vec4 behindClip = data.spotShadowViewProj * glm::vec4{0.0f, 7.0f, 0.0f, 1.0f};
        check(behindClip.w < 0.0f, "a point behind the spot is behind its shadow camera");

        engine::Scene noShadowScene;
        addCamera(noShadowScene);
        auto plainSpot = noShadowScene.createEntity("PlainSpot");
        plainSpot.addComponent<TransformComponent>(glm::vec3{0.0f, 5.0f, 0.0f});
        plainSpot.addComponent<SpotLightComponent>(); // castShadow defaults false
        const engine::SceneLightData plain = engine::LightingSystem::gather(noShadowScene);
        check(!plain.spotShadowEnabled && plain.spotShadowLightIndex == -1,
              "no shadow-casting spot, no spot shadow");

        const glm::mat4 vp = engine::spotShadowViewProjection(glm::vec3{0.0f, 5.0f, 0.0f}, glm::vec3{0.0f, -1.0f, 0.0f},
                                                              0.6f, 0.17f, 18.0f);
        const glm::vec4 centre = vp * glm::vec4{0.0f, 3.0f, 0.0f, 1.0f};
        const glm::vec3 centreNdc = glm::vec3{centre} / centre.w;
        check(std::abs(centreNdc.x) < 1e-4f && std::abs(centreNdc.y) < 1e-4f && centreNdc.z > 0.0f &&
                  centreNdc.z < 1.0f,
              "the spot axis lands on the shadow map centre at a mappable depth");
    }

    // --- Live material editing ------------------------------------------------------
    {
        engine::VramBudget vram(1024 * 1024);
        engine::VramBudget staging(1024 * 1024);
        engine::AssetManager assets(vram, staging);
        const std::vector<uint8_t> pixels(2 * 2 * 4, 200);
        const auto tex = [&assets, &pixels](const char* name) {
            return assets.registerTexture(name, 2, 2, std::span<const uint8_t>{pixels});
        };
        const engine::TextureId albedoId = tex("albedoTex");
        const engine::TextureId mrId = tex("mrTex");
        const engine::TextureId normalId = tex("nrmTex");
        const engine::TextureId aoId = tex("aoTex");
        check(albedoId != engine::kInvalidAssetId && mrId != engine::kInvalidAssetId &&
                  normalId != engine::kInvalidAssetId && aoId != engine::kInvalidAssetId,
              "material textures register for the binding test");

        engine::MaterialDesc desc;
        desc.name = "pbr1";
        desc.pbr.metallic = 0.2f;
        desc.pbr.roughness = 0.8f;
        const engine::MaterialId materialId = assets.registerMaterial(desc);
        const uint32_t descriptorIndex = assets.material(materialId)->descriptorIndex;
        check(assets.materialRegistryVersion() == 1, "material registration bumps the registry version");
        check(!assets.updateMaterial("notRegistered", desc), "updating an unknown material is refused");

        engine::MaterialDesc edited = desc;
        edited.baseColor = glm::vec4{0.25f, 0.5f, 0.75f, 1.0f};
        edited.pbr.metallic = 1.0f;
        edited.pbr.roughness = 0.25f;
        edited.pbr.occlusionStrength = 0.5f;
        edited.pbr.albedoTexture = "albedoTex";
        edited.pbr.metallicRoughnessTexture = "mrTex";
        edited.pbr.normalTexture = "nrmTex";
        edited.pbr.occlusionTexture = "aoTex";
        check(assets.updateMaterial("pbr1", edited) && assets.materialRegistryVersion() == 2,
              "updateMaterial commits and bumps the version");
        const engine::MaterialAsset* updated = assets.material("pbr1");
        check(updated != nullptr && updated->id == materialId && updated->descriptorIndex == descriptorIndex &&
                  updated->desc.pbr.metallic == 1.0f && updated->desc.pbr.roughness == 0.25f,
              "the update replaces values in place (id and descriptor slot stable)");

        engine::Scene scene;
        scene.createEntity("Cam").addComponent<CameraComponent>(engine::CameraConfig{});
        auto cube = scene.createEntity("Cube");
        cube.addComponent<TransformComponent>(glm::vec3{0.0f, 0.0f, -3.0f});
        auto& mesh = cube.addComponent<engine::MeshComponent>("cube");
        mesh.materialName = "pbr1";
        engine::FramePrepareOptions options;
        options.assets = &assets;
        options.frustumCulling = false;
        const engine::PreparedFrame frame = engine::RenderSystem::prepareFrame(scene, 16.0f / 9.0f, options);
        check(frame.renderQueue.size() == 1, "the material scene produces one draw item");
        if (frame.renderQueue.size() == 1) {
            const engine::DrawItem& item = frame.renderQueue.items().front();
            check(item.pbrParams == glm::vec4{1.0f, 0.25f, 0.5f, 0.0f} &&
                      item.baseColor == glm::vec4{0.25f, 0.5f, 0.75f, 1.0f},
                  "draw items pack the edited PBR factors and base color");
            check(item.textureIndices[0] == static_cast<int32_t>(albedoId) - 1 &&
                      item.textureIndices[1] == static_cast<int32_t>(mrId) - 1 &&
                      item.textureIndices[2] == static_cast<int32_t>(normalId) - 1 &&
                      item.textureIndices[3] == static_cast<int32_t>(aoId) - 1,
                  "all four material maps bind to their texture array slots");
        }

        engine::MaterialDesc reEdited = edited;
        reEdited.pbr.roughness = 0.9f;
        reEdited.pbr.normalTexture.clear();
        assets.updateMaterial("pbr1", reEdited);
        const engine::PreparedFrame reframed = engine::RenderSystem::prepareFrame(scene, 16.0f / 9.0f, options);
        check(reframed.renderQueue.size() == 1 && reframed.renderQueue.items().front().pbrParams.y == 0.9f &&
                  reframed.renderQueue.items().front().textureIndices[2] == -1,
              "a second edit flows into the very next frame (live viewport updates)");
    }

    // --- GBQ: directional light + spot shadow flag round-trip ------------------------
    {
        engine::Scene scene;
        auto sun = scene.createEntity("Sun");
        auto& sunLight = sun.addComponent<DirectionalLightComponent>();
        sunLight.color = glm::vec3{0.9f, 0.8f, 0.7f};
        sunLight.intensity = 3.25f;
        sunLight.direction = glm::vec3{0.1f, 0.9f, 0.2f};
        sunLight.castShadow = false;
        sunLight.cascadeCount = 3;
        auto spot = scene.createEntity("Spot");
        spot.addComponent<TransformComponent>(glm::vec3{1.0f, 2.0f, 3.0f});
        auto& spotLight = spot.addComponent<SpotLightComponent>();
        spotLight.intensity = 77.0f;
        spotLight.castShadow = true;

        engine::Scene loaded;
        check(engine::deserializeSceneFromBytes(loaded, engine::serializeSceneToBytes(scene)),
              "the phase-32 scene round-trips through .gbq bytes");
        const auto findByTag = [&loaded](const std::string& tag) -> engine::Entity {
            auto& registry = loaded.registry();
            for (const entt::entity entity : registry.view<engine::TagComponent>()) {
                if (registry.get<engine::TagComponent>(entity).tag == tag) {
                    return engine::Entity{entity, &loaded};
                }
            }
            return engine::Entity{};
        };
        const DirectionalLightComponent& rSun = findByTag("Sun").getComponent<DirectionalLightComponent>();
        check(rSun.color == glm::vec3{0.9f, 0.8f, 0.7f} && rSun.intensity == 3.25f &&
                  rSun.direction == glm::vec3{0.1f, 0.9f, 0.2f} && !rSun.castShadow && rSun.cascadeCount == 3,
              "the directional light survives serialization whole");
        check(findByTag("Spot").getComponent<SpotLightComponent>().castShadow,
              "the spot shadow flag survives serialization (SLGT section v2)");
    }

    // --- SLGT version 1 files (pre-Phase-32) still load ------------------------------
    {
        engine::Scene scene;
        auto spot = scene.createEntity("Spot");
        spot.addComponent<TransformComponent>(glm::vec3{0.0f});
        auto& spotLight = spot.addComponent<SpotLightComponent>();
        spotLight.intensity = 55.0f;
        spotLight.castShadow = true;
        std::vector<std::byte> bytes = scene.saveToGbqBytes();

        // Downgrade the file to SLGT v1: drop the trailing byte of
        // the (single) record and patch its TOC entry. The spot is
        // the scene's only sectioned component after META/ENTY/
        // TAGC/TRNF, so SLGT is the last section in the file.
        const auto u32At = [&bytes](size_t offset) {
            uint32_t value = 0;
            std::memcpy(&value, bytes.data() + offset, 4);
            return value;
        };
        const uint32_t sectionCount = u32At(12);
        size_t slgtToc = 0;
        uint64_t slgtSize = 0;
        for (uint32_t i = 0; i < sectionCount; ++i) {
            const size_t toc = 32 + static_cast<size_t>(i) * 32;
            if (std::memcmp(bytes.data() + toc, "SLGT", 4) == 0) {
                slgtToc = toc;
                std::memcpy(&slgtSize, bytes.data() + toc + 16, 8);
            }
        }
        check(slgtToc != 0 && slgtSize > 0, "fixture: the fresh save has an SLGT section to downgrade");
        const uint32_t downgrade = 1; // section version word
        bytes.erase(bytes.end() - 1); // trailing castShadow byte of the last record
        std::memcpy(bytes.data() + slgtToc + 4, &downgrade, 4);
        const uint64_t newSize = slgtSize - 1;
        std::memcpy(bytes.data() + slgtToc + 16, &newSize, 8);

        engine::Scene loaded;
        check(engine::deserializeSceneFromBytes(loaded, bytes) && loaded.entityCount() == 1,
              "a version-1 SLGT file still loads");
        const SpotLightComponent& rSpot = [&loaded]() -> const SpotLightComponent& {
            auto& registry = loaded.registry();
            const auto view = registry.view<SpotLightComponent>();
            return view.get<SpotLightComponent>(*view.begin());
        }();
        check(rSpot.intensity == 55.0f && !rSpot.castShadow,
              "v1 records gain the default (shadow off) for the field they lack");
    }
}

// Phase 33 fixtures: a native script class the test "game build"
// provides. The bundle packs only its name; the runtime (this test
// binary) supplies the implementation through the registry.
class Phase33ProbeScript final : public engine::INativeScript {};
REGISTER_SCRIPT(Phase33ProbeScript)

void testPhase33Export() {
    std::cout << "Phase 33: standalone packaging & game export\n";
    using engine::ColliderComponent;
    using engine::MeshComponent;
    using engine::NativeScriptComponent;
    using engine::RigidBodyComponent;
    using engine::TransformComponent;

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "engine_phase33_game";
    const fs::path bundlePath = dir / "game.gbq";

    // --- CRC-32 known vector ------------------------------------------------
    check(engine::bundleCrc32("123456789", 9) == 0xCBF43926u,
          "export: CRC-32 matches the ISO 3309 check value for \"123456789\"");

    // --- Fixture: assets + scene --------------------------------------------
    engine::VramBudget vram{64ull << 20};
    engine::VramBudget staging{64ull << 20};
    engine::AssetManager assets(vram, staging);

    const std::vector<engine::Vertex> triVertices = {
        engine::Vertex{glm::vec3{0.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, glm::vec3{1.0f, 0.0f, 0.0f},
                       glm::vec2{0.0f, 0.0f}},
        engine::Vertex{glm::vec3{1.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, glm::vec3{0.0f, 1.0f, 0.0f},
                       glm::vec2{1.0f, 0.0f}},
        engine::Vertex{glm::vec3{0.0f, 1.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 1.0f}, glm::vec3{0.0f, 0.0f, 1.0f},
                       glm::vec2{0.0f, 1.0f}},
    };
    const std::vector<uint32_t> triIndices{0, 1, 2};
    check(assets.registerMesh("tri", std::span<const engine::Vertex>{triVertices},
                              std::span<const uint32_t>{triIndices}) != engine::kInvalidAssetId,
          "export fixture: triangle mesh registers");

    const std::vector<uint8_t> texPixels{10, 20, 30, 255, 40, 50, 60, 255, 70, 80, 90, 255, 100, 110, 120, 255};
    check(assets.registerTexture("texA", 2, 2, std::span<const uint8_t>{texPixels}) != engine::kInvalidAssetId,
          "export fixture: 2x2 texture registers");
    const std::vector<uint8_t> bigPixels(32 * 32 * 4, 0x7F);
    check(assets.registerTexture("texBig", 32, 32, std::span<const uint8_t>{bigPixels}) !=
              engine::kInvalidAssetId,
          "export fixture: flat 32x32 texture registers (compression fodder)");

    const std::vector<uint32_t> vertWords{0x07230203u, 0x00010000u, 11u, 22u};
    const std::vector<uint32_t> fragWords{0x07230203u, 0x00010000u, 33u, 44u};
    check(assets.registerShader("a.vert.spv", std::span<const uint32_t>{vertWords}) != engine::kInvalidAssetId &&
              assets.registerShader("a.frag.spv", std::span<const uint32_t>{fragWords}) !=
                  engine::kInvalidAssetId,
          "export fixture: shader pair registers");

    engine::MaterialDesc matDesc;
    matDesc.name = "matA";
    matDesc.vertexShader = "a.vert.spv";
    matDesc.fragmentShader = "a.frag.spv";
    matDesc.baseColor = glm::vec4{0.2f, 0.4f, 0.6f, 1.0f};
    matDesc.pbr.metallic = 0.75f;
    matDesc.pbr.roughness = 0.3f;
    matDesc.pbr.albedoTexture = "texA";
    matDesc.pbr.normalTexture = "texBig";
    check(assets.registerMaterial(matDesc) != engine::kInvalidAssetId, "export fixture: material registers");

    engine::Scene scene;
    {
        auto floor = scene.createEntity("Floor");
        floor.addComponent<TransformComponent>(glm::vec3{0.0f, 0.0f, 0.0f});
        auto& floorMesh = floor.addComponent<MeshComponent>("tri");
        floorMesh.materialName = "matA";
        auto& floorBody = floor.addComponent<RigidBodyComponent>();
        floorBody.setStatic(true);
        floor.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        auto crate = scene.createEntity("Crate");
        crate.addComponent<TransformComponent>(glm::vec3{0.0f, 2.0f, 0.0f});
        auto& crateMesh = crate.addComponent<MeshComponent>("tri");
        crateMesh.materialName = "matA";
        crate.addComponent<RigidBodyComponent>(1.0f);
        crate.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{0.5f}));
        auto scripted = scene.createEntity("Scripted");
        scripted.addComponent<TransformComponent>(glm::vec3{3.0f, 0.0f, 0.0f});
        scripted.addComponent<NativeScriptComponent>("Phase33ProbeScript");
    }

    // --- Export ----------------------------------------------------------------
    const engine::ExportReport report = engine::ExportProject(scene, assets, bundlePath);
    check(report.ok && report.error.empty() && report.missing.empty(), "export: a complete scene exports cleanly");
    check(report.packed.size() == 8, "export: bundle packs 8 entries (scene, mesh, material, 2 textures, 2 "
                                     "shaders, script)");
    check(fs::exists(bundlePath) && fs::exists(dir / "game.manifest.json") && fs::exists(dir / "game.ini"),
          "export: bundle, manifest sidecar and game.ini land side by side");
    {
        std::ifstream sidecar(dir / "game.manifest.json", std::ios::binary);
        const std::string sidecarText{std::istreambuf_iterator<char>{sidecar}, std::istreambuf_iterator<char>{}};
        std::ifstream configFile(dir / "game.ini", std::ios::binary);
        const std::string configText{std::istreambuf_iterator<char>{configFile}, std::istreambuf_iterator<char>{}};
        check(sidecarText.find("\"tri\"") != std::string::npos,
              "export: the sidecar manifest indexes the packed assets by name");
        check(configText.find("bundle = game.gbq") != std::string::npos,
              "export: game.ini points the standalone player at the bundle");
    }

    // --- Bundle structure ---------------------------------------------------------
    std::vector<std::byte> bundleBytes;
    {
        std::ifstream in(bundlePath, std::ios::binary);
        const std::vector<char> chars{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        bundleBytes.resize(chars.size());
        std::memcpy(bundleBytes.data(), chars.data(), chars.size());
    }
    check(bundleBytes.size() > 32 && std::memcmp(bundleBytes.data(), "GBQB", 4) == 0,
          "bundle: file starts with the GBQB magic");

    engine::BundleInfo info;
    check(engine::inspectBundle(bundlePath, info) && info.version == 1 && info.entries.size() == 8 &&
              info.bundleFileSize == bundleBytes.size(),
          "bundle: header + manifest parse (version 1, 8 entries, true file size)");
    const engine::BundleEntryInfo* sceneEntry = nullptr;
    const engine::BundleEntryInfo* meshEntry = nullptr;
    const engine::BundleEntryInfo* bigTexEntry = nullptr;
    for (const engine::BundleEntryInfo& entry : info.entries) {
        if (entry.kind == engine::BundleEntryKind::Scene) {
            sceneEntry = &entry;
        } else if (entry.kind == engine::BundleEntryKind::Mesh && entry.name == "tri") {
            meshEntry = &entry;
        } else if (entry.kind == engine::BundleEntryKind::Texture && entry.name == "texBig") {
            bigTexEntry = &entry;
        }
    }
    check(sceneEntry != nullptr && sceneEntry->rawSize == scene.saveToGbqBytes().size(),
          "bundle: the scene entry carries the exact GBQ scene bytes");
    check(meshEntry != nullptr && meshEntry->rawSize == 8 + 3 * sizeof(engine::Vertex) + 3 * sizeof(uint32_t),
          "bundle: the mesh entry is a counts header plus raw vertex/index bytes");
    check(bigTexEntry != nullptr && bigTexEntry->storedSize < bigTexEntry->rawSize,
          "bundle: the flat texture compresses below its raw size (LZ77 works)");

    // --- Entry round-trips ---------------------------------------------------------
    std::vector<std::byte> extracted;
    check(engine::extractBundleEntry(bundlePath, "tri", extracted) && extracted.size() >= 8 &&
              *reinterpret_cast<const uint32_t*>(extracted.data()) == 3 &&
              *reinterpret_cast<const uint32_t*>(extracted.data() + 4) == 3,
          "bundle: the mesh entry extracts with its vertex/index counts");
    if (extracted.size() >= 8 + sizeof(engine::Vertex)) {
        const auto* vertex = reinterpret_cast<const engine::Vertex*>(extracted.data() + 8);
        check(vertex->position == triVertices[0].position && vertex->normal == triVertices[0].normal &&
                  vertex->texCoord == triVertices[0].texCoord,
              "bundle: mesh vertex data survives the pack/extract round trip");
    } else {
        check(false, "bundle: mesh vertex data survives the pack/extract round trip");
    }
    check(engine::extractBundleEntry(bundlePath, "texA", extracted) && extracted.size() == 8 + texPixels.size() &&
              std::memcmp(extracted.data() + 8, texPixels.data(), texPixels.size()) == 0,
          "bundle: texture pixels extract byte-identical");
    check(!engine::extractBundleEntry(bundlePath, "no-such-entry", extracted),
          "bundle: extracting an unlisted entry fails");

    {
        std::vector<std::byte> sceneBytes;
        engine::Scene revived;
        check(engine::extractBundleEntry(bundlePath, "scene", sceneBytes) &&
                  engine::deserializeSceneFromBytes(revived, sceneBytes) && revived.entityCount() == 3,
              "bundle: the packed scene entry deserializes to the exported entity set");
    }

    // --- Integrity: tamper + non-bundles ------------------------------------------------
    const fs::path tamperedPath = dir / "tampered.gbq";
    {
        std::vector<char> chars(bundleBytes.size());
        std::memcpy(chars.data(), bundleBytes.data(), chars.size());
        const size_t flipAt = meshEntry != nullptr
                                  ? static_cast<size_t>(meshEntry->dataOffset) + 2
                                  : chars.size() / 2;
        chars[flipAt] = static_cast<char>(chars[flipAt] ^ 0xFF);
        std::ofstream out(tamperedPath, std::ios::binary | std::ios::trunc);
        out.write(chars.data(), static_cast<std::streamsize>(chars.size()));
    }
    check(!engine::extractBundleEntry(tamperedPath, "tri", extracted),
          "bundle: a flipped payload byte fails extraction (CRC integrity)");
    {
        std::ofstream junk(dir / "junk.gbq", std::ios::binary | std::ios::trunc);
        junk << "this is not a bundle at all";
    }
    engine::BundleInfo unusedInfo;
    check(!engine::inspectBundle(dir / "junk.gbq", unusedInfo) &&
              !engine::inspectBundle(dir / "definitely-missing.gbq", unusedInfo),
          "bundle: garbage and missing files are refused at inspection");

    // --- Missing references fail atomically ------------------------------------------------
    {
        engine::Scene orphan;
        auto ghost = orphan.createEntity("Ghost");
        ghost.addComponent<TransformComponent>(glm::vec3{0.0f});
        auto& ghostMesh = ghost.addComponent<MeshComponent>("ghostMesh");
        ghostMesh.materialName = "ghostMat";
        engine::VramBudget v2{64ull << 20};
        engine::VramBudget s2{64ull << 20};
        engine::AssetManager emptyAssets(v2, s2);
        const fs::path orphanPath = dir / "orphan.gbq";
        const engine::ExportReport failed = engine::ExportProject(orphan, emptyAssets, orphanPath);
        bool namesBoth = failed.missing.size() == 2;
        for (const std::string& name : failed.missing) {
            namesBoth = namesBoth && (name == "ghostMesh" || name == "ghostMat");
        }
        check(!failed.ok && namesBoth, "export: unregistered references are all reported by name");
        check(!fs::exists(orphanPath), "export: a failed export writes nothing (atomic failure)");
    }

    // --- Determinism -------------------------------------------------------------------------
    {
        std::vector<char> before(bundleBytes.size());
        std::memcpy(before.data(), bundleBytes.data(), before.size());
        const engine::ExportReport again = engine::ExportProject(scene, assets, bundlePath);
        std::ifstream in(bundlePath, std::ios::binary);
        const std::vector<char> after{std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{}};
        check(again.ok && after == before, "export: re-exporting the same scene is byte-identical");
    }

    // --- Runtime boot (the player's load path) ---------------------------------------------------
    engine::VramBudget vram2{64ull << 20};
    engine::VramBudget staging2{64ull << 20};
    engine::AssetManager runtimeAssets(vram2, staging2);
    engine::Scene runtimeScene;
    const engine::RuntimeLoadResult load =
        engine::loadBundleIntoRuntime(bundlePath, runtimeAssets, runtimeScene);
    check(load.ok && load.error.empty() && load.entityCount == 3 && runtimeScene.entityCount() == 3,
          "runtime: the bundle boots into a fresh asset registry + scene");
    check(load.scripts.size() == 1 && load.scripts.front() == "Phase33ProbeScript",
          "runtime: the bundle reports the native script classes the scene requires");
    check(engine::ScriptRegistry::instance().has("Phase33ProbeScript"),
          "runtime: this build provides the required script class (player check passes)");
    const engine::MeshAsset* rtMesh = runtimeAssets.mesh("tri");
    const engine::TextureAsset* rtTex = runtimeAssets.texture("texA");
    const engine::MaterialAsset* rtMat = runtimeAssets.material("matA");
    const engine::ShaderAsset* rtShader = runtimeAssets.shader("a.vert.spv");
    check(rtMesh != nullptr && rtMesh->vertices.size() == 3 && rtMesh->indices.size() == 3,
          "runtime: the packed mesh registers with its geometry");
    check(rtTex != nullptr && rtTex->width == 2 && rtTex->pixels == texPixels,
          "runtime: the packed texture registers with its pixels");
    check(rtShader != nullptr && rtShader->code == vertWords,
          "runtime: the packed shader registers with its SPIR-V words");
    check(rtMat != nullptr && rtMat->desc.baseColor == glm::vec4{0.2f, 0.4f, 0.6f, 1.0f} &&
              rtMat->desc.pbr.metallic == 0.75f && rtMat->desc.pbr.roughness == 0.3f &&
              rtMat->desc.pbr.albedoTexture == "texA" && rtMat->desc.vertexShader == "a.vert.spv",
          "runtime: the packed material registers with its PBR/shader/texture wiring");

    // --- Headless simulation over the booted scene -----------------------------------------------
    {
        entt::entity crateHandle = entt::null;
        {
            auto& registry = runtimeScene.registry();
            for (const entt::entity entity : registry.view<engine::TagComponent>()) {
                if (registry.get<engine::TagComponent>(entity).tag == "Crate") {
                    crateHandle = entity;
                }
            }
        }
        check(crateHandle != entt::null, "runtime: the exported crate resolves in the booted scene");
        engine::PhysicsSystem physics;
        const engine::PhysicsSettings settings{};
        const float startY = crateHandle != entt::null
                                 ? runtimeScene.registry().get<TransformComponent>(crateHandle).position.y
                                 : 0.0f;
        for (int step = 0; step < 90; ++step) {
            engine::TransformSystem::update(runtimeScene);
            physics.update(runtimeScene, 1.0 / 60.0, settings, nullptr);
            engine::RotationSystem::update(runtimeScene, 1.0 / 60.0);
            engine::TransformSystem::update(runtimeScene);
        }
        const glm::vec3 rest = crateHandle != entt::null
                                   ? runtimeScene.registry().get<TransformComponent>(crateHandle).position
                                   : glm::vec3{0.0f};
        check(std::isfinite(rest.x) && std::isfinite(rest.y) && std::isfinite(rest.z),
              "runtime: 90 headless physics steps keep every transform finite");
        check(startY == 2.0f && rest.y > 0.9f && rest.y < 1.1f,
              "runtime: the crate falls from the exported scene and rests on the floor");
    }

    engine::Scene tamperedScene;
    engine::VramBudget vram3{64ull << 20};
    engine::VramBudget staging3{64ull << 20};
    engine::AssetManager tamperedAssets(vram3, staging3);
    check(!engine::loadBundleIntoRuntime(tamperedPath, tamperedAssets, tamperedScene).ok,
          "runtime: a tampered bundle refuses to boot");

    // The fixture bundle (dir/game.gbq) intentionally stays on disk:
    // it is what `game-player --bundle <fixture> --headless 120`
    // smoke-tests end to end.
}

void testAsyncAssetPipeline() {
    std::cout << "Async asset pipeline (Phase 20)\n";
    using engine::AssetKind;
    using engine::AssetManager;
    using engine::AssetState;
    using engine::JobSystem;
    using engine::VramBudget;

    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "engine_phase20_assets";
    fs::create_directories(dir);
    const auto writeText = [](const fs::path& path, const std::string& content) {
        std::ofstream file(path, std::ios::binary);
        file << content;
    };
    writeText(dir / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n");
    writeText(dir / "quad.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nf 1 2 3\nf 1 3 4\n");
    {
        std::ofstream ppm(dir / "tex.ppm", std::ios::binary);
        ppm << "P6\n2 1\n255\n";
        const std::array<char, 6> pixels{static_cast<char>(255), 0, 0, 0, static_cast<char>(255), 0};
        ppm.write(pixels.data(), static_cast<std::streamsize>(pixels.size()));
    }

    VramBudget vram{64ull << 20};
    VramBudget staging{64ull << 20};
    AssetManager assets{vram, staging};

    // --- Handles, fallbacks, synchronous texture registration ---------------------
    check(assets.mesh(engine::kInvalidAssetId) == nullptr && assets.mesh(9999) == nullptr &&
              assets.mesh("nope") == nullptr && assets.texture(engine::kInvalidAssetId) == nullptr,
          "invalid asset handles and unknown names resolve to nullptr");
    check(assets.fallbackMesh().name == "__fallbackMesh" && !assets.fallbackMesh().vertices.empty() &&
              assets.fallbackMesh().vertices.front().color == glm::vec3{1.0f, 0.0f, 1.0f} &&
              assets.fallbackTexture().width == 2 && assets.fallbackTexture().pixels.size() == 16,
          "built-in fallbacks: magenta cube mesh and 2x2 checker texture");
    check(assets.meshOrFallback("nope") == &assets.fallbackMesh() &&
              assets.textureOrFallback("nope") == &assets.fallbackTexture(),
          "OrFallback accessors serve fallbacks for unknown assets");
    const std::array<uint8_t, 4> onePixel{10, 20, 30, 40};
    const engine::TextureId solidId = assets.registerTexture("solid", 1, 1, onePixel);
    check(solidId != engine::kInvalidAssetId && assets.texture("solid") != nullptr &&
              assets.texture("solid")->pixels[2] == 30 && assets.textureState("solid") == AssetState::Loaded &&
              assets.registerTexture("bad", 2, 2, onePixel) == engine::kInvalidAssetId,
          "synchronous texture registration validates payload size and registers by name");

    // --- Async load lifecycle with a deterministic in-flight window -----------------
    JobSystem jobs{JobSystem::Config{2, 0}};
    assets.setJobSystem(&jobs);
    std::vector<std::pair<std::string, AssetState>> events;
    assets.setAssetEventCallback([&events](std::string_view name, AssetKind, AssetState state) {
        events.emplace_back(std::string{name}, state);
    });

    std::atomic<bool> releaseWorkers{false};
    std::atomic<int> blockersRunning{0};
    for (int i = 0; i < 2; ++i) {
        jobs.submit([&releaseWorkers, &blockersRunning] {
            blockersRunning.fetch_add(1);
            while (!releaseWorkers.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
    }
    while (blockersRunning.load(std::memory_order_acquire) < 2) {
        std::this_thread::yield();
    }

    check(assets.declareMeshSource("tri", dir / "tri.obj") && !assets.declareMeshSource("tri", dir / "tri.obj"),
          "declaring the same asset source twice is rejected");
    const bool triRequested = assets.requestLoad("tri");
    const bool triRequestedAgain = assets.requestLoad("tri");
    check(triRequested && !triRequestedAgain && assets.meshState("tri") == AssetState::Loading &&
              assets.mesh("tri") == nullptr && assets.meshOrFallback("tri") == &assets.fallbackMesh(),
          "in-flight load reports Loading, serves the fallback, and rejects duplicate requests");

    releaseWorkers.store(true, std::memory_order_release);
    jobs.waitForAll();
    check(assets.meshState("tri") == AssetState::Loading && assets.pendingUploadCount() == 1 &&
              assets.mesh("tri") == nullptr,
          "finished CPU work still waits for the main-thread sync boundary");
    check(assets.processCompletedLoads() == 1 && assets.meshState("tri") == AssetState::Loaded &&
              assets.pendingUploadCount() == 0,
          "processCompletedLoads publishes the finished load");
    const engine::MeshAsset* tri = assets.mesh("tri");
    check(tri != nullptr && tri->vertices.size() == 3 && tri->indices.size() == 3 &&
              assets.meshOrFallback("tri") == tri,
          "the loaded mesh carries the OBJ geometry and replaces the fallback");
    const auto hasEvent = [&events](const char* name, AssetState state) {
        return std::any_of(events.begin(), events.end(), [&](const auto& event) {
            return event.first == name && event.second == state;
        });
    };
    check(hasEvent("tri", AssetState::Loading) && hasEvent("tri", AssetState::Loaded),
          "asset events fire for Loading (request) and Loaded (sync boundary)");

    // --- Failed loads and async textures ---------------------------------------------
    const bool missingDeclared = assets.declareMeshSource("missing", dir / "nope.obj");
    const bool missingRequested = missingDeclared && assets.requestLoad("missing");
    const bool texDeclared = assets.declareTextureSource("tex", dir / "tex.ppm");
    const bool texRequested = texDeclared && assets.requestLoad("tex");
    jobs.waitForAll();
    assets.processCompletedLoads();
    check(missingRequested && assets.meshState("missing") == AssetState::Failed &&
              !assets.assetError("missing").empty() &&
              assets.meshOrFallback("missing") == &assets.fallbackMesh() && hasEvent("missing", AssetState::Failed),
          "a missing file ends in Failed with an error and keeps serving the fallback");
    const engine::TextureAsset* tex = assets.texture("tex");
    check(texRequested && assets.textureState("tex") == AssetState::Loaded && tex != nullptr && tex->width == 2 &&
              tex->height == 1 && tex->pixels[0] == 255 && tex->pixels[3] == 255 && tex->pixels[4] == 0 &&
              tex->pixels[5] == 255 && assets.textureOrFallback("tex") == tex,
          "PPM texture loads asynchronously with exact RGBA pixels");
    check(!assets.requestLoad("neverDeclared"), "requesting an undeclared asset is rejected");

    // --- Hot reload through the file watcher -------------------------------------------
    const engine::MeshId triId = tri->id;
    writeText(dir / "tri.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nv 0 0 1\nf 1 2 3\nf 1 3 4\n");
    std::error_code mtimeError;
    fs::last_write_time(dir / "tri.obj", fs::file_time_type::clock::now() + std::chrono::seconds(5), mtimeError);
    const size_t triggered = assets.pollFileChanges();
    jobs.waitForAll();
    assets.processCompletedLoads();
    check(triggered == 1 && assets.meshState("tri") == AssetState::Loaded && assets.mesh("tri") != nullptr &&
              assets.mesh("tri")->id == triId && assets.mesh("tri")->indices.size() == 6,
          "mtime polling hot-reloads the changed mesh in place under its stable id");
    check(assets.pollFileChanges() == 0, "an unchanged file does not retrigger the watcher");

    // --- Inline mode (no JobSystem) and bounded processing --------------------------------
    {
        VramBudget soloVram{64ull << 20};
        VramBudget soloStaging{64ull << 20};
        AssetManager solo{soloVram, soloStaging};
        solo.declareMeshSource("a", dir / "tri.obj");
        solo.declareMeshSource("b", dir / "tri.obj");
        solo.declareMeshSource("c", dir / "tri.obj");
        const bool ra = solo.requestLoad("a");
        const bool rb = solo.requestLoad("b");
        const bool rc = solo.requestLoad("c");
        check(ra && rb && rc && solo.meshState("a") == AssetState::Loading && solo.pendingUploadCount() == 3,
              "without a JobSystem loads run inline but still respect the sync boundary");
        check(solo.processCompletedLoads(2) == 2 && solo.pendingUploadCount() == 1 &&
                  solo.processCompletedLoads() == 1 && solo.meshState("c") == AssetState::Loaded,
              "processCompletedLoads honors its per-call budget");
    }

    // --- Concurrent submission stress -------------------------------------------------------
    {
        VramBudget bulkVram{256ull << 20};
        VramBudget bulkStaging{256ull << 20};
        AssetManager bulk{bulkVram, bulkStaging};
        JobSystem bulkJobs{JobSystem::Config{4, 0}};
        bulk.setJobSystem(&bulkJobs);
        constexpr int kAssets = 16;
        for (int i = 0; i < kAssets; ++i) {
            bulk.declareMeshSource("bulk" + std::to_string(i), dir / "tri.obj");
        }
        std::atomic<int> requestCount{0};
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&bulk, &requestCount, t] {
                for (int i = t * 4; i < t * 4 + 4; ++i) {
                    if (bulk.requestLoad("bulk" + std::to_string(i))) {
                        requestCount.fetch_add(1);
                    }
                    (void)bulk.meshState("bulk" + std::to_string(i)); // concurrent state polling
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
        bulkJobs.waitForAll();
        const size_t processed = bulk.processCompletedLoads();
        bool allLoaded = processed == static_cast<size_t>(kAssets);
        for (int i = 0; i < kAssets; ++i) {
            allLoaded = allLoaded && bulk.meshState("bulk" + std::to_string(i)) == AssetState::Loaded;
        }
        check(requestCount.load() == kAssets && allLoaded && bulk.meshCount() == static_cast<size_t>(kAssets),
              "16 assets submitted from 4 threads all load and register exactly once");
    }

    fs::remove_all(dir);
}

} // namespace

void testPhase21CullingAndLighting() {
    std::cout << "Phase 21: culling, CSM & dynamic lights\n";

    // --- (1) Cascaded shadow split scheme -----------------------------
    {
        const auto splits = engine::computeCascadeSplits(0.1f, 100.0f, 4, 0.7f);
        check(nearlyEqual(splits[3], 100.0, 1e-3), "csm: last cascade ends at the far plane");
        check(splits[0] > 0.1f && splits[0] < splits[1] && splits[1] < splits[2] && splits[2] < splits[3],
              "csm: cascade splits strictly increase");
        // Log-heavy blend: the first slice is far tighter than uniform.
        check(splits[0] < 25.0f, "csm: first cascade slice is tight (uniform would give 25.0m)");

        const auto cascades = engine::computeShadowCascades(
            glm::vec3{0.0f, 0.0f, 3.0f}, glm::vec3{0.0f, 0.0f, -1.0f}, glm::vec3{0.0f, 1.0f, 0.0f},
            glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 100.0f, glm::vec3{0.35f, 0.8f, 0.45f}, 4, 0.7f, 2048);
        bool allWellFormed = true;
        for (size_t i = 0; i < cascades.size(); ++i) {
            const glm::mat4& m = cascades[i].lightViewProj;
            for (int col = 0; col < 4; ++col) {
                for (int row = 0; row < 4; ++row) {
                    allWellFormed = allWellFormed && std::isfinite(m[col][row]);
                }
            }
            allWellFormed = allWellFormed && std::abs(m[3][3]) > 1e-6f &&
                            nearlyEqual(cascades[i].splitFarDepth, splits[i], 1e-3);
        }
        check(allWellFormed, "csm: every cascade matrix is finite and matches its split depth");

        // The camera origin maps inside the first cascade's light
        // frustum (the slice surrounds it).
        const glm::vec4 clipped = cascades[0].lightViewProj * glm::vec4{0.0f, 0.0f, 3.0f, 1.0f};
        check(clipped.w > 0.0f && std::abs(clipped.x / clipped.w) <= 1.0001f &&
                  std::abs(clipped.y / clipped.w) <= 1.0001f,
              "csm: camera position projects inside cascade 0");

        // Texel density: for the same world distance, projecting a
        // point slightly further from the camera stays inside the
        // owning cascade near the sweet spot.
        const glm::vec4 nearClip = cascades[0].lightViewProj * glm::vec4{0.0f, 0.0f, 0.0f, 1.0f};
        check(nearClip.w > 0.0f && std::abs(nearClip.x / nearClip.w) <= 1.0001f,
              "csm: frustum-center point projects inside cascade 0");
    }

    // --- (2) BVH frustum query ----------------------------------------
    {
        engine::Scene scene;
        engine::CameraConfig cameraConfig;
        cameraConfig.startPosition = glm::vec3{0.0f, 0.0f, 3.0f};
        engine::Entity player = scene.createEntity("Player");
        player.addComponent<engine::CameraComponent>(cameraConfig);

        const auto addCube = [&scene](const char* name, const glm::vec3& position) {
            engine::Entity cube = scene.createEntity(name);
            cube.addComponent<engine::TransformComponent>(position);
            cube.addComponent<engine::MeshComponent>("cube");
            return cube;
        };
        engine::Entity visible = addCube("Visible", glm::vec3{0.0f, 0.0f, -5.0f});
        addCube("Behind", glm::vec3{0.0f, 0.0f, 10.0f});
        addCube("FarRight", glm::vec3{60.0f, 0.0f, -5.0f});

        const engine::Camera& camera = player.getComponent<engine::CameraComponent>().camera;
        const engine::Frustum frustum{camera.projectionMatrix(16.0f / 9.0f) * camera.viewMatrix()};
        const std::vector<entt::entity> hits = scene.queryFrustum(frustum);
        auto contains = [&hits](const engine::Entity& e) {
            return std::find(hits.begin(), hits.end(), e.handle()) != hits.end();
        };
        check(hits.size() == 1 && contains(visible), "bvh queryFrustum returns exactly the visible entity");
    }

    // --- (3) Occluder unit test + occlusion integration ---------------
    {
        engine::CameraConfig cameraConfig;
        cameraConfig.startPosition = glm::vec3{0.0f, 0.0f, 0.0f};
        const engine::Camera camera(cameraConfig); // origin, looking -Z
        const glm::mat4 vp = camera.projectionMatrix(16.0f / 9.0f) * camera.viewMatrix();

        // The camera's projection flips NDC Y for Vulkan, so rect
        // comparisons still hold per-axis; this 2m wall screens the
        // hidden cube exactly while the peeking cube's NDC rect
        // (-0.64..-0.45) clears the wall's (-0.39..0.39).
        const engine::AABB wall{glm::vec3{-1.0f, -1.0f, -4.5f}, glm::vec3{1.0f, 1.0f, -3.5f}};
        const engine::AABB hiddenBehind{glm::vec3{-0.5f, -0.5f, -10.5f}, glm::vec3{0.5f, 0.5f, -9.5f}};
        // 4m left of the axis: clear of the wall's screen rect even
        // after perspective widening (verified against projected NDC).
        const engine::AABB peeking{glm::vec3{-4.5f, -0.5f, -10.5f}, glm::vec3{-3.5f, 0.5f, -9.5f}};
        const engine::AABB inFrontOfWall{glm::vec3{-0.5f, -0.5f, -2.5f}, glm::vec3{0.5f, 0.5f, -1.5f}};
        check(engine::isOccludedBy(vp, wall, hiddenBehind), "occlusion: cube fully behind a wall is occluded");
        check(!engine::isOccludedBy(vp, wall, peeking), "occlusion: cube sticking out sideways stays visible");
        check(!engine::isOccludedBy(vp, wall, inFrontOfWall), "occlusion: cube in front of the wall stays visible");
        check(!engine::isOccludedBy(vp, hiddenBehind, wall), "occlusion: the wall itself is not occluded by what hides behind it");
    }

    {
        engine::Scene scene;
        engine::CameraConfig cameraConfig;
        cameraConfig.startPosition = glm::vec3{0.0f, 0.0f, 3.0f};
        engine::Entity player = scene.createEntity("Player");
        player.addComponent<engine::CameraComponent>(cameraConfig);

        // Static wall with a box collider (also drawn as a cube mesh).
        engine::Entity wall = scene.createEntity("Wall");
        auto& wallTransform = wall.addComponent<engine::TransformComponent>(glm::vec3{0.0f, 0.0f, -6.0f});
        wallTransform.scale = glm::vec3{6.0f, 6.0f, 1.0f};
        wall.addComponent<engine::MeshComponent>("cube");
        auto& wallBody = wall.addComponent<engine::RigidBodyComponent>();
        wallBody.setStatic(true);
        wall.addComponent<engine::ColliderComponent>(engine::ColliderComponent::box(glm::vec3{0.5f}));

        engine::Entity hidden = scene.createEntity("HiddenCube");
        hidden.addComponent<engine::TransformComponent>(glm::vec3{0.0f, 0.0f, -12.0f});
        hidden.addComponent<engine::MeshComponent>("cube");

        engine::Entity visibleBeside = scene.createEntity("VisibleCube");
        visibleBeside.addComponent<engine::TransformComponent>(glm::vec3{-5.0f, 0.0f, -12.0f});
        visibleBeside.addComponent<engine::MeshComponent>("cube");

        engine::FramePrepareOptions options;
        options.frustumCulling = true;
        const engine::PreparedFrame blind = engine::RenderSystem::prepareFrame(scene, 16.0f / 9.0f, options);
        check(blind.meshCount == 3 && blind.occludedCount == 0 && blind.renderQueue.size() == 3,
              "occlusion off: wall and both cubes are drawn");

        options.occlusionCulling = true;
        const engine::PreparedFrame culled = engine::RenderSystem::prepareFrame(scene, 16.0f / 9.0f, options);
        check(culled.meshCount == 3 && culled.culledCount == 0, "occlusion on: nothing frustum-culled here");
        check(culled.occludedCount == 1, "occlusion on: the hidden cube is occlusion-culled");
        check(culled.renderQueue.size() == 2, "occlusion on: wall and visible cube remain");
        check(culled.renderQueue.size() + culled.occludedCount + culled.culledCount == culled.meshCount,
              "occlusion on: drawn + occluded + culled accounts for every mesh");
    }

    // --- (4) Dynamic-light gathering ----------------------------------
    {
        engine::Scene scene;
        engine::CameraConfig cameraConfig;
        cameraConfig.startPosition = glm::vec3{0.0f, 0.0f, 3.0f};
        engine::Entity player = scene.createEntity("Player");
        player.addComponent<engine::CameraComponent>(cameraConfig);

        const auto addLight = [&scene](const glm::vec3& position, float intensity) {
            engine::Entity light = scene.createEntity("PointLight");
            light.addComponent<engine::TransformComponent>(position);
            auto& component = light.addComponent<engine::PointLightComponent>();
            component.intensity = intensity;
            component.color = glm::vec3{1.0f, 0.5f, 0.25f};
            component.range = 9.0f;
            return light;
        };
        addLight(glm::vec3{0.0f, 1.0f, 0.0f}, 10.0f);   // nearest to the camera
        addLight(glm::vec3{0.0f, 0.0f, -30.0f}, 20.0f); // farthest
        addLight(glm::vec3{0.0f, 0.0f, -5.0f}, 30.0f);  // middle

        engine::Entity spotEntity = scene.createEntity("Spot");
        spotEntity.addComponent<engine::TransformComponent>(glm::vec3{0.0f, 4.0f, -2.0f});
        auto& spot = spotEntity.addComponent<engine::SpotLightComponent>();
        spot.innerConeRadians = 0.3f;
        spot.outerConeRadians = 0.9f;

        const engine::SceneLightData lighting = engine::LightingSystem::gather(scene);
        check(lighting.lightCounts.x == 3.0f && lighting.lightCounts.y == 1.0f && lighting.lightCounts.z == 4.0f,
              "lights: three points plus one spot are packed");
        check(lighting.lightPositionRange[0].x == 0.0f && lighting.lightPositionRange[0].y == 1.0f &&
                  lighting.lightPositionRange[0].z == 0.0f && lighting.lightPositionRange[0].w == 9.0f,
              "lights: nearest point light is packed first at its position and range");
        check(lighting.lightColorIntensity[0].w == 10.0f, "lights: first slot holds the nearest light's intensity");
        size_t spotIndex = 99;
        for (size_t i = 0; i < 4; ++i) {
            if (lighting.lightDirectionType[i].w == 1.0f) {
                spotIndex = i;
            }
        }
        check(spotIndex != 99, "lights: a spot slot is marked with type 1");
        check(lighting.lightDirectionType[spotIndex].x == 0.0f && lighting.lightDirectionType[spotIndex].y == 0.0f &&
                  nearlyEqual(lighting.lightDirectionType[spotIndex].z, -1.0, 1e-4),
              "lights: default-orientation spot shines along -Z");

        // Over-budget scenes keep only the 16 nearest lights.
        for (int i = 0; i < 20; ++i) {
            addLight(glm::vec3{static_cast<float>(i) * 10.0f, 0.0f, -100.0f}, 5.0f);
        }
        const engine::SceneLightData capped = engine::LightingSystem::gather(scene);
        check(capped.lightCounts.z == 16.0f && capped.lightCounts.x == 15.0f && capped.lightCounts.y == 1.0f,
              "lights: scene is capped at the 16 nearest lights");

        // Disabled lights are skipped entirely.
        scene.registry().view<engine::PointLightComponent>().each(
            [](engine::PointLightComponent& light) { light.enabled = false; });
        scene.registry().view<engine::SpotLightComponent>().each(
            [](engine::SpotLightComponent& light) { light.enabled = false; });
        const engine::SceneLightData empty = engine::LightingSystem::gather(scene);
        check(empty.lightCounts.z == 0.0f, "lights: disabled lights are not gathered");
    }

    // --- (5) Attenuation + spot-cone mirror ---------------------------
    {
        {
            const double r = 1.0 / 10.0;
            const double windowed = std::pow(std::max(0.0, 1.0 - std::pow(r, 4.0)), 2.0);
            check(nearlyEqual(engine::lightAttenuation(1.0f, 10.0f), windowed / 2.0, 1e-5),
                  "attenuation: d=1 matches the closed-form windowed value");
        }
        check(engine::lightAttenuation(10.0f, 10.0f) == 0.0f, "attenuation: zero at the light range");
        check(engine::lightAttenuation(25.0f, 10.0f) == 0.0f, "attenuation: zero beyond the light range");
        check(engine::lightAttenuation(1.0f, 10.0f) > engine::lightAttenuation(3.0f, 10.0f) &&
                  engine::lightAttenuation(3.0f, 10.0f) > engine::lightAttenuation(6.0f, 10.0f) &&
                  engine::lightAttenuation(6.0f, 10.0f) > 0.0f,
              "attenuation: monotonically decreasing before the cutoff");
        check(engine::lightAttenuation(5.0f, 0.0f) == 0.0f, "attenuation: zero range is dark");

        check(nearlyEqual(engine::spotConeFactor(1.0f, 0.3f, 0.9f), 1.0, 1e-9), "spot: on-axis is full strength");
        check(nearlyEqual(engine::spotConeFactor(std::cos(0.9f), 0.3f, 0.9f), 0.0, 1e-4),
              "spot: at the outer cone edge the factor is zero");
        const float midway = engine::spotConeFactor(std::cos(0.6f), 0.3f, 0.9f);
        check(midway > 0.0f && midway < 1.0f, "spot: between the cones the factor fades smoothly");

        // UBO layout contract: view+proj (128) + 4 cascade matrices
        // (256) + three vec4s + environmentParams (64) + 4x16 light
        // vec4s (1024) = 1472 (Phase 22 added the environment vec4).
        // Phase 32 appends directionalColor (16) + spot shadow
        // matrix (64) + spotShadowParams (16) at the end, so every
        // pre-existing offset is unchanged: 1568 total.
        check(sizeof(engine::UniformBufferObject) == static_cast<size_t>(128 + 256 + 64 + 1024 + 96) &&
                  offsetof(engine::UniformBufferObject, environmentParams) == 1456 &&
                  offsetof(engine::UniformBufferObject, directionalColor) == 1472 &&
                  offsetof(engine::UniformBufferObject, spotShadowViewProj) == 1488 &&
                  offsetof(engine::UniformBufferObject, spotShadowParams) == 1552,
              "ubo: C++ layout matches the GLSL byte budget (1568 bytes, Phase 32 fields appended)");
    }
}

void testPhase22PbrIblPost() {
    std::cout << "Phase 22: PBR, IBL & post-processing\n";

    // --- (1) Cook-Torrance BRDF terms ----------------------------------
    {
        // GGX distribution: closed form at NoH = 1, roughness = 0.5 is
        // alpha^2 / (pi * alpha^4) = 16/pi with alpha = roughness^2.
        check(nearlyEqual(engine::distributionGGX(1.0f, 0.5f), 16.0 / 3.14159265358979, 1e-3),
              "pbr: GGX distribution matches its closed form at the peak");
        check(engine::distributionGGX(1.0f, 0.5f) > engine::distributionGGX(0.5f, 0.5f) &&
                  engine::distributionGGX(0.5f, 0.5f) > engine::distributionGGX(0.1f, 0.5f),
              "pbr: GGX distribution falls off away from the half-vector");
        check(nearlyEqual(engine::geometrySmith(1.0f, 1.0f, 0.4f), 1.0, 1e-6),
              "pbr: Smith geometry is 1 at normal incidence");
        const float smith = engine::geometrySmith(0.3f, 0.4f, 0.7f);
        check(smith > 0.0f && smith <= 1.0f, "pbr: Smith geometry stays inside (0, 1]");

        const glm::vec3 f0{0.04f};
        check(glm::length(engine::fresnelSchlick(1.0f, f0) - f0) < 1e-6f, "pbr: Fresnel is F0 at normal incidence");
        check(nearlyEqual(engine::fresnelSchlick(0.0f, f0).x, 1.0, 1e-6), "pbr: Fresnel reaches 1 at grazing");
        check(nearlyEqual(engine::fresnelSchlickRoughness(0.0f, f0, 0.5f).x, 0.5, 1e-5),
              "pbr: roughness-aware Fresnel caps the grazing value");

        const glm::vec3 up{0.0f, 1.0f, 0.0f};
        const glm::vec3 white{1.0f};
        // Dielectric head-on: diffuse + a small dielectric specular.
        const glm::vec3 dielectric = engine::evaluateDirectLight(up, up, up, white, white, 0.0f, 0.5f);
        check(dielectric.x > 0.30f && dielectric.x < 0.42f && std::isfinite(dielectric.x),
              "pbr: dielectric head-on response matches the analytic sum");
        // Pure metal tinted red: no green diffuse at all, reflections
        // carry the albedo tint.
        const glm::vec3 metal = engine::evaluateDirectLight(up, up, up, white, glm::vec3{1.0f, 0.0f, 0.0f}, 1.0f, 0.5f);
        check(metal.y == 0.0f && metal.x > 0.0f, "pbr: metals have no diffuse and tint their reflections");
        // Back-facing light contributes nothing.
        const glm::vec3 backlit = engine::evaluateDirectLight(up, up, glm::vec3{0.0f, -1.0f, 0.0f}, white, white,
                                                              0.0f, 0.5f);
        check(backlit == glm::vec3{0.0f}, "pbr: back-facing lights are dark");
        // Sharp metals spike; rough metals spread and dim.
        const glm::vec3 sharp = engine::evaluateDirectLight(up, up, up, white, white, 1.0f, 0.2f);
        const glm::vec3 rough = engine::evaluateDirectLight(up, up, up, white, white, 1.0f, 0.9f);
        check(sharp.x > rough.x * 10.0f, "pbr: low roughness concentrates the specular peak");
    }

    // --- (2) Tone mapping + bright-pass --------------------------------
    {
        check(engine::acesFilmic(glm::vec3{0.0f}) == glm::vec3{0.0f}, "aces: black stays black");
        check(nearlyEqual(engine::acesFilmic(glm::vec3{1.0f}).x, 2.54 / 3.16, 1e-3),
              "aces: unit exposure matches the Narkowicz fit");
        check(engine::acesFilmic(glm::vec3{0.2f}).x < engine::acesFilmic(glm::vec3{0.8f}).x &&
                  engine::acesFilmic(glm::vec3{0.8f}).x < engine::acesFilmic(glm::vec3{2.0f}).x,
              "aces: monotonic over the working range");
        const float hot = engine::acesFilmic(glm::vec3{100.0f}).x;
        check(hot > 0.99f && hot <= 1.0f, "aces: HDR highlights roll off to white");

        check(engine::linearToSrgb(0.0f) == 0.0f && nearlyEqual(engine::linearToSrgb(1.0f), 1.0, 1e-6),
              "srgb: endpoints are exact");
        check(nearlyEqual(engine::linearToSrgb(0.5f), 0.73535698, 1e-4), "srgb: midpoint matches the standard");
        check(nearlyEqual(engine::srgbToLinear(engine::linearToSrgb(0.35f)), 0.35, 1e-5),
              "srgb: encode/decode round-trips");
        check(nearlyEqual(engine::linearToSrgb(0.002f), 0.002 * 12.92, 1e-7), "srgb: linear toe segment");

        check(engine::extractBright(glm::vec3{0.5f}, 1.0f, 0.5f) == glm::vec3{0.0f},
              "bloom: sub-threshold colors are rejected");
        const glm::vec3 bright = engine::extractBright(glm::vec3{4.0f}, 1.0f, 0.5f);
        check(nearlyEqual(bright.x, 3.0, 1e-4), "bloom: soft-knee weight matches the closed form");
        check(engine::extractBright(glm::vec3{2.0f, 3.0f, 4.0f}, 0.0f, 0.5f) == glm::vec3{2.0f, 3.0f, 4.0f},
              "bloom: above the knee band colors pass unweighted");
    }

    // --- (3) Environment generation -------------------------------------
    {
        // Cube mapping round-trips through face selection.
        bool roundTrips = true;
        const glm::vec3 directions[] = {glm::vec3{1, 0, 0},  glm::vec3{-1, 0, 0}, glm::vec3{0, 1, 0},
                                        glm::vec3{0, -1, 0}, glm::vec3{0, 0, 1},  glm::vec3{0, 0, -1},
                                        glm::normalize(glm::vec3{1, 2, 3}), glm::normalize(glm::vec3{-0.3f, 0.8f, -0.52f})};
        for (const glm::vec3& direction : directions) {
            uint32_t face = 0;
            float u = 0.0f;
            float v = 0.0f;
            engine::directionToCubeFace(direction, face, u, v);
            roundTrips = roundTrips && glm::length(engine::cubeFaceDirection(face, u, v) - direction) < 1e-4f;
        }
        check(roundTrips, "env: cube face mapping round-trips for axis and diagonal directions");

        // Constant environment: every convolution reproduces it exactly.
        engine::CubeMapData constant;
        constant.size = 8;
        const glm::vec3 c{0.3f, 0.5f, 0.7f};
        for (auto& faceData : constant.faces) {
            faceData.assign(64, c);
        }
        const engine::CubeMapData constIrradiance = engine::generateIrradiance(constant, 4, 256);
        bool irradianceConstant = constIrradiance.size == 4;
        for (const auto& faceData : constIrradiance.faces) {
            for (const glm::vec3& texel : faceData) {
                irradianceConstant = irradianceConstant && glm::length(texel - c) < 1e-5f;
            }
        }
        check(irradianceConstant, "env: irradiance of a constant environment is the constant");
        const std::vector<engine::CubeMapData> constPrefilter = engine::generatePrefiltered(constant, 8, 3, 128);
        bool prefilterConstant = constPrefilter.size() == 3;
        for (const engine::CubeMapData& level : constPrefilter) {
            for (const auto& faceData : level.faces) {
                for (const glm::vec3& texel : faceData) {
                    prefilterConstant = prefilterConstant && glm::length(texel - c) < 1e-4f;
                }
            }
        }
        check(prefilterConstant, "env: prefiltered levels of a constant environment are the constant");

        // BRDF LUT: bounded, mirror-like at smooth+head-on, deterministic.
        const std::vector<glm::vec2> lut = engine::integrateBrdfLut(16, 256);
        bool lutBounded = lut.size() == 256;
        for (const glm::vec2& entry : lut) {
            lutBounded = lutBounded && entry.x >= 0.0f && entry.x <= 1.001f && entry.y >= 0.0f &&
                          entry.y <= 1.001f && std::isfinite(entry.x) && std::isfinite(entry.y);
        }
        check(lutBounded, "env: BRDF LUT entries stay inside [0, 1]");
        const glm::vec2 smooth = lut[15];        // roughness ~0.03, NoV ~0.97
        const glm::vec2 rough = lut[15 * 16 + 15]; // roughness ~0.97, NoV ~0.97
        check(smooth.x > 0.85f && smooth.y < 0.2f, "env: smooth head-on LUT is nearly (1, 0)");
        check(rough.x < smooth.x, "env: roughness reduces the LUT specular scale");
        check(engine::integrateBrdfLut(8, 128) == engine::integrateBrdfLut(8, 128),
              "env: LUT generation is deterministic");

        // Procedural sky: the sun disc is an HDR peak; up beats down.
        const glm::vec3 sunDirection = engine::directionalLightDirection();
        const float peak = engine::skyRadiance(sunDirection, sunDirection).x;
        const float side = engine::skyRadiance(glm::vec3{1.0f, 0.0f, 0.0f}, sunDirection).x;
        check(peak > 8.0f && peak > side * 5.0f, "env: sky has an HDR sun peak (feeds bloom)");
        check(engine::skyRadiance(glm::vec3{0.0f, 1.0f, 0.0f}, sunDirection).z >
                  engine::skyRadiance(glm::vec3{0.0f, -1.0f, 0.0f}, sunDirection).z,
              "env: sky zenith is brighter than the ground bounce");

        // Prefiltering blurs: texel variance shrinks with roughness.
        const engine::CubeMapData sky = engine::generateEnvironmentCube(16, sunDirection);
        const std::vector<engine::CubeMapData> skyPrefilter = engine::generatePrefiltered(sky, 16, 4, 192);
        const auto faceVariance = [](const engine::CubeMapData& map, uint32_t face) {
            glm::vec3 mean{0.0f};
            for (const glm::vec3& texel : map.faces[face]) {
                mean += texel;
            }
            mean /= static_cast<float>(map.faces[face].size());
            float variance = 0.0f;
            for (const glm::vec3& texel : map.faces[face]) {
                const glm::vec3 delta = texel - mean;
                variance += glm::dot(delta, delta);
            }
            return variance / static_cast<float>(map.faces[face].size());
        };
        check(faceVariance(skyPrefilter.front(), 2) > faceVariance(skyPrefilter.back(), 2),
              "env: higher roughness levels are blurrier (variance drops)");
        const engine::CubeMapData skyIrradiance = engine::generateIrradiance(sky, 8, 256);
        const glm::vec3 upIrradiance = skyIrradiance.texel(2, 4, 4);
        const glm::vec3 downIrradiance = skyIrradiance.texel(3, 4, 4);
        check(glm::dot(upIrradiance, glm::vec3{1.0f}) > glm::dot(downIrradiance, glm::vec3{1.0f}),
              "env: up-facing irradiance exceeds down-facing (sky dome)");
    }

    // --- (4) Material -> draw item PBR mapping --------------------------
    {
        engine::VramBudget vram(64 * 1024 * 1024);
        engine::VramBudget staging(64 * 1024 * 1024);
        engine::AssetManager assets(vram, staging);

        const std::vector<uint8_t> pixels(2 * 2 * 4, 200);
        const uint64_t versionBefore = assets.textureRegistryVersion();
        check(assets.registerTexture("albedoTex", 2, 2, pixels) == 1, "pbr: texture registers with dense id 1");
        check(assets.textureRegistryVersion() > versionBefore && assets.textures().size() == 1,
              "pbr: texture registry version bumps and the table is enumerable");

        engine::MaterialDesc desc;
        desc.name = "pbrMat";
        desc.baseColor = glm::vec4{0.8f, 0.3f, 0.2f, 1.0f};
        desc.pbr.metallic = 0.8f;
        desc.pbr.roughness = 0.3f;
        desc.pbr.occlusionStrength = 0.7f;
        desc.pbr.albedoTexture = "albedoTex";
        desc.pbr.metallicRoughnessTexture = "notRegistered";
        check(assets.registerMaterial(desc) == 1, "pbr: material registers");

        engine::Scene scene;
        engine::Entity player = scene.createEntity("Player");
        player.addComponent<engine::CameraComponent>(engine::CameraConfig{});
        engine::Entity cube = scene.createEntity("Cube");
        cube.addComponent<engine::TransformComponent>(glm::vec3{0.0f, 0.0f, -5.0f});
        auto& mesh = cube.addComponent<engine::MeshComponent>("cube");
        mesh.materialName = "pbrMat";

        engine::FramePrepareOptions options;
        options.assets = &assets;
        const engine::PreparedFrame frame = engine::RenderSystem::prepareFrame(scene, 16.0f / 9.0f, options);
        check(frame.renderQueue.size() == 1, "pbr: textured material produces a draw item");
        if (frame.renderQueue.size() == 1) {
            const engine::DrawItem& item = frame.renderQueue.items()[0];
            check(nearlyEqual(item.pbrParams.x, 0.8f, 1e-6) && nearlyEqual(item.pbrParams.y, 0.3f, 1e-6) &&
                      nearlyEqual(item.pbrParams.z, 0.7f, 1e-6),
                  "pbr: metallic/roughness/occlusion reach the draw item");
            check(item.textureIndices[0] == 0 && item.textureIndices[1] == -1 && item.textureIndices[2] == -1 &&
                      item.textureIndices[3] == -1,
                  "pbr: albedo resolves to slot 0; unknown textures stay untextured");
        }

        // Sandbox palette PBR character (demo wiring).
        check(engine::makeSandboxFloorMaterial().pbr.metallic == 0.0f &&
                  nearlyEqual(engine::makeSandboxFloorMaterial().pbr.roughness, 0.95f, 1e-6) &&
                  nearlyEqual(engine::makeSandboxCubeMaterial().pbr.metallic, 0.9f, 1e-6),
              "pbr: sandbox materials carry metallic-roughness identities");

        // Post defaults + config parsing for the new [Render] keys.
        const engine::PostSettings post;
        check(post.bloomEnabled && post.skyboxEnabled && nearlyEqual(post.exposure, 1.0f, 1e-6),
              "post: default settings are cinematic-on with unit exposure");
        const auto dir = std::filesystem::temp_directory_path() / "engine_p22_cfg";
        std::filesystem::create_directories(dir);
        const auto path = dir / "engine.ini";
        {
            std::ofstream out(path);
            out << "[Render]\nexposure = 2.5\nbloomEnabled = false\nbloomStrength = 1.25\n"
                   "skyboxEnabled = false\nenvironmentIntensity = 0.5\n";
        }
        const engine::EngineConfig parsed = engine::EngineConfig::loadFromFile(path);
        check(nearlyEqual(parsed.render.exposure, 2.5) && !parsed.render.bloomEnabled &&
                  nearlyEqual(parsed.render.bloomStrength, 1.25) && !parsed.render.skyboxEnabled &&
                  nearlyEqual(parsed.render.environmentIntensity, 0.5),
              "post: [Render] post-processing keys parse from the INI file");
    }
}


// --- Phase 25 fixtures -------------------------------------------------------

std::vector<std::string> g_scriptEvents;
int g_probeScriptAlive = 0;
std::string g_probeScriptEntityName;

class ProbeScript final : public engine::INativeScript {
public:
    ProbeScript() { ++g_probeScriptAlive; }
    ~ProbeScript() override { --g_probeScriptAlive; }
    void onAwake() override { g_scriptEvents.push_back("awake"); }
    void onStart() override {
        g_scriptEvents.push_back("start");
        g_probeScriptEntityName = entity().name();
    }
    void onUpdate(float) override { g_scriptEvents.push_back("update"); }
    void onFixedUpdate(float) override { g_scriptEvents.push_back("fixed"); }
    void onDestroy() override { g_scriptEvents.push_back("destroy"); }
};
REGISTER_SCRIPT(ProbeScript)

struct EditorWidgetSample {
    int32_t count = 0;
    uint32_t flags = 0;
    float weight = 0.0f;
    bool active = false;
    glm::vec3 position{0.0f};
};

ENGINE_REFLECT_BEGIN(EditorWidgetSample)
    ENGINE_REFLECT_PROPERTY(count, "Count")
    ENGINE_REFLECT_PROPERTY(flags, "Flags")
    ENGINE_REFLECT_PROPERTY(weight, "Weight")
    ENGINE_REFLECT_PROPERTY(active, "Active")
    ENGINE_REFLECT_PROPERTY(position, "Position")
ENGINE_REFLECT_END()

void testPhase23GameModules() {
    std::cout << "Phase 23: game modules & hot reload\n";

    const auto dir = std::filesystem::temp_directory_path() / "engine_p23_modules";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path builtPlugin = ENGINE_TEST_PLUGIN_PATH;
    // Phase 24: module files must follow the branded naming policy.
    const std::filesystem::path pluginSource = dir / ("EngineRuntimeCore_mock" + builtPlugin.extension().string());
    std::filesystem::copy_file(builtPlugin, pluginSource);
    const auto statePath = dir / "module_state.txt";
    const std::string statePathString = statePath.string();

    const auto readLines = [&statePath] {
        std::vector<std::string> lines;
        std::ifstream in(statePath);
        std::string line;
        while (std::getline(in, line)) {
            lines.push_back(line);
        }
        return lines;
    };
    const auto countPrefix = [](const std::vector<std::string>& lines, const std::string& prefix) {
        size_t count = 0;
        for (const std::string& line : lines) {
            if (line.rfind(prefix, 0) == 0) {
                ++count;
            }
        }
        return count;
    };

    engine::Scene scene;
    engine::EngineContext context;
    context.scene = &scene;
    context.userData = const_cast<char*>(statePathString.c_str());

    // --- (1) Raw dynamic library --------------------------------------
    {
        engine::DynamicLibrary library;
        std::string error;
        check(library.load(pluginSource, error) && library.isLoaded(), "dynlib: test plugin loads");
        check(library.symbol("CreateGameModule") != nullptr && library.symbol("DestroyGameModule") != nullptr,
              "dynlib: factory symbols resolve by name");
        check(library.symbol("NoSuchSymbol") == nullptr, "dynlib: unknown symbols resolve to null");
        library.unload();
        check(!library.isLoaded(), "dynlib: unload releases the handle");
        check(!library.load(dir / "absent.so", error) && !error.empty(),
              "dynlib: a missing file fails with an OS error message");
    }

    // --- (2) Manager lifecycle through the scratch copy ----------------
    engine::ModuleManager manager;
    manager.update(context, 0.016f); // safe no-op with nothing loaded
    check(!manager.load(dir / "absent.so", context) && !manager.lastError().empty() && !manager.isLoaded(),
          "manager: missing module file is rejected gracefully");
    check(manager.load(pluginSource, context), "manager: plugin loads");
    check(manager.isLoaded() && manager.generation() == 1 && manager.moduleName() == "MockGameModule",
          "manager: reports the module name and first generation");
    check(std::filesystem::exists(manager.scratchPath()) && manager.scratchPath() != pluginSource,
          "manager: the loaded library is a scratch copy, not the compiler output");
    const std::filesystem::path firstScratch = manager.scratchPath();

    context.frameIndex = 1;
    manager.update(context, 0.016f);
    context.frameIndex = 2;
    manager.update(context, 0.016f);
    manager.fixedUpdate(context, 1.0f / 60.0f);
    {
        const std::vector<std::string> lines = readLines();
        check(countPrefix(lines, "load") == 1 && countPrefix(lines, "update") == 2 &&
                  countPrefix(lines, "fixed") == 1,
              "module: lifecycle calls cross the library boundary");
        double totalDt = 0.0;
        uint64_t lastFrame = 0;
        bool framesAscending = true;
        for (const std::string& line : lines) {
            if (line.rfind("update ", 0) == 0) {
                const size_t split = line.find(' ', 7);
                const uint64_t frame = std::stoull(line.substr(7, split - 7));
                totalDt += std::stod(line.substr(split + 1));
                framesAscending = framesAscending && frame > lastFrame;
                lastFrame = frame;
            }
        }
        check(framesAscending && nearlyEqual(totalDt, 0.032, 1e-5),
              "module: updates see monotonic frame indices and real deltas");
    }

    // --- (3) Hot reload: scene survives, scratch rotates ----------------
    engine::Entity persistent = scene.createEntity("Persistent");
    persistent.addComponent<engine::TransformComponent>(glm::vec3{1.0f, 2.0f, 3.0f});
    const entt::entity persistentHandle = persistent.handle();

    // Rebuilding over the source must succeed while the engine runs:
    // the loader holds the scratch copy, never the compiler output.
    std::filesystem::copy_file(builtPlugin, pluginSource, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::last_write_time(
        pluginSource, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(5));
    check(manager.pollForChanges(context), "manager: mtime poll hot-reloads the rebuilt module");
    check(manager.generation() == 2 && manager.scratchPath() != firstScratch &&
              !std::filesystem::exists(firstScratch),
          "manager: reload rotates to a fresh scratch and deletes the old one");
    check(scene.registry().valid(persistentHandle) &&
              scene.registry().get<engine::TransformComponent>(persistentHandle).position ==
                  glm::vec3{1.0f, 2.0f, 3.0f},
          "manager: engine-owned entities survive a module reload untouched");
    {
        const std::vector<std::string> lines = readLines();
        check(countPrefix(lines, "load") == 2 && countPrefix(lines, "unload") == 1,
              "module: reload unloads the old generation exactly once");
    }
    check(!manager.pollForChanges(context), "manager: no reload without a newer build");

    // --- (4) Broken rebuild: previous generation keeps running ----------
    std::filesystem::copy_file(ENGINE_TEST_BAD_PLUGIN_PATH, pluginSource,
                               std::filesystem::copy_options::overwrite_existing);
    std::filesystem::last_write_time(
        pluginSource, std::filesystem::file_time_type::clock::now() + std::chrono::seconds(30));
    check(!manager.pollForChanges(context), "manager: a module without the engine handshake is rejected");
    check(manager.isLoaded() && manager.generation() == 2 && !manager.lastError().empty(),
          "manager: the previous generation keeps running after a bad rebuild");
    context.frameIndex = 3;
    manager.update(context, 0.016f);
    check(countPrefix(readLines(), "update") == 3, "module: surviving generation still updates");

    // --- (5) Throwing initialization ------------------------------------
    {
        engine::ModuleManager throwing;
        check(!throwing.load(ENGINE_TEST_THROW_PLUGIN_PATH, context) && !throwing.isLoaded() &&
                  !throwing.lastError().empty(),
              "manager: a throwing onLoad is caught and rejected");
        std::filesystem::copy_file(builtPlugin, pluginSource, std::filesystem::copy_options::overwrite_existing);
        check(throwing.load(pluginSource, context) && throwing.isLoaded(),
              "manager: the host stays usable after a failed load");
    }
    check(countPrefix(readLines(), "load") == 3, "module: recovered manager loads the good module");

    // --- (6) Unload: marker written, scratch removed ---------------------
    const std::filesystem::path secondScratch = manager.scratchPath();
    manager.unload();
    check(!manager.isLoaded() && !std::filesystem::exists(secondScratch),
          "manager: unload removes the scratch copy");
    {
        const std::vector<std::string> lines = readLines();
        // Loads: generation 1, generation 2, the recovered manager.
        // Unloads: generation 1's retirement, the recovered manager's
        // scope exit, and this explicit unload — one per load.
        check(countPrefix(lines, "unload") == 3 && countPrefix(lines, "load") == 3,
              "module: every load is paired with exactly one unload");
    }
}

void testPhase24SpaceFlight() {
    std::cout << "Phase 24: branded modules & Newtonian space flight\n";

    // --- (a) Engine-branded naming policy ------------------------------
    check(engine::ModuleManager::isEngineBrandedModuleName("EngineRuntimeCore_0123abcd.so"),
          "branding: conventional .so name accepted");
    check(engine::ModuleManager::isEngineBrandedModuleName("EngineRuntimeCore_x.dll") &&
              engine::ModuleManager::isEngineBrandedModuleName("EngineRuntimeCore_x.dylib"),
          "branding: .dll and .dylib names accepted");
    check(!engine::ModuleManager::isEngineBrandedModuleName("game_logic.so"),
          "branding: generic names rejected");
    check(!engine::ModuleManager::isEngineBrandedModuleName("EngineRuntimeCore_.so"),
          "branding: an empty signature is rejected");
    check(!engine::ModuleManager::isEngineBrandedModuleName("EngineRuntimeCore_bad name.so") &&
              !engine::ModuleManager::isEngineBrandedModuleName("engineruntimecore_x.so") &&
              !engine::ModuleManager::isEngineBrandedModuleName("EngineRuntimeCore_x.txt"),
          "branding: malformed names rejected");

    const std::filesystem::path shipPlugin = ENGINE_TEST_SPACEFLIGHT_PLUGIN_PATH;
    check(engine::ModuleManager::isEngineBrandedModuleName(shipPlugin),
          "branding: the shipped flight module follows the convention");

    const auto dir = std::filesystem::temp_directory_path() / "engine_p24_space";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::filesystem::path brandedCopy = dir / shipPlugin.filename();
    std::filesystem::copy_file(shipPlugin, brandedCopy);

    engine::Scene scene;
    engine::Input input;
    engine::VramBudget vram(16 * 1024 * 1024);
    engine::VramBudget staging(16 * 1024 * 1024);
    engine::AssetManager assets(vram, staging);
    engine::EngineContext context;
    context.scene = &scene;
    context.input = &input;
    context.assets = &assets;
    context.scripts = &engine::ScriptRegistry::instance(); // Phase 25 script bridge

    const auto findByTag = [&scene](const char* tag) -> entt::entity {
        auto view = scene.registry().view<engine::TagComponent>();
        for (const entt::entity entity : view) {
            if (view.get<engine::TagComponent>(entity).tag == tag) {
                return entity;
            }
        }
        return entt::null;
    };
    const auto countByTag = [&scene](const char* tag) {
        size_t count = 0;
        auto view = scene.registry().view<engine::TagComponent>();
        for (const entt::entity entity : view) {
            if (view.get<engine::TagComponent>(entity).tag == tag) {
                ++count;
            }
        }
        return count;
    };

    // --- (b) Handshake validation ---------------------------------------
    engine::ModuleManager manager;
    check(!manager.load(ENGINE_TEST_FORGED_PLUGIN_PATH, context) && !manager.isLoaded() &&
              manager.lastError().find("token") != std::string::npos,
          "handshake: a forged ABI token is rejected before factories run");
    check(!manager.load(ENGINE_TEST_LEGACY_PLUGIN_PATH, context) &&
              manager.lastError().find("ABI version mismatch") != std::string::npos,
          "handshake: a stale ABI version is rejected");
    check(!manager.load(ENGINE_TEST_BAD_PLUGIN_PATH, context) &&
              manager.lastError().find("handshake") != std::string::npos,
          "handshake: a module without GetEngineModuleInfo is rejected");
    const std::filesystem::path unbranded = dir / ("game_logic" + shipPlugin.extension().string());
    std::filesystem::copy_file(shipPlugin, unbranded);
    check(!manager.load(unbranded, context) && manager.lastError().find("branded") != std::string::npos,
          "branding: a valid binary under a generic name is rejected");

    // --- (c) Flight model math (the plugin's own header) ------------------
    {
        const spaceflight::FlightParams params;
        const glm::vec3 inertia{900.0f, 900.0f, 600.0f};
        const spaceflight::FlightForces idle =
            spaceflight::computeFlightForces(params, spaceflight::FlightInput{}, glm::vec3{0.0f}, inertia);
        check(idle.forceLocal == glm::vec3{0.0f} && idle.torqueLocal == glm::vec3{0.0f},
              "flight model: no input in zero-g means no force and no torque");

        spaceflight::FlightInput burn;
        burn.main = 1.0f;
        const spaceflight::FlightForces thrust =
            spaceflight::computeFlightForces(params, burn, glm::vec3{0.0f}, inertia);
        check(nearlyEqual(thrust.forceLocal.z, -24000.0, 1e-3) && thrust.forceLocal.x == 0.0f &&
                  thrust.mainThrottle == 1.0f && thrust.totalThrottle == 1.0f,
              "flight model: full main throttle is 24 kN down the -Z ship axis");

        spaceflight::FlightInput pitchUp;
        pitchUp.pitch = 1.0f;
        const spaceflight::FlightForces pitched =
            spaceflight::computeFlightForces(params, pitchUp, glm::vec3{0.0f}, inertia);
        check(nearlyEqual(pitched.torqueLocal.x, 4200.0, 1e-3),
              "flight model: pitch demand maps to the pitch torque authority");

        const spaceflight::FlightForces damped = spaceflight::computeFlightForces(
            params, spaceflight::FlightInput{}, glm::vec3{0.0f, 2.0f, 0.0f}, inertia);
        check(nearlyEqual(damped.torqueLocal.y, -0.35 * 900.0 * 2.0, 1e-2),
              "flight model: the RCS damper opposes spin proportionally to I*omega");

        spaceflight::FlightParams rawParams = params;
        rawParams.angularDamping = 0.0f;
        const spaceflight::FlightForces drifting = spaceflight::computeFlightForces(
            rawParams, spaceflight::FlightInput{}, glm::vec3{1.0f, 2.0f, 3.0f}, inertia);
        check(drifting.torqueLocal == glm::vec3{0.0f},
              "flight model: with the damper off, angular momentum is conserved exactly");

        // Closed-loop: F = ma integrated for one second of main burn.
        const float acceleration = params.mainThrust / spaceflight::kShipMass;
        float velocity = 0.0f;
        for (int i = 0; i < 60; ++i) {
            velocity += acceleration * (1.0f / 60.0f);
        }
        check(nearlyEqual(velocity, 20.0, 1e-3), "flight model: 24 kN on 1200 kg gains 20 m/s in one second");
    }

    // --- (d) The real module, driving the real physics --------------------
    check(manager.load(brandedCopy, context) && manager.moduleName() == "SpaceFlightModule" &&
              manager.moduleAbiVersion() == engine::kEngineModuleAbiVersion,
          "flight module loads under its branded name with a valid handshake");

    const entt::entity ship = findByTag("Spacecraft");
    check(ship != entt::null, "flight module spawns the spacecraft");
    auto& shipBody = scene.registry().get<engine::RigidBodyComponent>(ship);
    check(!shipBody.useGravity && nearlyEqual(shipBody.mass, spaceflight::kShipMass, 1e-3),
          "spacecraft is a gravity-free body with the module's mass");
    check(countByTag("Asteroid") == static_cast<size_t>(spaceflight::kAsteroidCount),
          "flight module spawns the procedural asteroid field");
    check(countByTag("ThrusterNozzleMain") == 1 && countByTag("ThrusterNozzlePort") == 1 &&
              countByTag("ThrusterNozzleStarboard") == 1,
          "thruster nozzle lights are placed");

    engine::PhysicsSystem physics;
    engine::PhysicsSettings settings;
    settings.gravity = glm::vec3{0.0f}; // deep space
    const double fixedDt = 1.0 / 60.0;
    const auto stepFlight = [&](int steps) {
        for (int i = 0; i < steps; ++i) {
            context.fixedDeltaSeconds = static_cast<float>(fixedDt);
            manager.fixedUpdate(context, static_cast<float>(fixedDt));
            engine::NativeScriptSystem::fixedUpdate(context, static_cast<float>(fixedDt)); // Phase 25: script-driven flight
            physics.update(scene, fixedDt, settings);
        }
    };

    stepFlight(60);
    check(glm::length(shipBody.velocity) < 1e-3f && glm::length(scene.registry().get<engine::TransformComponent>(ship).position) < 1e-3f,
          "zero-g: a full second without input produces no drift");

    input.injectKeyState(engine::Key::W, true);
    stepFlight(30); // 0.5 s main burn
    check(shipBody.velocity.z < -8.5f && shipBody.velocity.z > -11.5f,
          "main burn: ~10 m/s of forward velocity after half a second");
    check(std::abs(shipBody.velocity.x) < 0.25f && std::abs(shipBody.velocity.y) < 0.25f,
          "main burn: velocity stays on the ship axis");
    const glm::vec3 velocityAfterBurn = shipBody.velocity;

    context.elapsedSeconds = 1.0;
    manager.update(context, 0.016f);
    engine::NativeScriptSystem::update(context, 0.016f); // script pulses the nozzle lights
    const entt::entity mainNozzle = findByTag("ThrusterNozzleMain");
    check(mainNozzle != entt::null &&
              scene.registry().get<engine::PointLightComponent>(mainNozzle).intensity > 12.0f,
          "thruster nozzle light pulses up with throttle");

    input.injectKeyState(engine::Key::W, false);
    stepFlight(60); // 1 s coast
    check(nearlyEqual(glm::length(shipBody.velocity), glm::length(velocityAfterBurn), 0.05),
          "zero-g coast: engines off, velocity conserved (no drag)");

    input.injectKeyState(engine::Key::Q, true);
    stepFlight(60); // 1 s roll burn
    check(glm::length(shipBody.angularVelocity) > 30.0f, "RCS roll torque spins the ship up");
    input.injectKeyState(engine::Key::Q, false);

    // --- (e) Hot reload mid-flight -----------------------------------------
    const glm::vec3 velocityBeforeReload = shipBody.velocity;
    std::filesystem::copy_file(shipPlugin, brandedCopy, std::filesystem::copy_options::overwrite_existing);
    std::filesystem::last_write_time(brandedCopy,
                                     std::filesystem::file_time_type::clock::now() + std::chrono::seconds(30));
    check(manager.pollForChanges(context) && manager.generation() == 2, "flight module hot-reloads mid-flight");
    check(countByTag("Asteroid") == static_cast<size_t>(spaceflight::kAsteroidCount) &&
              countByTag("Spacecraft") == 1 && countByTag("ThrusterNozzleMain") == 1,
          "reload adopts the existing sandbox instead of duplicating it");
    check(glm::length(shipBody.velocity - velocityBeforeReload) < 1e-4f,
          "ship velocity is bit-stable across the reload");
    input.injectKeyState(engine::Key::W, true);
    stepFlight(30);
    check(shipBody.velocity.z < velocityBeforeReload.z - 5.0f,
          "the reloaded generation flies the same ship immediately");
    input.injectKeyState(engine::Key::W, false);

    manager.unload();
    check(countByTag("Spacecraft") == 1 && countByTag("Asteroid") == static_cast<size_t>(spaceflight::kAsteroidCount),
          "unloading the module leaves the engine-owned world intact");
}

void testPhase25ReflectionScripting() {
    std::cout << "Phase 25: reflection & native scripting\n";

    // --- (a) Reflection traits & offsets --------------------------------
    const engine::TypeInfo& rigid = engine::reflect<engine::RigidBodyComponent>();
    check(std::string_view(rigid.name) == "RigidBodyComponent" && rigid.properties.size() == 8,
          "reflection: RigidBodyComponent exposes its property table");
    const engine::PropertyInfo* mass = rigid.findProperty("mass");
    check(mass != nullptr && mass->type == engine::PropertyType::Float &&
              mass->offset == offsetof(engine::RigidBodyComponent, mass) &&
              std::string_view(mass->displayName) == "Mass (kg)",
          "reflection: mass carries type, display name and the true byte offset");
    const engine::TypeInfo& transformInfo = engine::reflect<engine::TransformComponent>();
    const engine::PropertyInfo* position = transformInfo.findProperty("position");
    check(position != nullptr && position->type == engine::PropertyType::Vec3,
          "reflection: vec3 fields are typed as Vec3");
    check(rigid.findProperty("useGravity") != nullptr &&
              rigid.findProperty("useGravity")->type == engine::PropertyType::Bool,
          "reflection: bool fields are typed as Bool");
    check(rigid.findProperty("noSuchField") == nullptr, "reflection: unknown properties resolve to null");

    {
        engine::RigidBodyComponent body;
        body.mass = 5.0f;
        check(nearlyEqual(engine::readProperty<float>(&body, *mass), 5.0),
              "reflection: values read back through the offset");
        engine::writeProperty<float>(&body, *mass, 42.0f);
        const engine::PropertyInfo* velocity = rigid.findProperty("velocity");
        engine::writeProperty<glm::vec3>(&body, *velocity, glm::vec3{1.0f, 2.0f, 3.0f});
        check(nearlyEqual(body.mass, 42.0) && body.velocity == glm::vec3{1.0f, 2.0f, 3.0f},
              "reflection: generic writes land in the real fields (editor-style editing)");
    }

    {
        const engine::TypeInfo& widget = engine::reflect<EditorWidgetSample>();
        check(widget.properties.size() == 5 && widget.findProperty("count")->type == engine::PropertyType::Int32 &&
                  widget.findProperty("flags")->type == engine::PropertyType::UInt32,
              "reflection: int32/uint32 fields are typed distinctly");
        EditorWidgetSample sample;
        engine::writeProperty<int32_t>(&sample, *widget.findProperty("count"), -7);
        engine::writeProperty<bool>(&sample, *widget.findProperty("active"), true);
        check(sample.count == -7 && sample.active, "reflection: POD fields of any reflected struct are writable");
    }

    // --- (b) Reflection registry -----------------------------------------
    engine::ReflectionRegistry& reflection = engine::ReflectionRegistry::instance();
    const engine::TypeInfo* registeredRigid = reflection.find("RigidBodyComponent");
    // Lookup is by name; the registered table's identity belongs to
    // whichever binary registered it (engine builtins register from
    // EngineScene), so content equality — not pointer identity — is
    // the contract across module boundaries.
    check(registeredRigid != nullptr && registeredRigid->name == rigid.name &&
              registeredRigid->properties.size() == rigid.properties.size() &&
              registeredRigid->findProperty("mass") != nullptr &&
              reflection.find("TransformComponent") != nullptr && reflection.find("NoSuchType") == nullptr,
          "reflection registry: built-in types are findable by name");
    {
        size_t totalProperties = 0;
        for (const engine::TypeInfo* type : reflection.types()) {
            totalProperties += type->properties.size();
        }
        const size_t typeCountBefore = reflection.types().size();
        engine::registerReflection<engine::RigidBodyComponent>(); // duplicate: replaces in place
        check(typeCountBefore >= 5 && totalProperties >= 20 && reflection.types().size() == typeCountBefore,
              "reflection registry: iteration covers all builtins; re-registration replaces, never duplicates");
    }

    // Flight parameters are reflected too — an editor edit changes physics.
    {
        spaceflight::FlightParams params;
        const engine::TypeInfo& paramsInfo = engine::reflect<spaceflight::FlightParams>();
        engine::writeProperty<float>(&params, *paramsInfo.findProperty("mainThrust"), 48000.0f);
        spaceflight::FlightInput burn;
        burn.main = 1.0f;
        const spaceflight::FlightForces forces =
            spaceflight::computeFlightForces(params, burn, glm::vec3{0.0f}, glm::vec3{900.0f});
        check(paramsInfo.properties.size() == 7 && nearlyEqual(forces.forceLocal.z, -48000.0, 1e-2),
              "reflection: writing FlightParams.mainThrust doubles the burn");
    }

    // --- (c) Script registry ----------------------------------------------
    engine::ScriptRegistry& scripts = engine::ScriptRegistry::instance();
    check(scripts.has("ProbeScript") && !scripts.has("NoSuchScript"),
          "scripts: REGISTER_SCRIPT self-registers by class name");
    {
        engine::INativeScript* raw = scripts.create("ProbeScript");
        check(raw != nullptr && g_probeScriptAlive == 1, "scripts: the factory instantiates by name");
        delete raw;
        check(g_probeScriptAlive == 0 && scripts.create("NoSuchScript") == nullptr,
              "scripts: unknown names instantiate to null");
    }

    // --- (d) Lifecycle order ------------------------------------------------
    {
        engine::Scene scene;
        engine::EngineContext context;
        context.scene = &scene;
        g_scriptEvents.clear();

        engine::Entity probe = scene.createEntity("Probe");
        probe.addComponent<engine::NativeScriptComponent>("ProbeScript");
        engine::NativeScriptSystem::update(context, 0.016f);
        check(g_scriptEvents == std::vector<std::string>{"awake", "start", "update"},
              "scripts: awake -> start -> update on the first tick");
        check(g_probeScriptAlive == 1 && g_probeScriptEntityName == "Probe",
              "scripts: the instance knows its entity and scene");
        engine::NativeScriptSystem::fixedUpdate(context, 1.0f / 60.0f);
        engine::NativeScriptSystem::update(context, 0.016f);
        check(g_scriptEvents == std::vector<std::string>{"awake", "start", "update", "fixed", "update"},
              "scripts: start fires exactly once; fixed and variable ticks interleave");

        auto& component = probe.getComponent<engine::NativeScriptComponent>();
        check(component.instance != nullptr && component.started && component.scriptName == "ProbeScript",
              "scripts: the component tracks instance, started state and serialization name");

        scene.destroyEntity(probe);
        check(g_scriptEvents.back() == "destroy" && g_probeScriptAlive == 0,
              "scripts: entity destruction runs onDestroy and deletes the instance");

        engine::Entity orphan = scene.createEntity("Orphan");
        orphan.addComponent<engine::NativeScriptComponent>("NoSuchScript");
        engine::NativeScriptSystem::update(context, 0.016f);
        check(orphan.getComponent<engine::NativeScriptComponent>().instance == nullptr,
              "scripts: an unregistered name stays dormant instead of crashing");
        engine::NativeScriptSystem::shutdown(scene);
    }

    // --- (e) The flight module, script-driven -------------------------------
    {
        const std::filesystem::path shipPlugin = ENGINE_TEST_SPACEFLIGHT_PLUGIN_PATH;
        const auto dir = std::filesystem::temp_directory_path() / "engine_p25_space";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        const std::filesystem::path brandedCopy = dir / shipPlugin.filename();
        std::filesystem::copy_file(shipPlugin, brandedCopy);

        engine::Scene scene;
        engine::Input input;
        engine::VramBudget vram(16 * 1024 * 1024);
        engine::VramBudget staging(16 * 1024 * 1024);
        engine::AssetManager assets(vram, staging);
        engine::EngineContext context;
        context.scene = &scene;
        context.input = &input;
        context.assets = &assets;
        context.scripts = &scripts;

        engine::ModuleManager manager;
        check(manager.load(brandedCopy, context) && scripts.has("SpaceFlightScript"),
              "flight module registers SpaceFlightScript with the host");

        entt::entity ship = entt::null;
        {
            auto view = scene.registry().view<engine::TagComponent>();
            for (const entt::entity entity : view) {
                if (view.get<engine::TagComponent>(entity).tag == "Spacecraft") {
                    ship = entity;
                }
            }
        }
        check(ship != entt::null && scene.registry().all_of<engine::NativeScriptComponent>(ship) &&
                  scene.registry().get<engine::NativeScriptComponent>(ship).scriptName == "SpaceFlightScript",
              "the ship carries a NativeScriptComponent naming the flight script");

        engine::PhysicsSystem physics;
        engine::PhysicsSettings settings;
        settings.gravity = glm::vec3{0.0f};
        const double fixedDt = 1.0 / 60.0;
        const auto stepScripts = [&](int steps) {
            for (int i = 0; i < steps; ++i) {
                context.fixedDeltaSeconds = static_cast<float>(fixedDt);
                engine::NativeScriptSystem::fixedUpdate(context, static_cast<float>(fixedDt));
                physics.update(scene, fixedDt, settings);
            }
        };
        auto& shipBody = scene.registry().get<engine::RigidBodyComponent>(ship);
        stepScripts(10);
        check(scene.registry().get<engine::NativeScriptComponent>(ship).instance != nullptr,
              "the script system instantiates the flight script on the ship");

        input.injectKeyState(engine::Key::W, true);
        stepScripts(30);
        check(shipBody.velocity.z < -8.5f && shipBody.velocity.z > -11.5f,
              "script-driven burn reaches the same ~10 m/s as the old module loop");
        input.injectKeyState(engine::Key::W, false);

        const glm::vec3 velocityBeforeReload = shipBody.velocity;
        std::filesystem::copy_file(shipPlugin, brandedCopy, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::last_write_time(brandedCopy,
                                         std::filesystem::file_time_type::clock::now() + std::chrono::seconds(30));
        check(manager.pollForChanges(context) && manager.generation() == 2, "flight module hot-reloads");
        stepScripts(10);
        check(scene.registry().get<engine::NativeScriptComponent>(ship).instance != nullptr &&
                  glm::length(shipBody.velocity - velocityBeforeReload) < 1e-3f,
              "after reload a fresh script instance adopts the same ship (velocity untouched, engines off)");
        input.injectKeyState(engine::Key::W, true);
        stepScripts(30);
        check(shipBody.velocity.z < velocityBeforeReload.z - 4.0f,
              "the reloaded script generation flies the same ship immediately");
        input.injectKeyState(engine::Key::W, false);
        engine::NativeScriptSystem::shutdown(scene);
        manager.unload();
    }
}

void testPhase26UIRenderer() {
    const glm::vec4 fill{0.2f, 0.4f, 0.8f, 0.9f};

    // --- (a) Batch basics: one rect becomes one quad ----------------------
    engine::UIBatch batch;
    check(batch.empty() && batch.vertices().empty() && batch.indices().empty() && batch.bounds().empty,
          "ui: a fresh batch is empty with empty bounds");
    batch.begin(1280, 720);
    check(batch.viewportWidth() == 1280 && batch.viewportHeight() == 720, "ui: begin records the viewport");
    batch.drawRect(10.0f, 20.0f, 100.0f, 50.0f, fill);
    check(batch.vertices().size() == 4 && batch.indices().size() == 6,
          "ui: one rect accumulates 4 vertices / 6 indices");
    {
        const std::vector<engine::UIVertex>& v = batch.vertices();
        check(v[0].position == glm::vec2{10.0f, 20.0f} && v[0].uv == glm::vec2{0.0f, 0.0f} &&
                  v[2].position == glm::vec2{110.0f, 70.0f} && v[2].uv == glm::vec2{1.0f, 1.0f},
              "ui: quad corners span the rect with 0..1 uv");
        check(v[0].color == fill && v[0].cornerRadius == 0.0f && v[0].borderWidth == 0.0f,
              "ui: solid rect carries fill color and no rounding/border");
        const std::vector<uint32_t>& idx = batch.indices();
        check(idx[0] == 0 && idx[1] == 1 && idx[2] == 2 && idx[3] == 0 && idx[4] == 2 && idx[5] == 3,
              "ui: indices form two triangles over the quad");
        const engine::UIBounds b = batch.bounds();
        check(!b.empty && b.minX == 10.0f && b.minY == 20.0f && b.maxX == 110.0f && b.maxY == 70.0f &&
                  b.width() == 100.0f && b.height() == 50.0f,
              "ui: batch bounds equal the single rect");
    }

    // --- (b) Accumulation + bounds union ----------------------------------
    batch.drawRect(-5.0f, 60.0f, 20.0f, 20.0f, fill);
    check(batch.vertices().size() == 8 && batch.indices().size() == 12, "ui: a second rect appends a second quad");
    check(batch.indices()[6] == 4 && batch.indices()[11] == 7, "ui: appended quad indices offset by the vertex base");
    {
        const engine::UIBounds b = batch.bounds();
        check(b.minX == -5.0f && b.minY == 20.0f && b.maxX == 110.0f && b.maxY == 80.0f,
              "ui: batch bounds are the union of all rects");
    }

    // --- (c) Rounding / border styling --------------------------------------
    batch.begin(800, 600);
    batch.drawRoundedRect(0.0f, 0.0f, 40.0f, 20.0f, 999.0f, fill);
    check(batch.vertices()[0].cornerRadius == 10.0f, "ui: corner radius clamps to half the short side");
    const glm::vec4 border{1.0f, 1.0f, 1.0f, 0.5f};
    batch.drawRoundedRect(0.0f, 30.0f, 30.0f, 30.0f, engine::UIRectStyle{6.0f, 999.0f, border}, fill);
    check(batch.vertices()[4].cornerRadius == 6.0f && batch.vertices()[4].borderWidth == 15.0f &&
              batch.vertices()[4].borderColor == border,
          "ui: styled rect carries radius, clamped border width and border color");

    // --- (d) Degenerate calls + the drawText stub ---------------------------
    const size_t vertsBefore = batch.vertices().size();
    batch.drawRect(0.0f, 0.0f, 0.0f, 10.0f, fill);
    batch.drawRoundedRect(0.0f, 0.0f, 10.0f, -3.0f, 2.0f, fill);
    check(batch.vertices().size() == vertsBefore, "ui: degenerate (zero/negative size) rects emit nothing");
    batch.drawText("FPS 60", 12.0f, 12.0f, 2.0f, fill);
    batch.drawText("", 0.0f, 0.0f, 1.0f, fill);
    check(batch.textItemCount() == 1 && batch.vertices().size() == vertsBefore,
          "ui: drawText stub records non-empty items but emits no geometry yet");

    // --- (e) Vertex input contract (pipeline creation safety) ---------------
    {
        const engine::UIRenderer::VertexInput input = engine::UIRenderer::vertexInput();
        check(input.binding.binding == 0 && input.binding.inputRate == VK_VERTEX_INPUT_RATE_VERTEX &&
                  input.binding.stride == sizeof(engine::UIVertex),
              "ui: vertex binding 0 is per-vertex with stride sizeof(UIVertex)");
        const std::array<VkVertexInputAttributeDescription, 8>& a = input.attributes;
        check(a[0].location == 0 && a[0].format == VK_FORMAT_R32G32_SFLOAT &&
                  a[0].offset == offsetof(engine::UIVertex, position) &&
                  a[1].location == 1 && a[1].format == VK_FORMAT_R32G32_SFLOAT &&
                  a[1].offset == offsetof(engine::UIVertex, uv) &&
                  a[2].location == 2 && a[2].format == VK_FORMAT_R32G32B32A32_SFLOAT &&
                  a[2].offset == offsetof(engine::UIVertex, color),
              "ui: position/uv/color attributes match the UIVertex layout");
        check(a[3].location == 3 && a[3].format == VK_FORMAT_R32_SFLOAT &&
                  a[3].offset == offsetof(engine::UIVertex, cornerRadius) &&
                  a[4].location == 4 && a[4].format == VK_FORMAT_R32_SFLOAT &&
                  a[4].offset == offsetof(engine::UIVertex, borderWidth) &&
                  a[5].location == 5 && a[5].format == VK_FORMAT_R32G32B32A32_SFLOAT &&
                  a[5].offset == offsetof(engine::UIVertex, borderColor),
              "ui: radius/border attributes match the UIVertex layout");
        check(a[6].location == 6 && a[6].format == VK_FORMAT_R32G32B32A32_SFLOAT &&
                  a[6].offset == offsetof(engine::UIVertex, uvRect) &&
                  a[7].location == 7 && a[7].format == VK_FORMAT_R32_SFLOAT &&
                  a[7].offset == offsetof(engine::UIVertex, kind),
              "ui: uvRect/kind attributes match the UIVertex layout");
    }

    // --- (f) Capacity truncation stays index-consistent ----------------------
    batch.begin(64, 64);
    while (!batch.truncated()) {
        batch.drawRect(0.0f, 0.0f, 4.0f, 4.0f, fill);
    }
    check(batch.vertices().size() == engine::UIBatch::kMaxVertices &&
              batch.indices().size() == engine::UIBatch::kMaxIndices,
          "ui: batch truncates exactly at capacity instead of overflowing");
    uint32_t maxIndex = 0;
    for (const uint32_t index : batch.indices()) {
        maxIndex = std::max(maxIndex, index);
    }
    check(maxIndex < batch.vertices().size(), "ui: every index references an emitted vertex");
    batch.begin(64, 64);
    check(batch.empty() && !batch.truncated() && batch.textItemCount() == 0,
          "ui: begin resets geometry, text items and truncation");

    // --- (g) SDF font atlas generation (Step 2) ------------------------------
    const engine::SdfFontAtlas atlas = engine::generateSdfFontAtlas();
    check(!atlas.empty() && atlas.width == 384 && atlas.height == 192 &&
              atlas.pixels.size() == static_cast<size_t>(atlas.width) * atlas.height,
          "ui sdf: atlas page bakes to 384x192 R8");
    const engine::SdfGlyph& glyphA = atlas.glyphFor('A');
    check(glyphA.u0 >= 0.0f && glyphA.v0 >= 0.0f && glyphA.u1 <= 1.0f && glyphA.v1 <= 1.0f &&
              glyphA.u1 > glyphA.u0 && glyphA.advance == 6.0f,
          "ui sdf: glyph rects sit inside the page with monospace advance");
    check(atlas.glyphFor('a').u0 == glyphA.u0 && atlas.glyphFor('\x01').u0 == atlas.glyphFor('?').u0,
          "ui sdf: lowercase folds to uppercase; unknown glyphs fall back to '?'");
    {
        // Inspect the 'A' cell: deep-interior texels, a populated ramp
        // band (a true distance field, not a binary mask), an outside
        // corner; and the space cell: entirely outside.
        const uint32_t cellX = static_cast<uint32_t>('A' - 32) % 16 * engine::kSdfCellWidth;
        const uint32_t cellY = static_cast<uint32_t>('A' - 32) / 16 * engine::kSdfCellHeight;
        uint32_t mid = 0;
        uint8_t deepest = 0;
        uint8_t spaceMax = 0;
        for (uint32_t y = 0; y < engine::kSdfCellHeight; ++y) {
            for (uint32_t x = 0; x < engine::kSdfCellWidth; ++x) {
                const uint8_t value = atlas.pixels[(cellY + y) * atlas.width + (cellX + x)];
                deepest = std::max(deepest, value);
                if (value > 64 && value < 192) {
                    ++mid;
                }
                spaceMax = std::max(spaceMax, atlas.pixels[y * atlas.width + x]); // space cell is (0,0)
            }
        }
        check(deepest >= 160 && mid > 0 && atlas.pixels[cellY * atlas.width + cellX] < 128 && spaceMax == 0,
              "ui sdf: field has interior, ramp and empty cells where expected");
    }

    // --- (h) Atlas save/load round-trip ----------------------------------------
    {
        const std::vector<uint8_t> blob = engine::saveSdfFontAtlas(atlas);
        engine::SdfFontAtlas loaded;
        check(engine::loadSdfFontAtlas(blob, loaded) && loaded.width == atlas.width &&
                  loaded.height == atlas.height && loaded.pixels == atlas.pixels &&
                  loaded.glyphFor('A').u1 == glyphA.u1 && loaded.advance == atlas.advance,
              "ui sdf: save/load round-trips page, glyph table and metrics");
        engine::SdfFontAtlas untouched;
        const std::vector<uint8_t> shortBlob(blob.begin(), blob.begin() + 20);
        std::vector<uint8_t> corrupt = blob;
        corrupt[0] = 'X';
        check(!engine::loadSdfFontAtlas(shortBlob, untouched) && !engine::loadSdfFontAtlas(corrupt, untouched) &&
                  untouched.empty(),
              "ui sdf: truncated/corrupt blobs are rejected without touching the output");
    }

    // --- (i) SDF text layout -----------------------------------------------------
    engine::UIBatch textBatch;
    textBatch.setFontAtlas(&atlas);
    textBatch.begin(1280, 720);
    check(textBatch.measureText("AB", 1.0f) == 12.0f && textBatch.measureText("", 3.0f) == 0.0f &&
              textBatch.measureText("ABC\nD", 2.0f) == 36.0f,
          "ui text: measureText reports the widest line in screen pixels");
    textBatch.drawText("AB", 100.0f, 50.0f, 1.0f, fill);
    check(textBatch.vertices().size() == 8 && textBatch.indices().size() == 12 && textBatch.textItemCount() == 1,
          "ui text: two glyphs emit two quads");
    {
        const std::vector<engine::UIVertex>& v = textBatch.vertices();
        const engine::SdfGlyph& glyphB = atlas.glyphFor('B');
        check(v[0].kind == engine::kUITextPrimitive &&
                  v[0].uvRect == glm::vec4{glyphA.u0, glyphA.v0, glyphA.u1, glyphA.v1},
              "ui text: glyph quad samples its atlas cell as SDF text");
        check(v[0].position == glm::vec2{99.5f, 49.5f}, "ui text: quad covers the cell halo (0.5 font px inset)");
        check(v[4].position.x - v[0].position.x == 6.0f && v[4].uvRect.x == glyphB.u0,
              "ui text: pen advances one monospace step per glyph");
        check(textBatch.segments().size() == 1 && textBatch.segments()[0].texture == engine::kUIFontTexture,
              "ui text: glyphs batch into one font-texture segment");
    }
    textBatch.drawText("A\nB", 0.0f, 0.0f, 2.0f, fill);
    check(textBatch.vertices()[12].position == glm::vec2{-1.0f, 19.0f},
          "ui text: newline resets the pen and steps one line height");

    // --- (j) Texture batching splits segments by texture id ----------------------
    engine::UIBatch texBatch;
    texBatch.begin(800, 600);
    texBatch.drawRect(0.0f, 0.0f, 10.0f, 10.0f, fill);
    texBatch.drawTexture(7, 20.0f, 0.0f, 10.0f, 10.0f, fill);
    texBatch.drawTexture(7, 40.0f, 0.0f, 10.0f, 10.0f, fill);
    texBatch.drawTexture(9, 60.0f, 0.0f, 10.0f, 10.0f, glm::vec4{0.25f, 0.5f, 0.75f, 1.0f}, fill);
    texBatch.drawRect(80.0f, 0.0f, 10.0f, 10.0f, fill);
    {
        const std::vector<engine::UISegment>& segs = texBatch.segments();
        check(segs.size() == 4 && segs[0].texture == engine::kUINoTexture && segs[1].texture == 7 &&
                  segs[2].texture == 9 && segs[3].texture == engine::kUINoTexture,
              "ui texture: consecutive same-texture quads coalesce into segments");
        check(segs[0].indexCount == 6 && segs[1].indexCount == 12 && segs[1].firstIndex == 6 &&
                  segs[2].firstIndex == 18 && segs[3].firstIndex == 24 && segs[3].indexCount == 6,
              "ui texture: segment index ranges tile the index buffer");
        const std::vector<engine::UIVertex>& v = texBatch.vertices();
        check(v[4].kind == engine::kUIImagePrimitive && v[12].uvRect == glm::vec4{0.25f, 0.5f, 0.75f, 1.0f},
              "ui texture: image quads carry kind + atlas sub-rect");
        check(texBatch.bounds().maxX == 90.0f, "ui texture: images contribute to batch bounds");
    }

    // --- (k) Scissor stack nesting ------------------------------------------------
    engine::UIBatch clipBatch;
    clipBatch.begin(800, 600);
    check(clipBatch.scissorDepth() == 1 && clipBatch.currentScissor() == engine::UIScissorRect{0, 0, 800, 600},
          "ui scissor: the root scissor is the full viewport");
    clipBatch.drawRect(0.0f, 0.0f, 10.0f, 10.0f, fill);
    clipBatch.pushScissor(100.0f, 50.0f, 300.0f, 200.0f);
    clipBatch.pushScissor(200.0f, 25.0f, 300.0f, 100.0f);
    check(clipBatch.scissorDepth() == 3 && clipBatch.currentScissor() == engine::UIScissorRect{200, 50, 200, 75},
          "ui scissor: nested pushes intersect with the parent rect");
    clipBatch.drawRect(0.0f, 0.0f, 10.0f, 10.0f, fill);
    clipBatch.popScissor();
    check(clipBatch.currentScissor() == engine::UIScissorRect{100, 50, 300, 200},
          "ui scissor: pop restores the parent rect");
    clipBatch.drawRect(0.0f, 0.0f, 10.0f, 10.0f, fill);
    clipBatch.popScissor();
    clipBatch.popScissor(); // root pop: no-op
    check(clipBatch.scissorDepth() == 1 && clipBatch.currentScissor() == engine::UIScissorRect{0, 0, 800, 600},
          "ui scissor: popping the root is a safe no-op");
    clipBatch.drawRect(0.0f, 0.0f, 10.0f, 10.0f, fill);
    {
        const std::vector<engine::UISegment>& segs = clipBatch.segments();
        const std::vector<engine::UIScissorRect>& rects = clipBatch.scissors();
        check(segs.size() == 4 && rects.size() == 3, "ui scissor: every push appends one effective rect");
        check(rects[segs[0].scissor] == engine::UIScissorRect{0, 0, 800, 600} &&
                  rects[segs[1].scissor] == engine::UIScissorRect{200, 50, 200, 75} &&
                  rects[segs[2].scissor] == engine::UIScissorRect{100, 50, 300, 200} &&
                  rects[segs[3].scissor] == engine::UIScissorRect{0, 0, 800, 600},
              "ui scissor: segments reference the scissor active at emit time");
    }
    clipBatch.pushScissor(1000.0f, 1000.0f, 10.0f, 10.0f);
    check(clipBatch.currentScissor().width == 0.0f && clipBatch.currentScissor().height == 0.0f,
          "ui scissor: a disjoint nested rect collapses to empty");
}

void testPhase26Widgets() {
    struct Rig {
        engine::SdfFontAtlas atlas = engine::generateSdfFontAtlas();
        engine::UIBatch batch;
        engine::UIContext ctx;
        Rig() { batch.setFontAtlas(&atlas); }
        void begin(float mouseX, float mouseY, bool mouseDown, int keyStep = 0) {
            batch.begin(800, 600);
            ctx.beginFrame(engine::UIInput{mouseX, mouseY, mouseDown, keyStep}, batch);
        }
        void end() { ctx.endFrame(); }
    };
    Rig rig;

    // --- (a) Button hover / press / release ---------------------------------
    const engine::UIRect fireRect{100.0f, 100.0f, 120.0f, 30.0f};
    rig.begin(0.0f, 0.0f, false);
    check(!rig.ctx.button(fireRect, "Fire") && rig.ctx.hoveredId() == engine::kUINoWidget,
          "widgets: no hover away from the button");
    rig.end();
    rig.begin(150.0f, 110.0f, false);
    check(!rig.ctx.button(fireRect, "Fire") && rig.ctx.hoveredId() != engine::kUINoWidget,
          "widgets: hovering the button reports its id");
    const engine::UIWidgetId fireId = rig.ctx.hoveredId();
    rig.end();
    rig.begin(150.0f, 110.0f, true);
    check(!rig.ctx.button(fireRect, "Fire") && rig.ctx.activeId() == fireId && rig.ctx.focusedId() == fireId,
          "widgets: press captures active + focus without firing");
    rig.end();
    rig.begin(150.0f, 110.0f, true);
    check(!rig.ctx.button(fireRect, "Fire") && rig.ctx.activeId() == fireId, "widgets: hold keeps the capture");
    rig.end();
    rig.begin(150.0f, 110.0f, false);
    check(rig.ctx.button(fireRect, "Fire") && rig.ctx.activeId() == engine::kUINoWidget,
          "widgets: release over the button fires the click");
    rig.end();
    rig.begin(150.0f, 110.0f, true);
    rig.ctx.button(fireRect, "Fire");
    rig.end();
    rig.begin(400.0f, 300.0f, true);
    check(!rig.ctx.button(fireRect, "Fire") && rig.ctx.activeId() == fireId &&
              rig.ctx.hoveredId() == engine::kUINoWidget,
          "widgets: dragging off keeps the capture but loses hover");
    rig.end();
    rig.begin(400.0f, 300.0f, false);
    check(!rig.ctx.button(fireRect, "Fire") && rig.ctx.activeId() == engine::kUINoWidget,
          "widgets: releasing off the button cancels the click");
    rig.end();
    check(engine::UIContext::hashId(7, "Fire") == engine::UIContext::hashId(7, "Fire") &&
              engine::UIContext::hashId(7, "Fire") != engine::UIContext::hashId(8, "Fire") &&
              engine::UIContext::hashId(7, "Fire") != engine::kUINoWidget,
          "widgets: ids are stable, seed-sensitive and never zero");

    // --- (b) Focus stealing -----------------------------------------------------
    const engine::UIRect alphaRect{50.0f, 50.0f, 120.0f, 30.0f};
    const engine::UIRect betaRect{50.0f, 100.0f, 120.0f, 30.0f};
    rig.begin(80.0f, 60.0f, true);
    rig.ctx.button(alphaRect, "Alpha");
    const engine::UIWidgetId alphaId = rig.ctx.focusedId();
    rig.end();
    check(alphaId != engine::kUINoWidget, "widgets: pressing Alpha focuses it");
    rig.begin(80.0f, 60.0f, false);
    rig.ctx.button(alphaRect, "Alpha");
    rig.end();
    rig.begin(80.0f, 110.0f, true);
    rig.ctx.button(betaRect, "Beta");
    check(rig.ctx.focusedId() != alphaId && rig.ctx.focusedId() != engine::kUINoWidget,
          "widgets: pressing Beta steals focus from Alpha");
    rig.end();
    rig.begin(80.0f, 110.0f, false);
    rig.ctx.button(betaRect, "Beta");
    rig.end();
    rig.begin(500.0f, 400.0f, true);
    rig.ctx.button(alphaRect, "Alpha");
    rig.ctx.button(betaRect, "Beta");
    rig.end();
    check(rig.ctx.focusedId() == engine::kUINoWidget, "widgets: pressing empty space clears focus");

    // --- (c) Scissor-clipped hit-testing -----------------------------------------
    const engine::UIRect clipPanelRect{0.0f, 0.0f, 200.0f, 100.0f};
    const engine::UIRect clippedRect{150.0f, 80.0f, 120.0f, 30.0f}; // extends past the panel
    rig.begin(240.0f, 95.0f, false);
    rig.ctx.beginPanel("ClipPanel", clipPanelRect);
    check(!rig.ctx.button(clippedRect, "Clipped") && rig.ctx.hoveredId() == engine::kUINoWidget,
          "widgets: scissored-away pixels do not hover");
    rig.ctx.endPanel();
    rig.end();
    rig.begin(240.0f, 95.0f, true);
    rig.ctx.beginPanel("ClipPanel", clipPanelRect);
    rig.ctx.button(clippedRect, "Clipped");
    check(rig.ctx.activeId() == engine::kUINoWidget, "widgets: clicks in the clipped region never activate");
    rig.ctx.endPanel();
    rig.end();
    rig.begin(240.0f, 95.0f, false);
    rig.ctx.beginPanel("ClipPanel", clipPanelRect);
    rig.ctx.button(clippedRect, "Clipped");
    rig.ctx.endPanel();
    rig.end();
    rig.begin(180.0f, 90.0f, true);
    rig.ctx.beginPanel("ClipPanel", clipPanelRect);
    rig.ctx.button(clippedRect, "Clipped");
    check(rig.ctx.activeId() != engine::kUINoWidget, "widgets: the visible part of a clipped widget still presses");
    rig.ctx.endPanel();
    rig.end();
    rig.begin(180.0f, 90.0f, false);
    rig.ctx.beginPanel("ClipPanel", clipPanelRect);
    rig.ctx.button(clippedRect, "Clipped");
    rig.ctx.endPanel();
    rig.end();

    // --- (d) Slider drag math + keyboard stepping ----------------------------------
    const engine::UIRect speedRect{100.0f, 100.0f, 300.0f, 26.0f};
    const engine::UIRect track = engine::UIContext::sliderTrackRect(speedRect);
    check(track.x == 222.0f && track.width == 174.0f, "widgets: slider track follows the published layout");
    float speed = 2.0f;
    rig.begin(track.x + 87.0f, 113.0f, true);
    check(rig.ctx.slider(speedRect, "Speed", &speed, 0.0f, 10.0f) && speed == 5.0f,
          "widgets: pressing the track jumps to the mouse fraction");
    rig.end();
    rig.begin(track.x + track.width, 113.0f, true);
    check(rig.ctx.slider(speedRect, "Speed", &speed, 0.0f, 10.0f) && speed == 10.0f,
          "widgets: dragging to the track end reaches max");
    rig.end();
    rig.begin(600.0f, 113.0f, true);
    check(!rig.ctx.slider(speedRect, "Speed", &speed, 0.0f, 10.0f) && speed == 10.0f,
          "widgets: dragging past the end clamps at max (no change reported)");
    rig.end();
    rig.begin(260.0f, 400.0f, true);
    check(rig.ctx.slider(speedRect, "Speed", &speed, 0.0f, 10.0f) && nearlyEqual(speed, 2.1839f, 0.01f),
          "widgets: an active slider follows the mouse even off its row");
    rig.end();
    rig.begin(260.0f, 400.0f, false);
    rig.ctx.slider(speedRect, "Speed", &speed, 0.0f, 10.0f);
    check(rig.ctx.activeId() == engine::kUINoWidget, "widgets: release ends the slider drag");
    rig.end();
    rig.begin(0.0f, 0.0f, false, +1);
    check(rig.ctx.slider(speedRect, "Speed", &speed, 0.0f, 10.0f) && nearlyEqual(speed, 2.2839f, 0.01f),
          "widgets: a focused slider steps 1% of range per key press");
    rig.end();

    // --- (e) Checkbox ---------------------------------------------------------------
    const engine::UIRect turboRect{100.0f, 200.0f, 200.0f, 26.0f};
    bool turbo = false;
    rig.begin(120.0f, 213.0f, true);
    rig.ctx.checkbox(turboRect, "Turbo", &turbo);
    rig.end();
    rig.begin(120.0f, 213.0f, false);
    check(rig.ctx.checkbox(turboRect, "Turbo", &turbo) && turbo, "widgets: checkbox toggles on a completed click");
    rig.end();
    rig.begin(120.0f, 213.0f, true);
    rig.ctx.checkbox(turboRect, "Turbo", &turbo);
    rig.end();
    rig.begin(500.0f, 400.0f, false);
    check(!rig.ctx.checkbox(turboRect, "Turbo", &turbo) && turbo,
          "widgets: releasing outside leaves the checkbox unchanged");
    rig.end();

    // --- (f) Panel layout rows ---------------------------------------------------------
    rig.begin(30.0f, 25.0f, false);
    rig.ctx.beginPanel("LayoutPanel", engine::UIRect{10.0f, 10.0f, 300.0f, 200.0f});
    rig.ctx.button("First");
    rig.ctx.button("Second");
    const engine::UIWidgetId firstId = rig.ctx.hoveredId();
    rig.ctx.endPanel();
    rig.end();
    rig.begin(30.0f, 55.0f, false);
    rig.ctx.beginPanel("LayoutPanel", engine::UIRect{10.0f, 10.0f, 300.0f, 200.0f});
    rig.ctx.button("First");
    rig.ctx.button("Second");
    check(firstId != engine::kUINoWidget && rig.ctx.hoveredId() != firstId &&
              rig.ctx.hoveredId() != engine::kUINoWidget,
          "widgets: layout rows stack and hover independently");
    rig.ctx.endPanel();
    rig.end();

    // --- (g) Reflection-driven Component Inspector ---------------------------------------
    EditorWidgetSample sample;
    sample.count = 7;
    const engine::UIRect panel{0.0f, 0.0f, 360.0f, 260.0f};
    const engine::UIInspectorOptions options{0.0f, 10.0f, -5.0f, 5.0f};
    const auto rowRect = [&panel](int row) {
        return engine::UIRect{panel.x + engine::UIContext::kPanelPadding,
                              panel.y + engine::UIContext::kPanelPadding +
                                  static_cast<float>(row) *
                                      (engine::UIContext::kRowHeight + engine::UIContext::kRowSpacing),
                              panel.width - 2.0f * engine::UIContext::kPanelPadding,
                              engine::UIContext::kRowHeight};
    };
    const auto inspectorFrame = [&](float mouseX, float mouseY, bool mouseDown) {
        rig.batch.begin(800, 600);
        rig.ctx.beginFrame(engine::UIInput{mouseX, mouseY, mouseDown, 0}, rig.batch);
        const bool changed = engine::inspectComponent(rig.ctx, panel, engine::reflect<EditorWidgetSample>(),
                                                      &sample, options);
        rig.ctx.endFrame();
        return changed;
    };
    // Rows: 0 title, 1 count (label), 2 flags (label), 3 weight (slider),
    // 4 active (checkbox), 5..7 position components (sliders).
    check(!inspectorFrame(700.0f, 500.0f, false) && sample.weight == 0.0f && !rig.batch.empty(),
          "inspector: builds all rows without changing values on an idle frame");
    const engine::UIRect weightTrack = engine::UIContext::sliderTrackRect(rowRect(3));
    check(inspectorFrame(weightTrack.x + weightTrack.width, rowRect(3).y + 13.0f, true) &&
              sample.weight == 10.0f,
          "inspector: dragging the float slider writes through the reflected offset");
    inspectorFrame(weightTrack.x + weightTrack.width, rowRect(3).y + 13.0f, false);
    inspectorFrame(30.0f, rowRect(4).y + 13.0f, true);
    check(inspectorFrame(30.0f, rowRect(4).y + 13.0f, false) && sample.active,
          "inspector: clicking the bool checkbox toggles the field");
    const engine::UIRect posTrack = engine::UIContext::sliderTrackRect(rowRect(5));
    check(inspectorFrame(posTrack.x + 0.75f * posTrack.width, rowRect(5).y + 13.0f, true) &&
              nearlyEqual(sample.position.x, 2.5f, 0.001f) && sample.position.y == 0.0f,
          "inspector: vec3 component sliders edit one axis at a time");
    inspectorFrame(posTrack.x + 0.75f * posTrack.width, rowRect(5).y + 13.0f, false);
    check(sample.count == 7, "inspector: read-only int rows never write back");
}

void testPhase27Editor() {
    std::cout << "Phase 27: editor shell, outliner & context menus\n";
    struct EditorRig {
        engine::SdfFontAtlas atlas = engine::generateSdfFontAtlas();
        engine::UIBatch batch;
        engine::UIContext ctx;
        EditorRig() { batch.setFontAtlas(&atlas); }
        void frame(float mouseX, float mouseY, bool down, bool right = false, bool esc = false, float scroll = 0.0f) {
            batch.begin(800, 600);
            engine::UIInput input;
            input.mouseX = mouseX;
            input.mouseY = mouseY;
            input.mouseDown = down;
            input.mouseRightDown = right;
            input.escapePressed = esc;
            input.scrollY = scroll;
            ctx.beginFrame(input, batch);
        }
        void end() { ctx.endFrame(); }
    };

    // --- (a) Right-click dispatch is edge-only and button-independent ------------
    {
        EditorRig rig;
        const engine::UIRect viewport{0.0f, 0.0f, 800.0f, 600.0f};
        rig.frame(400.0f, 300.0f, false, true);
        check(rig.ctx.rightPressedRect(viewport) && !rig.ctx.pressedRect(viewport),
              "context: right press dispatches to rightPressedRect only");
        rig.end();
        rig.frame(400.0f, 300.0f, false, true);
        check(!rig.ctx.rightPressedRect(viewport), "context: holding the right button does not re-fire");
        rig.end();
        rig.frame(400.0f, 300.0f, false, false);
        check(!rig.ctx.rightPressedRect(viewport) && !rig.ctx.popupOpen(),
              "context: release leaves no stray popup state");
        rig.end();
    }

    // --- (b) Menu item fires on the press edge and closes the popup --------------
    {
        EditorRig rig;
        rig.frame(200.0f, 150.0f, false, true);
        if (rig.ctx.rightPressedRect(engine::UIRect{0.0f, 0.0f, 800.0f, 600.0f})) {
            rig.ctx.openPopup("File", 200.0f, 150.0f);
        }
        rig.end();
        check(rig.ctx.isPopupOpen("File"), "context: openPopup raises the named popup");
        rig.frame(200.0f, 150.0f, false);
        bool rendered = false;
        if (rig.ctx.beginPopup("File")) {
            rendered = true;
            rig.ctx.menuItem("Open");
            rig.ctx.menuItem("Close");
            rig.ctx.endPopup();
        }
        check(rendered, "context: beginPopup renders the frame after opening");
        rig.end();
        rig.frame(215.0f, 164.0f, true); // first item: {204,154,176,20}
        bool fired = false;
        bool other = true;
        if (rig.ctx.beginPopup("File")) {
            fired = rig.ctx.menuItem("Open");
            other = rig.ctx.menuItem("Close");
            rig.ctx.endPopup();
        }
        check(fired && !other && !rig.ctx.popupOpen(),
              "context: pressing an item fires it and dismisses the menu");
        rig.end();
    }

    // --- (c) Esc dismisses; dismissal is safe with nothing open -------------------
    {
        EditorRig rig;
        rig.frame(10.0f, 10.0f, false, false, true);
        check(!rig.ctx.popupOpen(), "context: Esc with no popup is a harmless no-op");
        rig.end();
        rig.ctx.openPopup("Edit", 100.0f, 100.0f);
        rig.frame(10.0f, 10.0f, false);
        check(rig.ctx.beginPopup("Edit"), "context: popup renders while open");
        rig.ctx.endPopup();
        rig.end();
        rig.frame(10.0f, 10.0f, false, false, true);
        check(!rig.ctx.popupOpen() && !rig.ctx.beginPopup("Edit"),
              "context: Esc dismisses the popup before widgets run");
        rig.end();
    }

    // --- (d) Opening a popup replaces the previous one (single-menu stack) --------
    {
        EditorRig rig;
        rig.frame(10.0f, 10.0f, false);
        rig.ctx.openPopup("First", 10.0f, 10.0f);
        rig.ctx.openPopup("Second", 50.0f, 50.0f);
        check(!rig.ctx.isPopupOpen("First") && rig.ctx.isPopupOpen("Second"),
              "context: opening a second menu replaces the first");
        rig.end();
        rig.frame(10.0f, 10.0f, false);
        check(!rig.ctx.beginPopup("First") && rig.ctx.beginPopup("Second"),
              "context: only the replacement menu renders");
        rig.ctx.endPopup();
        rig.end();
        rig.ctx.closePopup();
        rig.ctx.closePopup();
        check(!rig.ctx.popupOpen(), "context: closePopup is idempotent");
    }

    // --- (e) Outside click: dismisses AND the press is consumed --------------------
    {
        EditorRig rig;
        const engine::UIRect guarded{100.0f, 100.0f, 140.0f, 32.0f};
        rig.ctx.openPopup("Ctx", 400.0f, 300.0f);
        for (int i = 0; i < 2; ++i) {
            rig.frame(700.0f, 500.0f, false);
            if (rig.ctx.beginPopup("Ctx")) {
                rig.ctx.menuItem("One");
                rig.ctx.menuItem("Two");
                rig.ctx.endPopup();
            }
            rig.end();
        }
        rig.frame(120.0f, 116.0f, true);
        const bool clicked = rig.ctx.button(guarded, "Guarded");
        check(!clicked && rig.ctx.activeId() == engine::kUINoWidget &&
                  rig.ctx.focusedId() == engine::kUINoWidget && !rig.ctx.popupOpen(),
              "context: clicking outside closes the menu and is consumed by it");
        rig.end();
        rig.frame(120.0f, 116.0f, false);
        rig.ctx.button(guarded, "Guarded");
        rig.end();
        rig.frame(120.0f, 116.0f, true);
        check(!rig.ctx.button(guarded, "Guarded") && rig.ctx.activeId() != engine::kUINoWidget,
              "context: the next honest press reaches the widget again");
        rig.end();
        rig.frame(120.0f, 116.0f, false);
        check(rig.ctx.button(guarded, "Guarded"), "context: ...and its release completes the click");
        rig.end();
        // A press inside the popup's own frame does not dismiss it.
        rig.ctx.openPopup("Ctx", 400.0f, 300.0f);
        rig.frame(700.0f, 500.0f, false);
        if (rig.ctx.beginPopup("Ctx")) {
            rig.ctx.menuItem("One");
            rig.ctx.menuItem("Two");
            rig.ctx.endPopup();
        }
        rig.end();
        rig.frame(500.0f, 343.0f, true); // inside the menu frame, below the items
        check(rig.ctx.popupOpen(), "context: pressing inside the menu frame keeps it open");
        rig.end();
    }

    // --- (f) Disabled menu items never fire and keep the menu open -----------------
    {
        EditorRig rig;
        rig.ctx.openPopup("Act", 100.0f, 100.0f);
        rig.frame(0.0f, 0.0f, false);
        if (rig.ctx.beginPopup("Act")) {
            rig.ctx.menuItem("Locked", false);
            rig.ctx.menuItem("Ready");
            rig.ctx.endPopup();
        }
        rig.end();
        rig.frame(120.0f, 114.0f, true); // first item row, but disabled
        bool firedDisabled = true;
        if (rig.ctx.beginPopup("Act")) {
            firedDisabled = rig.ctx.menuItem("Locked", false);
            rig.ctx.menuItem("Ready");
            rig.ctx.endPopup();
        }
        check(!firedDisabled && rig.ctx.popupOpen(), "context: a disabled item swallows the press silently");
        rig.end();
        rig.frame(120.0f, 114.0f, false);
        if (rig.ctx.beginPopup("Act")) {
            rig.ctx.menuItem("Locked", false);
            rig.ctx.menuItem("Ready");
            rig.ctx.endPopup();
        }
        rig.end();
        rig.frame(120.0f, 134.0f, true); // the enabled sibling
        bool firedReady = false;
        if (rig.ctx.beginPopup("Act")) {
            rig.ctx.menuItem("Locked", false);
            firedReady = rig.ctx.menuItem("Ready");
            rig.ctx.endPopup();
        }
        check(firedReady && !rig.ctx.popupOpen(), "context: the enabled item fires on the next press");
        rig.end();
    }

    // --- (g) Outliner: selection, per-entity context actions, scrolling --------------
    {
        EditorRig rig;
        engine::Scene scene;
        engine::UIOutliner outliner;
        auto& registry = scene.registry();
        const engine::UIRect panel{0.0f, 0.0f, 320.0f, 300.0f};
        // Rows: top = panel.y + 38, height 22. Roots sort by name:
        // Alpha (row 0), Beta (row 1). Creation order deliberately differs.
        auto beta = scene.createEntity("Beta");
        beta.addComponent<engine::TransformComponent>(glm::vec3{1.0f, 2.0f, 3.0f});
        beta.addComponent<engine::RigidBodyComponent>(2.0f);
        beta.addComponent<engine::NativeScriptComponent>(std::string("Orbit"));
        auto alpha = scene.createEntity("Alpha");
        const entt::entity alphaHandle = alpha.handle();
        const entt::entity betaHandle = beta.handle();

        auto build = [&](float x, float y, bool down, bool right = false, bool esc = false, float scroll = 0.0f) {
            rig.frame(x, y, down, right, esc, scroll);
            const engine::UIOutliner::Result result = outliner.build(rig.ctx, panel, scene);
            rig.end();
            return result;
        };

        check(build(700.0f, 500.0f, false).action == engine::UIOutliner::Action::None &&
                  outliner.selected() == entt::null,
              "outliner: an idle frame selects nothing");
        check(build(30.0f, 49.0f, true).selectionChanged && outliner.selected() == alphaHandle,
              "outliner: clicking row 0 selects Alpha (roots sorted by name)");
        check(build(30.0f, 49.0f, false).action == engine::UIOutliner::Action::None,
              "outliner: release is inert");
        build(30.0f, 71.0f, true);
        check(outliner.selected() == betaHandle, "outliner: clicking row 1 moves the selection to Beta");

        // Right-click Beta -> menu; Duplicate from the menu.
        check(build(60.0f, 71.0f, false, true).action == engine::UIOutliner::Action::None &&
                  rig.ctx.isPopupOpen("EntityContextMenu") && outliner.selected() == betaHandle,
              "outliner: right-click selects and opens the entity menu");
        build(60.0f, 71.0f, false);
        const auto duplicateResult = build(70.0f, 85.0f, true); // "Duplicate" item
        const entt::entity copy = duplicateResult.entity;
        check(duplicateResult.action == engine::UIOutliner::Action::Duplicated && copy != entt::null &&
                  copy != betaHandle && registry.valid(copy),
              "outliner: Duplicate creates a fresh entity");
        check(registry.valid(copy) && registry.get<engine::TagComponent>(copy).tag == "Beta copy" &&
                  nearlyVec3(registry.get<engine::TransformComponent>(copy).position,
                             glm::vec3{1.0f, 2.0f, 3.0f}) &&
                  nearlyEqual(registry.get<engine::RigidBodyComponent>(copy).inverseMass, 0.5) &&
                  registry.get<engine::NativeScriptComponent>(copy).scriptName == "Orbit" &&
                  !registry.all_of<engine::ParentComponent>(copy) && outliner.selected() == copy &&
                  registry.view<engine::TagComponent>().size() == 3u,
              "outliner: the copy inherits components and becomes the selection");

        // Right-click the copy (now row 2) -> Create Child.
        build(60.0f, 93.0f, false, true);
        build(60.0f, 93.0f, false);
        const auto childResult = build(70.0f, 127.0f, true); // "Create Child" item
        const entt::entity child = childResult.entity;
        bool childLinked = false;
        if (registry.valid(child) && registry.valid(copy)) {
            const auto* parent = registry.try_get<engine::ParentComponent>(child);
            const auto* kids = registry.try_get<engine::ChildrenComponent>(copy);
            bool listedAsChild = false;
            if (kids != nullptr) {
                for (const entt::entity candidate : kids->children) {
                    listedAsChild = listedAsChild || candidate == child;
                }
            }
            childLinked = parent != nullptr && parent->parent == copy && listedAsChild;
        }
        check(childResult.action == engine::UIOutliner::Action::CreatedChild && childLinked &&
                  registry.get<engine::TagComponent>(child).tag == "Entity" && outliner.selected() == child,
              "outliner: Create Child parents a new entity under the target");

        // Retarget the open menu from Alpha to the copy, then Delete the copy.
        build(60.0f, 49.0f, false, true);
        check(outliner.selected() == alphaHandle && rig.ctx.isPopupOpen("EntityContextMenu"),
              "outliner: a second right-click retargets selection and menu");
        build(60.0f, 93.0f, false);       // release the right button first...
        build(60.0f, 93.0f, false, true); // ...then right-click the copy row (row 2) instead
        check(outliner.selected() == copy, "outliner: right-clicking another row switches the target");
        build(60.0f, 93.0f, false);
        const auto deleteResult = build(70.0f, 156.0f, true); // "Delete" item
        // Default HierarchyDestroyMode::Orphan: the child survives as
        // a root; only the target itself is destroyed.
        check(deleteResult.action == engine::UIOutliner::Action::Deleted && !registry.valid(copy) &&
                  registry.valid(child) && !registry.all_of<engine::ParentComponent>(child) &&
                  outliner.selected() == entt::null && registry.view<engine::TagComponent>().size() == 3u,
              "outliner: Delete destroys the target and clears the selection");

        // Esc closes the menu without touching the scene.
        build(60.0f, 71.0f, false, true);
        check(rig.ctx.isPopupOpen("EntityContextMenu"), "outliner: menu reopened for the Esc test");
        build(700.0f, 500.0f, false, false, true);
        check(!rig.ctx.popupOpen() && registry.valid(betaHandle) &&
                  registry.view<engine::TagComponent>().size() == 3u,
              "outliner: Esc dismisses the menu and changes nothing");
        // A consumed outside click likewise leaves scene and selection alone.
        build(60.0f, 71.0f, false, true);
        build(700.0f, 500.0f, true);
        check(!rig.ctx.popupOpen() && outliner.selected() == betaHandle,
              "outliner: outside click closes the menu without reselecting");

        // Wheel scrolling over the panel.
        for (int i = 0; i < 15; ++i) {
            scene.createEntity("Filler");
        }
        build(100.0f, 150.0f, false, false, false, -5.0f);
        const float scrolledDown = outliner.scrollOffset();
        build(100.0f, 150.0f, false, false, false, 50.0f);
        check(scrolledDown > 0.0f && outliner.scrollOffset() == 0.0f,
              "outliner: the wheel scrolls the list and clamps at the top");
    }

    // --- (h) Duplicating a child keeps it under the same parent ---------------------
    {
        EditorRig rig;
        engine::Scene scene;
        engine::UIOutliner outliner;
        auto& registry = scene.registry();
        const engine::UIRect panel{0.0f, 0.0f, 320.0f, 300.0f};
        scene.setHierarchyDestroyMode(engine::HierarchyDestroyMode::Cascade);
        auto parent = scene.createEntity("Parent");
        auto kid = scene.createEntity("Kid");
        scene.setParent(kid, parent);

        auto build = [&](float x, float y, bool down, bool right = false) {
            rig.frame(x, y, down, right);
            const engine::UIOutliner::Result result = outliner.build(rig.ctx, panel, scene);
            rig.end();
            return result;
        };
        build(60.0f, 71.0f, false, true); // right-click the Kid row (row 1, depth 1)
        check(outliner.selected() == kid.handle(), "outliner: nested rows are selectable targets");
        build(60.0f, 71.0f, false);
        const auto duplicateResult = build(70.0f, 85.0f, true);
        const entt::entity copy = duplicateResult.entity;
        const auto* copiedParent =
            registry.valid(copy) ? registry.try_get<engine::ParentComponent>(copy) : nullptr;
        check(duplicateResult.action == engine::UIOutliner::Action::Duplicated && copiedParent != nullptr &&
                  copiedParent->parent == parent.handle() &&
                  registry.get<engine::TagComponent>(copy).tag == "Kid copy",
              "outliner: a duplicated child is adopted by the source's parent");
        // Deleting the parent cascades through both children.
        build(60.0f, 49.0f, false, true); // right-click Parent (row 0)
        build(60.0f, 49.0f, false);
        const auto deleteResult = build(70.0f, 112.0f, true);
        check(deleteResult.action == engine::UIOutliner::Action::Deleted &&
                  registry.view<engine::TagComponent>().size() == 0u && outliner.selected() == entt::null,
              "outliner: deleting a parent removes the whole subtree in Cascade mode");
    }
}

void testPhase28Gizmos() {
    std::cout << "Phase 28: raycast picking & transform gizmos\n";
    struct GizmoRig {
        engine::SdfFontAtlas atlas = engine::generateSdfFontAtlas();
        engine::UIBatch batch;
        engine::UIContext ctx;
        GizmoRig() { batch.setFontAtlas(&atlas); }
        void frame(float mouseX, float mouseY, bool down) {
            batch.begin(800, 600);
            engine::UIInput input;
            input.mouseX = mouseX;
            input.mouseY = mouseY;
            input.mouseDown = down;
            ctx.beginFrame(input, batch);
        }
        void end() { ctx.endFrame(); }
    };
    const glm::vec2 viewport{800.0f, 600.0f};

    // Axis-aligned test camera: eye (0,0,5) looking down -Z.
    engine::CameraConfig camConfig;
    camConfig.startPosition = glm::vec3{0.0f, 0.0f, 5.0f};
    const engine::Camera camera{camConfig};

    // --- (a) Screen -> NDC -> world ray mapping --------------------------------
    check(engine::screenToNdc(glm::vec2{400.0f, 300.0f}, viewport) == glm::vec2{0.0f, 0.0f} &&
              engine::screenToNdc(glm::vec2{0.0f, 0.0f}, viewport) == glm::vec2{-1.0f, -1.0f},
          "gizmo: viewport centre/corner map to NDC origin/top-left");
    const engine::Ray centreRay = engine::cameraRay(camera, glm::vec2{0.0f, 0.0f}, 800.0f / 600.0f);
    check(nearlyVec3(centreRay.origin, glm::vec3{0.0f, 0.0f, 5.0f}) &&
              nearlyVec3(centreRay.direction, glm::vec3{0.0f, 0.0f, -1.0f}),
          "gizmo: centre ray leaves the eye along -Z");
    const engine::Ray cornerRay = engine::cameraRay(camera, engine::screenToNdc(glm::vec2{799.0f, 0.0f}, viewport),
                                                    800.0f / 600.0f);
    check(cornerRay.direction.x > 0.4f && cornerRay.direction.y > 0.3f && cornerRay.direction.z < -0.7f,
          "gizmo: top-right pixel ray leans right/up/forward (Vulkan y-down NDC)");

    // --- (b) Ray vs AABB slab test ------------------------------------------------
    {
        float distance = -1.0f;
        glm::vec3 normal{0.0f};
        const engine::AABB box{glm::vec3{-0.5f}, glm::vec3{0.5f}};
        check(engine::rayIntersectsAABB(centreRay, box, distance, normal) && nearlyEqual(distance, 4.5f, 1e-4) &&
                  nearlyVec3(normal, glm::vec3{0.0f, 0.0f, 1.0f}),
              "gizmo: ray enters the box front face at 4.5 with a +Z normal");
        const engine::Ray missRay{glm::vec3{0.0f, 3.0f, 5.0f}, glm::vec3{0.0f, 0.0f, -1.0f}};
        check(!engine::rayIntersectsAABB(missRay, box, distance, normal), "gizmo: an offset ray misses cleanly");
        const engine::Ray awayRay{glm::vec3{0.0f, 0.0f, 5.0f}, glm::vec3{0.0f, 0.0f, 1.0f}};
        check(!engine::rayIntersectsAABB(awayRay, box, distance, normal), "gizmo: a box behind the ray is no hit");
        const engine::Ray insideRay{glm::vec3{0.0f}, glm::vec3{0.0f, 0.0f, -1.0f}};
        check(engine::rayIntersectsAABB(insideRay, box, distance, normal) && distance == 0.0f &&
                  nearlyVec3(normal, glm::vec3{0.0f, 0.0f, 1.0f}),
              "gizmo: an inside origin hits at 0 facing the ray");
    }

    // --- (c) World -> screen projection (screenToNdc inverse) -----------------------
    {
        const glm::mat4 viewProjection = camera.projectionMatrix(800.0f / 600.0f) * camera.viewMatrix();
        glm::vec2 pixels{0.0f};
        check(engine::TransformGizmo::projectToScreen(glm::vec3{0.0f}, viewProjection, viewport, pixels) &&
                  nearlyEqual(pixels.x, 400.0f, 0.5f) && nearlyEqual(pixels.y, 300.0f, 0.5f),
              "gizmo: the world origin projects to the viewport centre");
        check(engine::TransformGizmo::projectToScreen(glm::vec3{1.4f, 0.0f, 0.0f}, viewProjection, viewport, pixels) &&
                  nearlyEqual(pixels.x, 602.8f, 3.0f) && nearlyEqual(pixels.y, 300.0f, 0.5f),
              "gizmo: +X projects right at the analytic pixel");
        check(engine::TransformGizmo::projectToScreen(glm::vec3{0.0f, 1.4f, 0.0f}, viewProjection, viewport, pixels) &&
                  nearlyEqual(pixels.x, 400.0f, 0.5f) && nearlyEqual(pixels.y, 97.2f, 3.0f),
              "gizmo: +Y projects up-screen (engine y-down pixels)");
        check(!engine::TransformGizmo::projectToScreen(glm::vec3{0.0f, 0.0f, 10.0f}, viewProjection, viewport, pixels),
              "gizmo: points behind the camera do not project");
        // Round-trip: the picking ray through the projected pixel must
        // pass through the original world point.
        const glm::vec3 world{0.3f, -0.2f, -1.0f};
        (void)engine::TransformGizmo::projectToScreen(world, viewProjection, viewport, pixels);
        const engine::Ray roundTrip = engine::cameraRay(camera, engine::screenToNdc(pixels, viewport), 800.0f / 600.0f);
        const float offRay = glm::length(glm::cross(world - roundTrip.origin, roundTrip.direction));
        check(offRay < 1e-3f, "gizmo: project -> pick ray round-trips through the same point");
    }

    // --- (d) Axis-line parameter (closest point between ray and line) ---------------
    {
        using engine::TransformGizmo;
        const engine::Ray down{glm::vec3{2.0f, 1.0f, 5.0f}, glm::vec3{0.0f, 0.0f, -1.0f}};
        check(nearlyEqual(TransformGizmo::rayAxisParameter(down, glm::vec3{0.0f}, glm::vec3{1.0f, 0.0f, 0.0f}), 2.0f,
                          1e-4f),
              "gizmo: closest point on the X axis sits below the offset ray");
        const engine::Ray parallel{glm::vec3{0.0f, 3.0f, 5.0f}, glm::vec3{1.0f, 0.0f, 0.0f}};
        check(nearlyEqual(TransformGizmo::rayAxisParameter(parallel, glm::vec3{0.0f}, glm::vec3{1.0f, 0.0f, 0.0f}),
                          0.0f, 1e-4f),
              "gizmo: a parallel ray falls back to the projected origin parameter");
        check(TransformGizmo::handleLength(5.0f) == 1.4f && TransformGizmo::handleLength(0.0f) == 0.4f &&
                  TransformGizmo::handleLength(1000.0f) == 20.0f,
              "gizmo: handle length is distance-proportional and clamped");
    }

    // --- (e) Viewport picking ---------------------------------------------------------
    {
        engine::Scene scene;
        auto far = scene.createEntity("Far");
        far.addComponent<engine::TransformComponent>(glm::vec3{1.2f, 0.0f, -4.0f});
        far.addComponent<engine::ColliderComponent>(engine::ColliderComponent::box(glm::vec3{0.5f}));
        auto nearCube = scene.createEntity("Near");
        nearCube.addComponent<engine::TransformComponent>(glm::vec3{0.0f});
        nearCube.addComponent<engine::ColliderComponent>(engine::ColliderComponent::box(glm::vec3{0.5f}));
        auto ghost = scene.createEntity("Ghost"); // transform only: nothing to hit
        ghost.addComponent<engine::TransformComponent>(glm::vec3{0.0f, 0.0f, -2.0f});

        engine::RaycastHit hit;
        check(engine::pickEntityAt(scene, camera, glm::vec2{400.0f, 300.0f}, viewport, hit) &&
                  hit.entity == nearCube.handle() && nearlyEqual(hit.distance, 4.5f, 1e-3f) &&
                  nearlyVec3(hit.normal, glm::vec3{0.0f, 0.0f, 1.0f}),
              "picking: the centre pixel selects the nearest collider entity");
        check(!engine::pickEntityAt(scene, camera, glm::vec2{60.0f, 60.0f}, viewport, hit),
              "picking: an empty pixel selects nothing");
        check(!engine::pickEntityAt(scene, camera, glm::vec2{400.0f, 300.0f}, glm::vec2{0.0f, 600.0f}, hit),
              "picking: a degenerate viewport is rejected");
        // Aiming right of Near reaches the laterally offset Far:
        // pixel x 509 crosses the z=0 plane at x = 0.75 (outside
        // Near's [-0.5, 0.5]) and Far's face at x = 1.275.
        check(engine::pickEntityAt(scene, camera, glm::vec2{509.0f, 300.0f}, viewport, hit) &&
                  hit.entity == far.handle(),
              "picking: a ray past the front cube reaches the one behind");
    }

    // --- (f) Translate drag state machine ---------------------------------------------
    {
        GizmoRig rig;
        engine::Scene scene;
        engine::TransformGizmo gizmo;
        auto cube = scene.createEntity("Cube");
        cube.addComponent<engine::TransformComponent>(glm::vec3{0.0f});
        cube.addComponent<engine::ColliderComponent>(engine::ColliderComponent::box(glm::vec3{0.5f}));
        const entt::entity cubeHandle = cube.handle();
        auto& registry = scene.registry();
        auto update = [&](float x, float y, bool down, bool blocked = false) {
            rig.frame(x, y, down);
            const engine::GizmoUpdateResult result =
                gizmo.update(rig.ctx, scene, cubeHandle, camera, viewport, blocked);
            rig.end();
            return result;
        };

        // Idle hover over empty space: nothing lights up.
        engine::GizmoUpdateResult result = update(100.0f, 100.0f, false);
        check(result.hoveredAxis == engine::GizmoAxis::None && !result.dragging && !result.pressConsumed,
              "gizmo: empty viewport space hovers no axis");
        // Hover the X handle midpoint (projected (0.7,0,0) ~= (501,300)).
        result = update(501.0f, 300.0f, false);
        check(result.hoveredAxis == engine::GizmoAxis::X && gizmo.hoveredAxis() == engine::GizmoAxis::X,
              "gizmo: the X arrow lights up under the cursor");
        // Press grabs the axis, the press counts as consumed (no re-pick).
        result = update(501.0f, 300.0f, true);
        check(result.pressConsumed && result.dragging && gizmo.activeAxis() == engine::GizmoAxis::X &&
                  gizmo.dragTarget() == cubeHandle,
              "gizmo: pressing a handle starts an axis-locked drag");
        // Drag +100 px: world x follows the analytic delta, y/z frozen.
        result = update(601.0f, 300.0f, true);
        const glm::vec3 afterDrag = registry.get<engine::TransformComponent>(cubeHandle).position;
        check(result.changed && nearlyEqual(afterDrag.x, 0.69f, 0.04f) && afterDrag.y == 0.0f && afterDrag.z == 0.0f,
              "gizmo: translate drag moves exactly along X (axis constraint)");
        // The reflection layer sees the very same write immediately.
        const engine::TypeInfo& transformType = engine::reflect<engine::TransformComponent>();
        const engine::PropertyInfo* positionProperty = nullptr;
        for (const engine::PropertyInfo& property : transformType.properties) {
            if (property.name == "position") {
                positionProperty = &property;
            }
        }
        const glm::vec3 reflected =
            positionProperty != nullptr
                ? engine::readProperty<glm::vec3>(&registry.get<engine::TransformComponent>(cubeHandle),
                                                  *positionProperty)
                : glm::vec3{-999.0f};
        check(positionProperty != nullptr && nearlyVec3(reflected, afterDrag),
              "gizmo: reflection reads the dragged position back unchanged");
        // Dragging past the screen edge keeps following; release ends it.
        result = update(790.0f, 300.0f, true);
        check(result.dragging && registry.get<engine::TransformComponent>(cubeHandle).position.x > 1.8f,
              "gizmo: the drag keeps tracking near the viewport edge");
        result = update(790.0f, 300.0f, false);
        check(!result.dragging && !gizmo.dragging(), "gizmo: releasing ends the drag");
        // A fresh press in open space starts nothing.
        result = update(150.0f, 520.0f, true);
        check(!result.pressConsumed && !gizmo.dragging(), "gizmo: pressing open space never grabs an axis");
        // Blocked input (popup/capture) cannot start a drag.
        update(501.0f, 300.0f, false);
        result = update(501.0f, 300.0f, true, true);
        check(!result.pressConsumed && !gizmo.dragging(), "gizmo: blocked input starts no drag");
        // Switching tools mid-drag cancels without snapping back.
        update(501.0f, 300.0f, false); // release, so the next press has an edge
        update(501.0f, 300.0f, true);
        const float xBeforeSwitch = registry.get<engine::TransformComponent>(cubeHandle).position.x;
        gizmo.setMode(engine::GizmoMode::Rotate);
        result = update(601.0f, 300.0f, true);
        check(!result.dragging && gizmo.mode() == engine::GizmoMode::Rotate &&
                  registry.get<engine::TransformComponent>(cubeHandle).position.x == xBeforeSwitch,
              "gizmo: switching tools cancels the drag cleanly");
    }

    // --- (g) Rotate drag about Y (diagonal camera keeps the ring plane usable) --------
    {
        GizmoRig rig;
        engine::Scene scene;
        engine::TransformGizmo gizmo;
        gizmo.setMode(engine::GizmoMode::Rotate);
        engine::CameraConfig tiltedConfig;
        tiltedConfig.startPosition = glm::vec3{0.0f, 2.5f, 5.0f};
        tiltedConfig.startPitchDegrees = -26.565f; // look straight at the origin
        const engine::Camera tilted{tiltedConfig};
        auto dial = scene.createEntity("Dial");
        dial.addComponent<engine::TransformComponent>(glm::vec3{0.0f});
        const entt::entity dialHandle = dial.handle();
        auto& registry = scene.registry();
        auto update = [&](float x, float y, bool down) {
            rig.frame(x, y, down);
            const engine::GizmoUpdateResult result = gizmo.update(rig.ctx, scene, dialHandle, tilted, viewport);
            rig.end();
            return result;
        };
        const glm::mat4 viewProjection = tilted.projectionMatrix(800.0f / 600.0f) * tilted.viewMatrix();
        const float len = engine::TransformGizmo::handleLength(glm::distance(tilted.position(), glm::vec3{0.0f}));
        // Ring points at +/-45 degrees in the XZ plane (chosen off
        // the axis crossings, where two rings would share a point).
        const float s = len * 0.70710678f;
        glm::vec2 pointA{0.0f};
        glm::vec2 pointB{0.0f};
        (void)engine::TransformGizmo::projectToScreen(glm::vec3{s, 0.0f, s}, viewProjection, viewport, pointA);
        (void)engine::TransformGizmo::projectToScreen(glm::vec3{-s, 0.0f, s}, viewProjection, viewport, pointB);
        engine::GizmoUpdateResult result = update(pointA.x, pointA.y, false);
        check(result.hoveredAxis == engine::GizmoAxis::Y, "gizmo: the Y ring lights up under the cursor");
        result = update(pointA.x, pointA.y, true);
        check(result.pressConsumed && gizmo.activeAxis() == engine::GizmoAxis::Y,
              "gizmo: pressing the ring starts a rotation drag");
        result = update(pointB.x, pointB.y, true);
        const engine::TransformComponent& dialTransform = registry.get<engine::TransformComponent>(dialHandle);
        check(result.changed && nearlyEqual(dialTransform.rotationDegrees.y, -90.0f, 2.0f) &&
                  dialTransform.rotationDegrees.x == 0.0f && dialTransform.rotationDegrees.z == 0.0f,
              "gizmo: rotating between ring points turns exactly -90 about Y");
        check(dialTransform.useQuaternionOrientation &&
                  nearlyEqual(glm::degrees(glm::eulerAngles(dialTransform.orientation)).y, -90.0f, 2.0f),
              "gizmo: the orientation quaternion is synced with the Euler edit");
        update(pointB.x, pointB.y, false);
    }

    // --- (h) Scale drag with clamping ---------------------------------------------------
    {
        GizmoRig rig;
        engine::Scene scene;
        engine::TransformGizmo gizmo;
        gizmo.setMode(engine::GizmoMode::Scale);
        auto block = scene.createEntity("Block");
        block.addComponent<engine::TransformComponent>(glm::vec3{0.0f});
        const entt::entity blockHandle = block.handle();
        auto& registry = scene.registry();
        auto update = [&](float x, float y, bool down) {
            rig.frame(x, y, down);
            const engine::GizmoUpdateResult result = gizmo.update(rig.ctx, scene, blockHandle, camera, viewport);
            rig.end();
            return result;
        };
        engine::GizmoUpdateResult result = update(501.0f, 300.0f, true);
        check(result.pressConsumed && gizmo.activeAxis() == engine::GizmoAxis::X,
              "gizmo: the scale handle grabs like an arrow");
        result = update(601.0f, 300.0f, true);
        glm::vec3 scale = registry.get<engine::TransformComponent>(blockHandle).scale;
        check(nearlyEqual(scale.x, 1.69f, 0.05f) && scale.y == 1.0f && scale.z == 1.0f,
              "gizmo: scale drag grows exactly one axis");
        result = update(20.0f, 300.0f, true);
        scale = registry.get<engine::TransformComponent>(blockHandle).scale;
        check(scale.x == engine::TransformGizmo::kMinScale && scale.y == 1.0f,
              "gizmo: dragging through zero clamps at the minimum scale (never inverts)");
        update(20.0f, 300.0f, false);
        // Deleting the target mid-drag cancels safely.
        update(501.0f, 300.0f, true);
        check(gizmo.dragging(), "gizmo: second drag running before the delete");
        scene.destroyEntity(block);
        result = update(601.0f, 300.0f, true);
        check(!result.dragging && !gizmo.dragging(), "gizmo: deleting the target mid-drag cancels safely");
    }

    // --- (i) Inert targets ----------------------------------------------------------------
    {
        GizmoRig rig;
        engine::Scene scene;
        engine::TransformGizmo gizmo;
        auto bare = scene.createEntity("Bare"); // tag only, no transform
        auto update = [&](entt::entity target, float x, float y, bool down) {
            rig.frame(x, y, down);
            const engine::GizmoUpdateResult result = gizmo.update(rig.ctx, scene, target, camera, viewport);
            rig.end();
            return result;
        };
        engine::GizmoUpdateResult result = update(entt::null, 400.0f, 300.0f, true);
        check(!result.dragging && result.hoveredAxis == engine::GizmoAxis::None,
              "gizmo: no selection means no gizmo");
        result = update(bare.handle(), 400.0f, 300.0f, true);
        check(!result.dragging && result.hoveredAxis == engine::GizmoAxis::None,
              "gizmo: an entity without a Transform gets no gizmo");
    }
}

void testPhase29Assets() {
    std::cout << "Phase 29: asset browser & drag-and-drop\n";
    struct AssetRig {
        engine::SdfFontAtlas atlas = engine::generateSdfFontAtlas();
        engine::UIBatch batch;
        engine::UIContext ctx;
        AssetRig() { batch.setFontAtlas(&atlas); }
        void frame(float mouseX, float mouseY, bool down, bool esc = false) {
            batch.begin(800, 600);
            engine::UIInput input;
            input.mouseX = mouseX;
            input.mouseY = mouseY;
            input.mouseDown = down;
            input.escapePressed = esc;
            ctx.beginFrame(input, batch);
        }
        void end() { ctx.endFrame(); }
    };

    // --- Fixture tree ------------------------------------------------------------
    const std::filesystem::path root = std::filesystem::temp_directory_path() / "engine_phase29_assets";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root / "models");
    std::filesystem::create_directories(root / "textures");
    const auto writeFile = [&root](const std::filesystem::path& relative, const std::string& content) {
        std::ofstream out(root / relative, std::ios::binary);
        out << content;
    };
    writeFile("models/cube.obj", "v 0 0 0\n");
    writeFile("models/rock.gltf", "{}\n");
    writeFile("textures/checker.ppm", "P6\n1 1\n255\n\xff\x00\x00");
    writeFile("app.cpp", "int main() {}\n");
    writeFile("notes.txt", "hello\n");
    writeFile(".hidden", "secret\n");

    using engine::AssetEntryKind;
    using engine::UIAssetBrowser;

    // --- (a) Asset type recognition ------------------------------------------------
    check(UIAssetBrowser::classifyPath("cube.OBJ", false) == AssetEntryKind::Mesh &&
              UIAssetBrowser::classifyPath("scene.gltf", false) == AssetEntryKind::Mesh &&
              UIAssetBrowser::classifyPath("hero.glb", false) == AssetEntryKind::Mesh &&
              UIAssetBrowser::classifyPath("skin.png", false) == AssetEntryKind::Texture &&
              UIAssetBrowser::classifyPath("photo.JPG", false) == AssetEntryKind::Texture &&
              UIAssetBrowser::classifyPath("checker.ppm", false) == AssetEntryKind::Texture &&
              UIAssetBrowser::classifyPath("ui.frag.spv", false) == AssetEntryKind::Shader &&
              UIAssetBrowser::classifyPath("sky.vert", false) == AssetEntryKind::Shader &&
              UIAssetBrowser::classifyPath("gameplay.cpp", false) == AssetEntryKind::Script &&
              UIAssetBrowser::classifyPath("plugin.lua", false) == AssetEntryKind::Script &&
              UIAssetBrowser::classifyPath("module.so", false) == AssetEntryKind::Module &&
              UIAssetBrowser::classifyPath("level.json", false) == AssetEntryKind::SceneFile &&
              UIAssetBrowser::classifyPath("data.bin", false) == AssetEntryKind::Unknown &&
              UIAssetBrowser::classifyPath("models.obj", true) == AssetEntryKind::Folder,
          "assets: extensions classify into kinds (case-insensitive, folders win)");

    // --- (b) Draggability and importability -------------------------------------------
    check(UIAssetBrowser::payloadTypeFor(AssetEntryKind::Mesh) == "AssetMesh" &&
              UIAssetBrowser::payloadTypeFor(AssetEntryKind::Texture) == "AssetTexture" &&
              UIAssetBrowser::payloadTypeFor(AssetEntryKind::Shader) == "AssetShader" &&
              UIAssetBrowser::payloadTypeFor(AssetEntryKind::Folder).empty() &&
              UIAssetBrowser::payloadTypeFor(AssetEntryKind::SceneFile).empty() &&
              UIAssetBrowser::payloadTypeFor(AssetEntryKind::Unknown).empty(),
          "assets: only files with drop semantics carry payload types");
    check(UIAssetBrowser::isImportableMesh("a/b/cube.obj") && !UIAssetBrowser::isImportableMesh("rock.gltf") &&
              !UIAssetBrowser::isImportableMesh("checker.ppm"),
          "assets: OBJ is the importable mesh format today");

    // --- (c) Directory scanning ----------------------------------------------------------
    {
        const std::vector<engine::AssetEntry> entries = UIAssetBrowser::scanDirectory(root, "");
        check(entries.size() == 4u && entries[0].name == "models" && entries[0].isDirectory &&
                  entries[1].name == "textures" && entries[2].name == "app.cpp" &&
                  entries[2].kind == AssetEntryKind::Script && entries[3].name == "notes.txt" &&
                  entries[3].kind == AssetEntryKind::Unknown,
              "assets: scan sorts folders first and skips dotfiles");
        const std::vector<engine::AssetEntry> models = UIAssetBrowser::scanDirectory(root, "models");
        check(models.size() == 2u && models[0].relativePath == "models/cube.obj" &&
                  models[0].kind == AssetEntryKind::Mesh && models[1].relativePath == "models/rock.gltf",
              "assets: scan reports root-relative paths and kinds");
        check(UIAssetBrowser::scanDirectory(root / "does-not-exist", "").empty(),
              "assets: an unreadable folder scans as empty");
    }

    // --- (d) Root containment for path resolution ------------------------------------------
    {
        std::filesystem::path absolute;
        check(UIAssetBrowser::resolveWithinRoot(root, "models/cube.obj", absolute) &&
                  absolute == std::filesystem::weakly_canonical(root / "models" / "cube.obj"),
              "assets: a nested path resolves under the root");
        check(UIAssetBrowser::resolveWithinRoot(root, "models/../textures/checker.ppm", absolute) &&
                  absolute == std::filesystem::weakly_canonical(root / "textures" / "checker.ppm"),
              "assets: normalized dot-dot that stays inside resolves");
        check(!UIAssetBrowser::resolveWithinRoot(root, "../outside.obj", absolute) &&
                  !UIAssetBrowser::resolveWithinRoot(root, "/etc/passwd", absolute) &&
                  !UIAssetBrowser::resolveWithinRoot(root, "models/../../outside.obj", absolute),
              "assets: escapes and absolute paths are refused");
    }

    // --- (e) Ground drop point ----------------------------------------------------------------
    {
        engine::CameraConfig flatConfig;
        flatConfig.startPosition = glm::vec3{0.0f, 0.0f, 5.0f};
        const engine::Camera flat{flatConfig};
        engine::CameraConfig tiltedConfig;
        tiltedConfig.startPosition = glm::vec3{0.0f, 2.5f, 5.0f};
        tiltedConfig.startPitchDegrees = -26.565f;
        const engine::Camera tilted{tiltedConfig};
        glm::vec3 point{9.0f};
        const glm::vec2 viewport{800.0f, 600.0f};
        check(!engine::dropPointOnGround(flat, glm::vec2{400.0f, 300.0f}, viewport, point),
              "assets: a level camera ray never lands (fallback territory)");
        check(engine::dropPointOnGround(tilted, glm::vec2{400.0f, 300.0f}, viewport, point) &&
                  glm::distance(point, glm::vec3{0.0f}) < 0.05f,
              "assets: a downward centre ray lands on the floor under the crosshair");
        check(!engine::dropPointOnGround(flat, glm::vec2{400.0f, 0.0f}, viewport, point),
              "assets: a ray above the horizon lands nowhere");
    }

    // --- (f) Drag-and-drop state machine ------------------------------------------------------
    {
        AssetRig rig;
        const engine::UIRect source{50.0f, 50.0f, 120.0f, 30.0f};
        const engine::UIRect target{400.0f, 300.0f, 160.0f, 60.0f};
        auto sourceCall = [&] { return rig.ctx.beginDragSource(source, "AssetMesh", "models/cube.obj", "cube.obj"); };

        // Click without travel: no drag, no drop.
        rig.frame(60.0f, 60.0f, true);
        check(sourceCall() && !rig.ctx.dragActive(), "dnd: a press arms a pending drag only");
        rig.end();
        rig.frame(450.0f, 330.0f, false);
        check(!rig.ctx.dropTarget(target, "AssetMesh"), "dnd: an un-dragged click drops nothing");
        rig.end();

        // A real drag: promote, hover, deliver exactly once.
        rig.frame(60.0f, 60.0f, true);
        sourceCall();
        rig.end();
        rig.frame(90.0f, 90.0f, true);
        check(rig.ctx.dragActive() && rig.ctx.dragPayload().type == "AssetMesh" &&
                  rig.ctx.dragPayload().data == "models/cube.obj",
              "dnd: crossing the threshold promotes the drag with its payload");
        check(!rig.ctx.beginDragSource(engine::UIRect{600.0f, 60.0f, 100.0f, 30.0f}, "AssetTexture",
                                        "textures/checker.ppm", "checker.ppm") &&
                  rig.ctx.dragPayload().data == "models/cube.obj",
              "dnd: a second source cannot hijack a live drag");
        rig.end();
        rig.frame(450.0f, 330.0f, true);
        check(!rig.ctx.dropTarget(target, "AssetMesh") && rig.ctx.dropTargetHovered(target, "AssetMesh"),
              "dnd: targets report hover but deliver only on release");
        rig.end();
        rig.frame(450.0f, 330.0f, false);
        check(rig.ctx.dropTarget(target, "AssetMesh") && !rig.ctx.dropTarget(target, "AssetMesh"),
              "dnd: the drop delivers to exactly one target");
        rig.end();
        rig.frame(450.0f, 330.0f, false);
        check(!rig.ctx.dragActive() && rig.ctx.dragPayload().type.empty(), "dnd: the drag fully resets afterwards");
        rig.end();

        // Wrong-typed targets never consume; the matching one still can.
        rig.frame(60.0f, 60.0f, true);
        sourceCall();
        rig.end();
        rig.frame(90.0f, 90.0f, true);
        rig.end();
        rig.frame(450.0f, 330.0f, false);
        check(!rig.ctx.dropTarget(target, "AssetTexture") && rig.ctx.dropTarget(target, "AssetMesh"),
              "dnd: payload type must match the target contract");
        rig.end();

        // Released over nothing: no delivery, state still resets.
        rig.frame(60.0f, 60.0f, true);
        sourceCall();
        rig.end();
        rig.frame(90.0f, 90.0f, true);
        rig.end();
        rig.frame(700.0f, 550.0f, false);
        check(!rig.ctx.dropTarget(target, "AssetMesh"), "dnd: releasing off-target delivers nowhere");
        rig.end();

        // Esc cancels mid-drag.
        rig.frame(60.0f, 60.0f, true);
        sourceCall();
        rig.end();
        rig.frame(90.0f, 90.0f, true);
        rig.end();
        rig.frame(450.0f, 330.0f, true, true);
        check(!rig.ctx.dropTargetHovered(target, "AssetMesh"), "dnd: Esc kills target highlighting");
        rig.end();
        rig.frame(450.0f, 330.0f, false);
        check(!rig.ctx.dropTarget(target, "AssetMesh"), "dnd: a cancelled drag delivers nothing");
        rig.end();
    }

    // --- (g) Browser panel: navigation, drag payloads, activation -------------------------------
    {
        AssetRig rig;
        engine::UIAssetBrowser browser;
        browser.setRootPath(root);
        const engine::UIRect panel{0.0f, 0.0f, 360.0f, 240.0f};
        auto build = [&](float x, float y, bool down) {
            rig.frame(x, y, down);
            const engine::UIAssetBrowser::Result result = browser.build(rig.ctx, panel);
            rig.end();
            return result;
        };
        check(browser.entries().size() == 4u, "browser: opens on the project root");
        // Rows start at y=38, 22 px each: row 0 = models/, row 1 = textures/.
        build(30.0f, 49.0f, true);
        check(browser.selectedEntry() != nullptr && browser.selectedEntry()->name == "models",
              "browser: clicking a row selects it");
        build(30.0f, 49.0f, false);
        const engine::UIAssetBrowser::Result navResult = build(30.0f, 49.0f, true);
        check(navResult.navigated && browser.currentDirectory() == "models" &&
                  browser.entries().size() == 2u,
              "browser: double-clicking a folder navigates into it");
        build(30.0f, 49.0f, false);
        // Row 0 is now ".."; row 1 is cube.obj. Drag it into the viewport.
        rig.frame(30.0f, 71.0f, true);
        const engine::UIAssetBrowser::Result dragBuild = browser.build(rig.ctx, panel);
        (void)dragBuild;
        rig.end();
        rig.frame(300.0f, 400.0f, true);
        browser.build(rig.ctx, panel);
        rig.end();
        check(rig.ctx.dragActive() && rig.ctx.dragPayload().type == "AssetMesh" &&
                  rig.ctx.dragPayload().data == "models/cube.obj",
              "browser: item drags carry the kind payload and root-relative path");
        rig.frame(500.0f, 400.0f, false);
        check(rig.ctx.dropTarget(engine::UIRect{0.0f, 0.0f, 800.0f, 600.0f}, "AssetMesh"),
              "browser: the viewport accepts the mesh payload");
        rig.end();
        // Back up via the parent row, then re-enter: staged clicks
        // keep every double-click unambiguous.
        build(30.0f, 49.0f, true);
        build(30.0f, 49.0f, false);
        build(30.0f, 49.0f, true);
        check(browser.currentDirectory().empty() && browser.entries().size() == 4u,
              "browser: the parent row walks back to the root");
        build(30.0f, 49.0f, false);
        build(30.0f, 49.0f, true);
        build(30.0f, 49.0f, false);
        build(30.0f, 49.0f, true); // double-click models/ again
        build(30.0f, 49.0f, false);
        // Double-click the file itself -> activation (no navigation).
        build(30.0f, 71.0f, true);
        build(30.0f, 71.0f, false);
        const engine::UIAssetBrowser::Result activateResult = build(30.0f, 71.0f, true);
        check(activateResult.activated != nullptr && activateResult.activated->name == "cube.obj" &&
                  !activateResult.navigated && browser.currentDirectory() == "models",
              "browser: double-clicking a file activates it in place");
        build(30.0f, 71.0f, false);
    }

    // --- (h) Drop -> import -> spawn, as the application performs it ------------------------------
    {
        const std::filesystem::path objPath = root / "models" / "quad.obj";
        {
            std::ofstream out(objPath, std::ios::binary);
            out << "v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\nf 1 2 3\nf 1 3 4\n";
        }
        engine::VramBudget vram(64 * 1024 * 1024);
        engine::VramBudget staging(64 * 1024 * 1024);
        engine::AssetManager assets(vram, staging);
        engine::Scene scene;
        const engine::MeshId id = assets.loadObj("quad", objPath);
        const engine::MeshAsset* asset = assets.mesh("quad");
        check(id != engine::kInvalidAssetId && asset != nullptr, "assets: a dropped OBJ imports into the registry");
        const engine::Entity spawned =
            asset != nullptr ? scene.spawnImportedModel(*asset, "quad", glm::vec3{0.0f}, 1.0f, 1.0f) : engine::Entity{};
        auto& registry = scene.registry();
        const bool spawnOk = spawned.isValid() && registry.all_of<engine::MeshComponent>(spawned.handle()) &&
                             registry.get<engine::MeshComponent>(spawned.handle()).assetId == "quad" &&
                             registry.all_of<engine::ColliderComponent>(spawned.handle());
        check(spawnOk, "assets: the imported mesh spawns fully wired (mesh + collider)");
        const engine::MeshAsset emptyAsset{};
        check(!scene.spawnImportedModel(emptyAsset, "empty", glm::vec3{0.0f}).isValid(),
              "assets: spawning a geometry-less asset is refused");
    }

    std::filesystem::remove_all(root, ec);
}

int main() {
    testConfigLoading();
    testPresentationModes();
    testLogger();
    testCamera();
    testPhysics();
    testDynamicModulesAndMaterials();
    testShadowsDepthAndRotationSync();
    testPhysicsPhase11();
    testPhysicsSettling();
    testRaycasting();
    testSpatialPartitioning();
    testCollisionLayers();
    testJointConstraints();
    testEventBus();
    testInputSystem();
    testPhysicsEvents();
    testSceneHierarchy();
    testSceneSerialization();
    testPhase30Gbq();
    testPhase31PlayMode();
    testPhase32Pbr();
    testPhase33Export();
    testAsyncAssetPipeline();
    testPhase21CullingAndLighting();
    testPhase22PbrIblPost();
    testPhase23GameModules();
    testPhase24SpaceFlight();
    testPhase25ReflectionScripting();
    testPhase26UIRenderer();
    testPhase26Widgets();
    testPhase27Editor();
    testPhase28Gizmos();
    testPhase29Assets();
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
