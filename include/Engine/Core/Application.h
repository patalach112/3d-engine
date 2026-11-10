#pragma once
#include "Core/Export.h"

#include <functional>
#include <string>
#include <string_view>

#include "Assets/AssetManager.h"
#include "Core/EngineConfig.h"
#include "Core/EventBus.h"
#include "Core/GameLoop.h"
#include "Core/Jobs/JobSystem.h"
#include "Graphics/GraphicsPipeline.h"
#include "Core/ModuleManager.h"
#include "Graphics/Renderer.h"
#include "Graphics/UIContext.h"
#include "Graphics/VramBudget.h"
#include "Graphics/Swapchain.h"
#include "Graphics/VulkanContext.h"
#include "Platform/Input.h"
#include "Platform/Window.h"
#include "Scene/PlaySession.h"
#include "Scene/Scene.h"
#include "Scene/TransformGizmo.h"
#include "Scene/UIAssetBrowser.h"
#include "Scene/UIOutliner.h"
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
class ENGINE_APP_API Application {
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
    // Phase 29 asset drag-and-drop: spawn a dropped mesh into the
    // scene, or apply a dropped asset to the selected entity.
    void spawnDroppedMesh(std::string_view relativePath, glm::vec2 mousePixels, const Camera& camera,
                          glm::vec2 viewportSize);
    void applyDroppedAsset(entt::entity entity, const UIDragPayload& payload);

    // Phase 30 editor scene file commands (.gbq quicksave under
    // the asset root): save serializes the active registry; load
    // clears and restores it, dropping the editor selection first
    // so no stale entity handle survives the rebuild.
    void saveSceneToGbq();
    void loadSceneFromGbq();
    // Phase 33: "Export Game" (Edit mode only) — distills the
    // live scene and every asset it references into a deployable
    // GBQB bundle (plus manifest + game.ini) the standalone
    // game-player boots. Failures list every missing reference.
    void exportGame();

    // Phase 31 play mode: enterPlayMode snapshots the scene (GBQ
    // bytes) and starts the runtime simulation; exitPlayMode
    // restores the snapshot wholesale. Both drop the editor
    // selection so the inspector/gizmo never chase a stale or
    // recycled entity handle across the transition.
    void enterPlayMode();
    void exitPlayMode();
    // Registers (once) a material derived from the mesh's current
    // one and points the mesh at it. Returns the material name.
    std::string deriveMaterial(MeshComponent& mesh, const std::string& derivedName,
                               const std::function<void(MaterialDesc&)>& mutate);
    void handlePhysicsReset();
    void handleEventImpulse();
    void logLoopStats(const char* context) const;
    // Assembles the context handed to the game module (Phase 23).
    EngineContext buildModuleContext(float deltaSeconds, float fixedDeltaSeconds);

    EngineConfig m_config;
    Window m_window;
    Input m_input;
    VulkanContext m_context;
    Swapchain m_swapchain;
    GraphicsPipeline m_pipeline;
    Renderer m_renderer;
    UIContext m_uiContext; // Phase 26 Step 3 widget state (CPU-only)
    UIOutliner m_outliner; // Phase 27 entity hierarchy panel
    TransformGizmo m_gizmo; // Phase 28 viewport transform gizmo
    UIAssetBrowser m_assetBrowser; // Phase 29 asset browser panel
    PlaySession m_playSession; // Phase 31 Edit/Play state + scene snapshot
    GameLoop m_gameLoop;
    JobSystem m_jobSystem;
    VramBudget m_vramBudget;
    VramBudget m_stagingBudget;
    AssetManager m_assets;
    Scene m_scene;
    // Phase 17 event hub: Scene and PhysicsSystem publish lifecycle,
    // collision and trigger events here; the demo's logging hooks and
    // any future gameplay/audio/UI consumers subscribe to it.
    EventBus m_eventBus;
    EventBus::Subscription m_triggerLogSubscription;
    EventBus::Subscription m_collisionLogSubscription;
    EventBus::Subscription m_destroyedLogSubscription;
    PhysicsSystem m_physicsSystem;
    RenderSystem m_renderSystem;

    // Native game module (Phase 23): engine-owned; the scene it
    // drives survives its reloads. Empty [Game] modulePath = unused.
    ModuleManager m_moduleManager;
    double m_moduleElapsedSeconds = 0.0;
    uint64_t m_moduleFrameIndex = 0;

    double m_secondsSinceStatsLog = 0.0;
    double m_secondsSinceAssetPoll = 0.0;
    double m_secondsSinceModulePoll = 0.0;
    double m_secondsSinceHudUpdate = 0.0;

};

} // namespace engine
