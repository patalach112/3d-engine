#pragma once

#include "Assets/AssetManager.h"
#include "Core/EngineConfig.h"
#include "Core/GameLoop.h"
#include "Core/Jobs/JobSystem.h"
#include "Graphics/GraphicsPipeline.h"
#include "Graphics/Renderer.h"
#include "Graphics/VramBudget.h"
#include "Graphics/Swapchain.h"
#include "Graphics/VulkanContext.h"
#include "Platform/Input.h"
#include "Platform/Window.h"
#include "Scene/Scene.h"
#include "Scene/Systems/PhysicsSystem.h"
#include "Scene/Systems/RenderSystem.h"

namespace engine {

// ---------------------------------------------------------------------------
// Application
//
// Composition root of the engine. Owns every subsystem by value and relies
// on member declaration order for both construction and destruction:
//
//   construct:  config -> Window -> Input -> VulkanContext -> swapchain ->
//               GraphicsPipeline -> Renderer -> GameLoop -> JobSystem ->
//               Scene -> RenderSystem
//   destruct:   exact reverse, so GPU resources are always released before
//               the device, surface, instance and window they depend on.
//               (Input unregisters itself from the Window while the Window
//               is still alive.)
//
// The Application no longer contains gameplay or camera logic: it builds
// the Scene, then lets ECS systems (camera, rotation, render) update and
// draw the world each frame. All tuning arrives through EngineConfig.
// ---------------------------------------------------------------------------
class Application {
public:
    explicit Application(EngineConfig config);
    ~Application();

    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;
    Application(Application&&) = delete;
    Application& operator=(Application&&) = delete;

    void run();

private:
    void registerAssets();
    void buildScene();
    void handleMouseCapture();
    void logLoopStats(const char* context) const;

    EngineConfig m_config;
    Window m_window;
    Input m_input;
    VulkanContext m_context;
    Swapchain m_swapchain;
    GraphicsPipeline m_pipeline;
    Renderer m_renderer;
    GameLoop m_gameLoop;
    JobSystem m_jobSystem;
    VramBudget m_vramBudget;
    VramBudget m_stagingBudget;
    AssetManager m_assets;
    Scene m_scene;
    PhysicsSystem m_physicsSystem;
    RenderSystem m_renderSystem;

    double m_secondsSinceStatsLog = 0.0;
    double m_secondsSinceHudUpdate = 0.0;
};

} // namespace engine
