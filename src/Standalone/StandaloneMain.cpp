// Standalone game player (Phase 33). The ship build's entry point:
// boots a GBQB bundle (or a raw GBQ scene) produced by the editor's
// "Export Game" command straight into the game — no editor shell
// (outliner/inspector/asset browser/gizmos never link into this
// binary; it depends on every engine library except EngineApp).
//
//   game-player [--config game.ini] [--bundle game.gbq]
//               [--scene scene.gbq] [--headless <frames>] [--help]
//
// Without --headless the player opens a window and runs the game
// loop (WASD + mouse camera, physics, native scripts, game module).
// --headless runs the fixed-step simulation with no window or GPU
// and exits 0 only when the bundle loads and every transform stays
// finite — the CI smoke test for an exported game.

#include "Assets/AssetManager.h"
#include "Core/EngineConfig.h"
#include "Core/GameLoop.h"
#include "Core/GameModule.h"
#include "Core/Jobs/JobSystem.h"
#include "Core/Logger.h"
#include "Core/ModuleManager.h"
#include "Geometry/Cube.h"
#include "Graphics/VramBudget.h"
#include "Graphics/GraphicsPipeline.h"
#include "Graphics/Renderer.h"
#include "Graphics/Swapchain.h"
#include "Graphics/VulkanContext.h"
#include "Platform/Input.h"
#include "Platform/Window.h"
#include "Scene/Components.h"
#include "Scene/NativeScript.h"
#include "Scene/ProjectExport.h"
#include "Scene/Scene.h"
#include "Scene/SceneBinarySerialization.h"
#include "Scene/Systems/CameraSystem.h"
#include "Scene/Systems/PhysicsSystem.h"
#include "Scene/Systems/AnimationSystem.h"
#include "Scene/Systems/PlayerSystem.h"
#include "Net/DedicatedServer.h"
#include "Scene/Systems/RenderSystem.h"
#include "Scene/Systems/RotationSystem.h"
#include "Scene/Systems/TransformSystem.h"

#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#ifndef ENGINE_SHADER_DIR
#define ENGINE_SHADER_DIR "shaders"
#endif

namespace {

std::atomic<bool> g_serverStop{false};
void requestServerStop(int) {
    g_serverStop.store(true, std::memory_order_relaxed);
}

struct Options {
    std::filesystem::path configPath; // game.ini / engine.ini
    std::filesystem::path bundlePath; // .gbq bundle (from Export Game)
    std::filesystem::path scenePath;  // raw scene .gbq (no bundle)
    long headlessFrames = -1;         // >= 0: headless simulation run
    bool help = false;
    // Dedicated-server mode (--server): no window, no Vulkan; the
    // same runtime as vibe_server, hosted by this binary.
    bool server = false;
    std::filesystem::path serverConfigPath = "server.cfg";
    long serverTicks = 0; // 0 = run until SIGINT/SIGTERM
};

void printUsage(const char* program) {
    std::cout << "Usage: " << program << " [--config <game.ini>] [--bundle <game.gbq>] [--scene <scene.gbq>]\n"
              << "            [--headless <frames>] [--help]\n"
              << "  --config <path>     Engine config (default lookup as the editor); its\n"
              << "                      [Game] bundle key selects the bundle when --bundle is absent\n"
              << "  --bundle <path>     GBQB deployment bundle written by Export Game\n"
              << "  --scene <path>      Raw scene .gbq, booted with built-in fallback assets\n"
              << "  --headless <frames> No window/GPU: load, run N fixed steps, validate, exit\n"
              << "  --server            Run as a headless dedicated server (server.cfg, no window/GPU)\n"
              << "  --server-cfg <path> server.cfg for --server (default: server.cfg)\n"
              << "  --server-ticks <n>  Stop the server after N fixed ticks (default: until signal)\n"
              << "  --help              Show this message\n";
}

bool parseArguments(int argc, char* argv[], Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto takeValue = [&](const char* flag, std::filesystem::path& target) {
            if (i + 1 < argc) {
                target = argv[++i];
                return true;
            }
            std::cerr << "Missing value for " << flag << '\n';
            return false;
        };
        if (argument == "--help" || argument == "-h") {
            options.help = true;
            return true;
        }
        if (argument == "--config") {
            if (!takeValue("--config", options.configPath)) {
                return false;
            }
        } else if (argument == "--bundle") {
            if (!takeValue("--bundle", options.bundlePath)) {
                return false;
            }
        } else if (argument == "--scene") {
            if (!takeValue("--scene", options.scenePath)) {
                return false;
            }
        } else if (argument == "--server") {
            options.server = true;
        } else if (argument == "--server-cfg") {
            if (!takeValue("--server-cfg", options.serverConfigPath)) {
                return false;
            }
        } else if (argument == "--server-ticks") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --server-ticks\n";
                return false;
            }
            char* end = nullptr;
            options.serverTicks = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || options.serverTicks < 0) {
                std::cerr << "--server-ticks expects a non-negative tick count\n";
                return false;
            }
        } else if (argument == "--headless") {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for --headless\n";
                return false;
            }
            char* end = nullptr;
            options.headlessFrames = std::strtol(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || options.headlessFrames < 0) {
                std::cerr << "--headless expects a non-negative frame count\n";
                return false;
            }
        } else {
            std::cerr << "Unknown argument: " << argument << '\n';
            return false;
        }
    }
    return true;
}

// The exported game.ini selects its bundle through [Game] bundle
// (an EngineConfig key since Phase 33); relative paths resolve
// against the configuration file's directory.
std::filesystem::path bundlePathFromConfig(const engine::EngineConfig& config) {
    if (config.game.bundlePath.empty()) {
        return {};
    }
    std::filesystem::path bundle = config.game.bundlePath;
    if (bundle.is_absolute() || config.sourcePath.empty()) {
        return bundle;
    }
    return std::filesystem::path(config.sourcePath).parent_path() / bundle;
}

// Registers the procedural fallbacks every shipped scene may assume
// (built-in cube + the sandbox palette), only where the bundle /
// scene did not provide assets under those names itself.
void registerFallbackAssets(engine::AssetManager& assets) {
    if (assets.mesh("cube") == nullptr) {
        assets.registerMesh("cube", engine::Cube::vertices(), engine::Cube::indices());
    }
    const auto ensureMaterial = [&assets](engine::MaterialDesc desc) {
        if (assets.material(desc.name) == nullptr) {
            assets.registerMaterial(std::move(desc));
        }
    };
    engine::MaterialDesc defaultMaterial = engine::makeSandboxCubeMaterial();
    defaultMaterial.name = "default"; // fallback for unassigned meshes
    ensureMaterial(defaultMaterial);
    ensureMaterial(engine::makeSandboxFloorMaterial());
    ensureMaterial(engine::makeSandboxCubeMaterial());
    ensureMaterial(engine::makeSandboxStackMaterial());
    ensureMaterial(engine::makeSandboxEnvironmentMaterial());
    ensureMaterial(engine::makeSandboxPlayerMaterial());
}

// Bytes a GBQB bundle ships for one registered shader; empty when
// the asset is not a shader the bundle carries.
bool extractBundleShader(const engine::BundleInfo& info, const std::filesystem::path& bundlePath,
                         const std::string& name, std::vector<std::byte>& outBytes) {
    const engine::BundleEntryInfo* entry = nullptr;
    for (const engine::BundleEntryInfo& candidate : info.entries) {
        if (candidate.kind == engine::BundleEntryKind::Shader && candidate.name == name) {
            entry = &candidate;
            break;
        }
    }
    return entry != nullptr && engine::extractBundleEntry(bundlePath, name, outBytes);
}

// GraphicsPipeline reads SPIR-V from ENGINE_SHADER_DIR; a deployed
// game extracts the bundle's shaders there, without overwriting a
// shader directory the install already provides.
void ensureBundleShadersOnDisk(const std::filesystem::path& bundlePath) {
    engine::BundleInfo info;
    if (!engine::inspectBundle(bundlePath, info)) {
        return;
    }
    const std::filesystem::path shaderDir = ENGINE_SHADER_DIR;
    for (const engine::BundleEntryInfo& entry : info.entries) {
        if (entry.kind != engine::BundleEntryKind::Shader) {
            continue;
        }
        const std::filesystem::path target = shaderDir / entry.name;
        std::error_code ec;
        if (std::filesystem::exists(target, ec)) {
            continue;
        }
        std::vector<std::byte> bytes;
        if (!extractBundleShader(info, bundlePath, entry.name, bytes)) {
            ENGINE_LOG_WARNING("Player: bundle shader '" << entry.name << "' failed its integrity check");
            continue;
        }
        std::filesystem::create_directories(shaderDir, ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (out) {
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            ENGINE_LOG_INFO("Player: extracted bundle shader '" << entry.name << "' to " << target.string());
        }
    }
}

// Script classes the bundle's scene needs but this build does not
// provide (scripts are compiled code — they ship inside the game
// module / executable, never inside the bundle). Booting continues
// degraded, exactly like a missing game module does.
void reportScriptRequirements(const engine::RuntimeLoadResult& result) {
    for (const std::string& name : result.scripts) {
        if (!engine::ScriptRegistry::instance().has(name)) {
            ENGINE_LOG_ERROR("Player: native script class '" << name
                                                              << "' is required by the scene but not provided by "
                                                                 "this build (register it or ship a game module)");
        }
    }
    if (result.ok) {
        ENGINE_LOG_INFO("Player: scene loaded (" << result.entityCount << " entities, "
                                                 << result.scripts.size() << " script requirement(s))");
    }
}

// ---------------------------------------------------------------------------
// Headless boot: bundle/scene load + fixed-step simulation with the
// same systems the windowed loop runs (minus input/render). Exit 0
// only when everything loaded and all transforms stayed finite.
// ---------------------------------------------------------------------------
int runHeadless(const engine::EngineConfig& config, const Options& options, const std::filesystem::path& bundlePath) {
    engine::VramBudget vram(static_cast<uint64_t>(config.render.vramBudgetMiB) * 1024 * 1024);
    engine::VramBudget staging(static_cast<uint64_t>(config.render.stagingBufferBudgetMiB) * 1024 * 1024);
    engine::AssetManager assets(vram, staging);
    engine::Scene scene;

    if (!options.scenePath.empty()) {
        if (!engine::deserializeScene(scene, options.scenePath)) {
            ENGINE_LOG_ERROR("Player: failed to load scene '" << options.scenePath.string() << "'");
            return EXIT_FAILURE;
        }
        registerFallbackAssets(assets);
        ENGINE_LOG_INFO("Player: scene '" << options.scenePath.string() << "' loaded (" << scene.entityCount()
                                          << " entities)");
    } else {
        const engine::RuntimeLoadResult result = engine::loadBundleIntoRuntime(bundlePath, assets, scene);
        if (!result.ok) {
            ENGINE_LOG_ERROR("Player: bundle load failed: " << result.error);
            return EXIT_FAILURE;
        }
        reportScriptRequirements(result);
        registerFallbackAssets(assets);
    }

    engine::JobSystem jobs(engine::JobSystem::Config{config.performance.jobWorkerCount,
                                                     config.performance.parallelBatchSize});
    engine::PhysicsSystem physics;
    const double fixedDelta = 1.0 / 60.0;

    std::unordered_map<entt::entity, glm::vec3> initialPositions;
    {
        auto& registry = scene.registry();
        for (const entt::entity entity : registry.view<engine::TransformComponent>()) {
            initialPositions.emplace(entity, registry.get<engine::TransformComponent>(entity).position);
        }
    }

    for (long frame = 0; frame < options.headlessFrames; ++frame) {
        engine::TransformSystem::update(scene);
        physics.update(scene, fixedDelta, config.physics, jobs);
        engine::RotationSystem::update(scene, fixedDelta);
        engine::TransformSystem::update(scene);
    }

    size_t movedBodies = 0;
    bool allFinite = true;
    auto& registry = scene.registry();
    for (const entt::entity entity : registry.view<engine::TransformComponent>()) {
        const engine::TransformComponent& transform = registry.get<engine::TransformComponent>(entity);
        const glm::vec3& p = transform.position;
        if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
            allFinite = false;
        }
        const auto initial = initialPositions.find(entity);
        if (initial != initialPositions.end()) {
            const glm::vec3 d = p - initial->second;
            if (d.x * d.x + d.y * d.y + d.z * d.z > 1e-8f) {
                ++movedBodies;
            }
        }
    }
    if (!allFinite) {
        ENGINE_LOG_ERROR("Player: non-finite transform after " << options.headlessFrames << " headless steps");
        return EXIT_FAILURE;
    }
    std::cout << "game-player headless: " << options.headlessFrames << " fixed steps OK, " << scene.entityCount()
              << " entities, " << movedBodies << " moved, all transforms finite\n";
    return EXIT_SUCCESS;
}

// ---------------------------------------------------------------------------
// Windowed boot: the editor's system bring-up minus every editor
// surface — window, Vulkan, pipelines, game loop, modules, scripts.
// ---------------------------------------------------------------------------
class PlayerRuntime {
public:
    explicit PlayerRuntime(engine::EngineConfig config)
        : m_config(std::move(config)),
          m_window(m_config.window.width, m_config.window.height, m_config.window.title.c_str(),
                   m_config.window.resizable, m_config.input.captureMouseOnStart),
          m_input(m_window), m_context(m_window),
          m_swapchain(m_context, m_window,
                      engine::PresentationSettings{m_config.render.presentMode, m_config.render.vsync}),
          m_pipeline(m_context, m_swapchain),
          m_renderer(m_context, m_window, m_swapchain, m_pipeline,
                     engine::Renderer::framesInFlightForPresentMode(m_swapchain.presentMode())),
          m_gameLoop(m_config.loop),
          m_jobSystem(engine::JobSystem::Config{m_config.performance.jobWorkerCount,
                                                m_config.performance.parallelBatchSize}),
          m_vramBudget(static_cast<uint64_t>(m_config.render.vramBudgetMiB) * 1024 * 1024),
          m_stagingBudget(static_cast<uint64_t>(m_config.render.stagingBufferBudgetMiB) * 1024 * 1024),
          m_assets(m_vramBudget, m_stagingBudget),
          m_renderSystem(m_renderer, m_swapchain, &m_jobSystem, m_config.render.frustumCulling,
                         m_config.performance.parallelMinDrawItems, m_config.render.sortDrawItems, &m_assets,
                         m_config.render.occlusionCulling) {
        m_renderer.setPostSettings(engine::PostSettings{static_cast<float>(m_config.render.exposure),
                                                        m_config.render.bloomEnabled,
                                                        static_cast<float>(m_config.render.bloomStrength),
                                                        static_cast<float>(m_config.render.bloomThreshold),
                                                        m_config.render.skyboxEnabled,
                                                        static_cast<float>(m_config.render.environmentIntensity)});
        // Optional native game module (same contract as the editor):
        // the engine runs unchanged without one.
        if (!m_config.game.modulePath.empty()) {
            engine::EngineContext context = buildModuleContext(0.0f, 0.0f);
            m_moduleManager.load(m_config.game.modulePath, context);
        }
    }

    ~PlayerRuntime() {
        // Same teardown contract as the editor: retire script
        // instances while the module manager's module is still
        // loaded (run() already shut them down on the normal
        // path; this covers early exits), then drain the GPU.
        engine::NativeScriptSystem::shutdown(m_scene);
        m_context.waitIdle();
    }

    PlayerRuntime(const PlayerRuntime&) = delete;
    PlayerRuntime& operator=(const PlayerRuntime&) = delete;

    bool loadGame(const Options& options, const std::filesystem::path& bundlePath) {
        if (!options.scenePath.empty()) {
            if (!engine::deserializeScene(m_scene, options.scenePath)) {
                ENGINE_LOG_ERROR("Player: failed to load scene '" << options.scenePath.string() << "'");
                return false;
            }
            ENGINE_LOG_INFO("Player: scene '" << options.scenePath.string() << "' loaded (" << m_scene.entityCount()
                                              << " entities)");
        } else {
            ensureBundleShadersOnDisk(bundlePath);
            const engine::RuntimeLoadResult result = engine::loadBundleIntoRuntime(bundlePath, m_assets, m_scene);
            if (!result.ok) {
                ENGINE_LOG_ERROR("Player: bundle load failed: " << result.error);
                return false;
            }
            reportScriptRequirements(result);
        }
        registerFallbackAssets(m_assets);
        // Singleplayer movement is character-driven: the local
        // player ticks the decoupled MovementSystem (friction 8 /
        // accelerate 6) directly in the fixed input loop through
        // PlayerSystem::update — no network wrappers involved.
        (void)engine::PlayerSystem::ensureCharacterController(m_scene);
        m_physicsSystem.captureSpawnStates(m_scene);
        // Deterministic PSO creation per registered material
        // (deduplicated by pipeline state + shader pair).
        for (engine::MaterialId id = 1; id <= m_assets.materialCount(); ++id) {
            const engine::MaterialAsset* material = m_assets.material(id);
            if (material == nullptr) {
                continue;
            }
            m_pipeline.getOrCreatePipeline(material->id, material->desc.pipeline, material->desc.vertexShader,
                                           material->desc.fragmentShader, m_swapchain);
        }
        ENGINE_LOG_INFO("Player: " << m_assets.meshCount() << " mesh(es), " << m_assets.materialCount()
                                   << " material(s), " << m_pipeline.cachedPipelineCount() << " pipeline(s)");
        return true;
    }

    void run() {
        ENGINE_LOG_INFO("Player started (fixed timestep "
                        << m_config.loop.fixedDeltaSeconds << "s, present mode "
                        << engine::Swapchain::presentModeName(m_swapchain.presentMode()) << ")");

        engine::LoopCallbacks callbacks;
        callbacks.shouldContinue = [this] { return !m_window.shouldClose(); };
        callbacks.pollEvents = [this] {
            m_input.beginFrame();
            m_window.pollEvents();
        };
        callbacks.fixedUpdate = [this](double fixedDeltaSeconds) {
            engine::TransformSystem::update(m_scene);
            engine::EngineContext context =
                buildModuleContext(0.0f, static_cast<float>(fixedDeltaSeconds));
            m_moduleManager.fixedUpdate(context, static_cast<float>(fixedDeltaSeconds));
            engine::NativeScriptSystem::fixedUpdate(context, static_cast<float>(fixedDeltaSeconds));
            // Animation playback + ragdoll transitions tick before
            // physics so exported games animate identically.
            engine::AnimationSystem::update(m_scene, fixedDeltaSeconds);
            engine::PlayerSystem::update(m_scene, m_input, fixedDeltaSeconds);
            m_physicsSystem.update(m_scene, fixedDeltaSeconds, m_config.physics, m_jobSystem);
            engine::PlayerSystem::syncCamera(m_scene);
            engine::RotationSystem::update(m_scene, fixedDeltaSeconds, m_jobSystem);
            engine::TransformSystem::update(m_scene);
        };
        callbacks.update = [this](double deltaSeconds) {
            m_moduleElapsedSeconds += deltaSeconds;
            ++m_moduleFrameIndex;
            engine::EngineContext context = buildModuleContext(static_cast<float>(deltaSeconds), 0.0f);
            m_moduleManager.update(context, static_cast<float>(deltaSeconds));
            engine::NativeScriptSystem::update(context, static_cast<float>(deltaSeconds));
            if (m_config.game.moduleHotReload) {
                m_secondsSinceModulePoll += deltaSeconds;
                if (m_secondsSinceModulePoll >= 0.5) {
                    m_secondsSinceModulePoll = 0.0;
                    m_moduleManager.pollForChanges(context);
                }
            }
            handleMouseCapture();
            engine::CameraSystem::update(m_scene, m_input, deltaSeconds);
        };
        callbacks.render = [this](double) { return m_renderSystem.render(m_scene); };

        m_gameLoop.run(callbacks);
        m_context.waitIdle();
        engine::NativeScriptSystem::shutdown(m_scene);
        ENGINE_LOG_INFO("Player shutting down");
    }

private:
    engine::EngineContext buildModuleContext(float deltaSeconds, float fixedDeltaSeconds) {
        engine::EngineContext context;
        context.scene = &m_scene;
        context.input = &m_input;
        context.assets = &m_assets;
        context.jobs = &m_jobSystem;
        context.scripts = &engine::ScriptRegistry::instance();
        context.deltaSeconds = deltaSeconds;
        context.fixedDeltaSeconds = fixedDeltaSeconds;
        context.elapsedSeconds = m_moduleElapsedSeconds;
        context.frameIndex = m_moduleFrameIndex;
        return context;
    }

    void handleMouseCapture() {
        if (m_input.wasKeyPressed(engine::Key::Escape)) {
            if (m_input.isMouseCaptured()) {
                m_input.setMouseCaptured(false);
            } else {
                m_window.requestClose();
            }
            return;
        }
        if (!m_input.isMouseCaptured() && m_config.input.captureMouseOnClick &&
            m_input.wasMouseButtonPressed(engine::MouseButton::Left)) {
            m_input.setMouseCaptured(true);
        }
    }

    engine::EngineConfig m_config;
    engine::Window m_window;
    engine::Input m_input;
    engine::VulkanContext m_context;
    engine::Swapchain m_swapchain;
    engine::GraphicsPipeline m_pipeline;
    engine::Renderer m_renderer;
    engine::GameLoop m_gameLoop;
    engine::JobSystem m_jobSystem;
    engine::VramBudget m_vramBudget;
    engine::VramBudget m_stagingBudget;
    engine::AssetManager m_assets;
    engine::Scene m_scene;
    engine::PhysicsSystem m_physicsSystem;
    engine::RenderSystem m_renderSystem;
    engine::ModuleManager m_moduleManager;
    double m_moduleElapsedSeconds = 0.0;
    uint64_t m_moduleFrameIndex = 0;
    double m_secondsSinceModulePoll = 0.0;
};

} // namespace

int main(int argc, char* argv[]) {
    Options options;
    if (!parseArguments(argc, argv, options)) {
        printUsage(argv[0]);
        return EXIT_FAILURE;
    }
    if (options.help) {
        printUsage(argv[0]);
        return EXIT_SUCCESS;
    }

    // Configuration first (the logger reads it), then the logger, so
    // every engine system can log — same bootstrap as the editor.
    engine::EngineConfig config = engine::EngineConfig::load(options.configPath.string());
    engine::Logger::instance().initialize(config.logger);

    if (options.server) {
        // Dedicated-server mode: the authoritative fixed-step
        // runtime only. No bundle resolution, no window, no GPU.
        std::signal(SIGINT, requestServerStop);
        std::signal(SIGTERM, requestServerStop);
        engine::DedicatedServerOptions serverOptions;
        serverOptions.configPath = options.serverConfigPath;
        serverOptions.scenePath = options.scenePath;
        serverOptions.maxTicks = options.serverTicks;
        const int serverExit = engine::runDedicatedServer(serverOptions, &g_serverStop);
        engine::Logger::instance().shutdown();
        return serverExit == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }
    for (const std::string& warning : config.loadWarnings) {
        ENGINE_LOG_WARNING(warning);
    }

    // Resolve what to boot: explicit bundle > explicit scene > the
    // config's [Game] bundle > a game.gbq beside the config/cwd.
    std::filesystem::path bundlePath = options.bundlePath;
    if (bundlePath.empty() && options.scenePath.empty()) {
        bundlePath = bundlePathFromConfig(config);
        if (bundlePath.empty() && !config.sourcePath.empty() &&
            std::filesystem::exists(std::filesystem::path(config.sourcePath).parent_path() / "game.gbq")) {
            bundlePath = std::filesystem::path(config.sourcePath).parent_path() / "game.gbq";
        }
        if (bundlePath.empty()) {
            bundlePath = "game.gbq";
        }
    }

    int exitCode = EXIT_SUCCESS;
    try {
        exitCode = options.headlessFrames >= 0 ? runHeadless(config, options, bundlePath) : EXIT_SUCCESS;
        if (options.headlessFrames < 0) {
            PlayerRuntime player(std::move(config));
            if (!player.loadGame(options, bundlePath)) {
                exitCode = EXIT_FAILURE;
            } else {
                player.run();
            }
        }
    } catch (const std::exception& error) {
        ENGINE_LOG_FATAL("Player fatal error: " << error.what());
        exitCode = EXIT_FAILURE;
    }

    engine::Logger::instance().shutdown();
    return exitCode;
}
