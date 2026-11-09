// Unit tests for the Phase 2 core infrastructure: configuration loading,
// logger filtering/rotation, fixed-timestep math, Time and GameLoop
// dispatch. These cover the parts of the engine that can be verified
// without a GPU or a window.

#include "Core/Camera.h"
#include "Core/EngineConfig.h"
#include "Core/GameLoop.h"
#include "Core/Logger.h"
#include "Core/Time.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/vec4.hpp>

#include "Scene/Scene.h"
#include "Scene/Systems/RenderSystem.h"
#include "Scene/Systems/RotationSystem.h"

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
             << "[Loop]\nmaxFrameSeconds = not_a_number\n"
             << "[Camera]\nmovementSpeed = 7.5\nmouseSensitivity = 0.25\npositionZ = 5\n"
             << "[Input]\ncaptureMouseOnStart = false\n";
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
    check(nearlyEqual(config.camera.movementSpeed, 7.5), "camera movement speed parsed");
    check(nearlyEqual(config.camera.mouseSensitivity, 0.25), "camera mouse sensitivity parsed");
    check(nearlyEqual(config.camera.startPosition.z, 5.0), "camera start position parsed");
    check(!config.input.captureMouseOnStart, "input capture flag parsed");
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

    const engine::PreparedFrame before = engine::RenderSystem::prepareFrame(scene, 16.0f / 9.0f);
    check(before.hasCamera, "render prep finds the active camera");
    check(before.drawItems.size() == 1, "render prep collects visible mesh entities");

    // This is what CameraSystem does with input each frame (look + move);
    // exercised through the camera's window-free API here.
    playerCamera.camera.translate(glm::vec3{0.0f, 0.0f, 1.0f}, 1.0); // 3 units forward
    const engine::PreparedFrame after = engine::RenderSystem::prepareFrame(scene, 16.0f / 9.0f);
    check(!nearlyEqual(after.view[3][2], before.view[3][2], 1e-6),
          "render prep after camera update uses the new view matrix");
    check(nearlyEqual(after.drawItems[0].model[3][0], before.drawItems[0].model[3][0]),
          "camera movement does not disturb entity transforms");

    // Hidden entities are excluded from rendering.
    engine::Entity hidden = scene.createEntity("Hidden");
    hidden.addComponent<engine::TransformComponent>();
    auto& hiddenMesh = hidden.addComponent<engine::MeshComponent>("cube");
    hiddenMesh.visible = false;
    check(engine::RenderSystem::prepareFrame(scene, 1.0f).drawItems.size() == 1,
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
