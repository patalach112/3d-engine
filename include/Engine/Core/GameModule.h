#pragma once

#include "Core/PluginApi.h"

#include <cstdint>

namespace engine {

class AssetManager;
class Input;
class JobSystem;
class Scene;

// ---------------------------------------------------------------------------
// EngineContext — everything a game module may touch, handed over by
// the host on every call.
//
// Ownership rules (the state-preservation contract of Phase 23):
//   * The engine owns the scene, input, assets and jobs. Modules
//     never create, destroy or replace engine systems — they only
//     run logic against them.
//   * The scene and every entity in it live in the core engine and
//     survive module loads, reloads and crashes untouched; a module
//     registers behavior, not world ownership.
//   * Nothing in here may be cached across an onUnload: after a
//     reload the module gets fresh calls with the same context.
// ---------------------------------------------------------------------------
struct EngineContext {
    Scene* scene = nullptr;
    const Input* input = nullptr;
    AssetManager* assets = nullptr;
    JobSystem* jobs = nullptr;
    float deltaSeconds = 0.0f;      // variable step, valid in onUpdate
    float fixedDeltaSeconds = 0.0f; // fixed step, valid in onFixedUpdate
    double elapsedSeconds = 0.0;    // host clock at call time
    uint64_t frameIndex = 0;        // monotonically increasing per frame
    // Opaque host <-> module channel, never read by the engine.
    // Hosts use it for out-of-band data (tests point it at a state
    // file path); production modules usually ignore it.
    void* userData = nullptr;
};

// ---------------------------------------------------------------------------
// IGameModule — the game logic plugin interface.
//
// Implementations live entirely inside the game shared library. The
// host resolves two C factories (CreateGameModule/DestroyGameModule,
// see ENGINE_IMPLEMENT_GAME_MODULE) so construction and destruction
// happen with the plugin's own allocator — objects never cross the
// boundary by value, and STL types never appear in this interface.
// Lifecycle: onLoad once after creation -> onUpdate / onFixedUpdate
// per frame -> onUnload before destruction. Any callback may throw;
// the host catches, logs, and falls back to running without a module
// (or keeps the previous generation on a failed reload).
// ---------------------------------------------------------------------------
class IGameModule {
public:
    virtual ~IGameModule() = default;

    virtual void onLoad(EngineContext& context) = 0;
    virtual void onUnload() = 0;
    virtual void onUpdate(EngineContext& context, float deltaSeconds) = 0;
    virtual void onFixedUpdate(EngineContext& context, float fixedDeltaSeconds) = 0;

    // Human-readable name for logs and diagnostics.
    [[nodiscard]] virtual const char* moduleName() const { return "GameModule"; }
};

// Factory signatures resolved by the host through the dynamic loader.
using CreateGameModuleFn = IGameModule* (*)();
using DestroyGameModuleFn = void (*)(IGameModule*);

inline constexpr const char* kCreateGameModuleSymbol = "CreateGameModule";
inline constexpr const char* kDestroyGameModuleSymbol = "DestroyGameModule";

} // namespace engine

// Stamps the two factory exports a game module must provide. Use
// once in the plugin's translation unit:
//
//   class MyGame final : public engine::IGameModule { ... };
//   ENGINE_IMPLEMENT_GAME_MODULE(MyGame)
#define ENGINE_IMPLEMENT_GAME_MODULE(ModuleClass)                                                         \
    extern "C" ENGINE_PLUGIN_API engine::IGameModule* CreateGameModule() { return new ModuleClass(); }    \
    extern "C" ENGINE_PLUGIN_API void DestroyGameModule(engine::IGameModule* module) { delete module; }
