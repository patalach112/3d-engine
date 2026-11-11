#pragma once
#include "Core/Export.h"

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

#include "Assets/AssetManager.h"
#include "Core/EngineConfig.h"
#include "Core/EventBus.h"
#include "Core/GameLoop.h"
#include "Core/Jobs/JobSystem.h"
#include "Graphics/EditorLayout.h"
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
#include "Scripting/ScriptSystem.h"
#include "Graphics/TextEditor.h"
#include "Graphics/ConsoleLog.h"
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
    // Editor layout for the window's *current* framebuffer size,
    // recomputed whenever needed (resize/DPI changes reflow the
    // docks on the very next frame -- nothing caches window size).
    [[nodiscard]] EditorLayout currentEditorLayout() const;
    // GLFW mouse position converted into framebuffer/UI pixels.
    [[nodiscard]] glm::vec2 uiMousePosition() const;
    // Edit-mode viewport navigation: hold the right mouse button
    // inside the viewport to look around. The cursor is never
    // captured in Edit mode.
    void updateEditorCamera(double deltaSeconds);
    // Phase 29 asset drag-and-drop: spawn a dropped mesh into the
    // scene, or apply a dropped asset to the selected entity.
    void spawnDroppedMesh(std::string_view relativePath, glm::vec2 mousePixels, const Camera& camera,
                          const UIRect& viewport);
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
    // Editor feedback + context actions: a transient status-bar
    // notification (Save/Load/Export results, context-menu actions)
    // and the Focus camera move shared by both context menus.
    void setStatusMessage(std::string message);
    void focusEditorCameraOn(entt::entity entity);
    // Arms the inspector rename field for `entity` (tag copied into
    // the edit buffer, keyboard focus grabbed on its first frame).
    void beginRename(entt::entity entity);
    // Script dock: binds the text editor to an entity's
    // ScriptComponent, commits buffer -> component (+ .dsla file)
    // with an immediate parse verdict, renders the inspector's
    // script section and the bottom-dock script tab, and drives
    // the dock splitters.
    void bindScriptEditor(entt::entity entity);
    void saveAndCompileScript();
    void resetScriptRuntimes();
    float inspectScriptAt(float x, float y, float width, entt::entity entity);
    void drawScriptDock(const UIRect& rect, double deltaSeconds);
    void updateSplitters(const EditorLayout& layout);
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
    // Backrooms hybrid scripting: DSL runtimes + event queue for
    // every ScriptComponent in the scene (fed by m_eventBus).
    ScriptSystemContext m_scriptContext;

    // Native game module (Phase 23): engine-owned; the scene it
    // drives survives its reloads. Empty [Game] modulePath = unused.
    ModuleManager m_moduleManager;
    double m_moduleElapsedSeconds = 0.0;
    uint64_t m_moduleFrameIndex = 0;

    // Last computed dock layout (refreshed at the top of every
    // update/render from the live framebuffer size). Input handling
    // (viewport capture, editor look) reads this; rendering
    // recomputes and republishes it each frame.
    EditorLayout m_editorLayout{};
    std::string m_statsText; // toolbar stats line (TrueType UI text)

    // Editor interaction state. The inspector dock scrolls with the
    // wheel (content height is last frame's stack, the standard
    // immediate-mode feedback loop); the scroll resets whenever the
    // selection changes. Rename is armed by either context menu and
    // edited in a field at the top of the inspector. The viewport
    // context menu opens on a right *click* (press+release without
    // travel), so right-drag camera look never raises it.
    float m_inspectorScroll = 0.0f;        // pixels scrolled in the Inspector dock
    float m_inspectorContentHeight = 0.0f; // last frame's inspector stack height
    entt::entity m_inspectorSelection = entt::null; // selection the scroll belongs to
    entt::entity m_renameTarget = entt::null;
    std::string m_renameBuffer;
    bool m_renameFocusPending = false;
    entt::entity m_viewportContextTarget = entt::null;
    glm::vec2 m_rightPressPos{0.0f, 0.0f};
    // True while the editor fly-look owns the GLFW cursor capture:
    // engaged by an RMB press inside the viewport, held for the
    // whole drag (cursor hidden/locked, raw deltas drive the
    // camera), released with the button.
    bool m_editorFlyCapture = false;
    bool m_rightPressInViewport = false;
    // Script editor + resizable docks: splitter drag state feeds
    // computeEditorLayout every frame, so the scene viewport (and
    // with it projection/picking) follows panel sizes immediately.
    EditorDockSizes m_dockSizes;
    int m_activeSplitter = 0;  // 0 none, 1 hierarchy, 2 inspector, 3 bottom dock
    int m_hoveredSplitter = 0; // same encoding, for hover/cursor feedback
    enum class BottomDockTab : uint8_t { Assets, Script };
    BottomDockTab m_bottomDockTab = BottomDockTab::Assets;
    TextEditor m_scriptEditor;
    std::string m_scriptStatus; // last Save & Compile verdict for the bound script
    UIRect m_scriptEditorRect{}; // last drawn script-dock rect (I-beam cursor)
    // In-engine console: Logger listener -> ring buffer -> panel.
    // Visible in Edit and Play (layout.console strip).
    ConsoleLog m_consoleLog;
    ConsolePanelState m_consoleState;
    float m_consoleHeight = 170.0f; // remembered strip height when open
    uint64_t m_consoleListenerId = 0;
    std::string m_statusMessage;           // transient status-bar notification
    float m_statusSecondsRemaining = 0.0f; // decays in the update callback
    std::filesystem::path m_activeScenePath; // scene file Save/Load last used

    double m_lastUpdateDeltaSeconds = 0.0; // update() dt, reused by render-time widgets
    double m_secondsSinceStatsLog = 0.0;
    double m_secondsSinceAssetPoll = 0.0;
    double m_secondsSinceModulePoll = 0.0;
    double m_secondsSinceHudUpdate = 0.0;

};

} // namespace engine
