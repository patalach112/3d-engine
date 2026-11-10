#pragma once

#include "Core/Export.h"

#include "Core/GameModule.h" // EngineContext (shared with game modules)
#include "Scene/Scene.h"

#include <string>
#include <string_view>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// Native scripting (Phase 25, step 1) — Unity/UE-style behaviour
// components in plain C++.
//
// A script is a class deriving from INativeScript with lifecycle
// hooks. Entities carry a NativeScriptComponent { scriptName,
// instance }: the name is the serialization identifier, the
// instance is created on demand from ScriptRegistry (a name ->
// factory directory where classes self-register with
// REGISTER_SCRIPT). NativeScriptSystem drives the lifecycle from
// the host loop (Application, or a game module hosting its own
// scripts):
//
//   attach -> onAwake -> onStart -> onUpdate / onFixedUpdate ...
//          -> onDestroy (entity or component destroyed)
//
// Scripts run on the main thread, alongside the other ECS systems;
// the EngineContext snapshot refreshed before every callback batch
// gives them the same controlled engine surface as game modules
// (scene, input, assets, jobs — a script may enqueue JobSystem
// work, it just gets ticked on the main thread).
// ---------------------------------------------------------------------------

class INativeScript {
public:
    virtual ~INativeScript() = default;

    virtual void onAwake() {}
    virtual void onStart() {}
    virtual void onUpdate(float /*deltaSeconds*/) {}
    virtual void onFixedUpdate(float /*fixedDeltaSeconds*/) {}
    virtual void onDestroy() {}

    // The entity this script instance is attached to, its scene,
    // and the latest host context snapshot.
    [[nodiscard]] Entity entity() const { return m_entity; }
    [[nodiscard]] Scene* scene() const { return m_context.scene; }
    [[nodiscard]] const EngineContext& context() const { return m_context; }

    template <typename T>
    [[nodiscard]] T& getComponent() {
        return m_entity.getComponent<T>();
    }
    template <typename T>
    [[nodiscard]] bool hasComponent() const {
        return m_entity.hasComponent<T>();
    }

private:
    friend class NativeScriptSystem;

    void bind(Entity entity, const EngineContext& context) {
        m_entity = entity;
        m_context = context;
    }
    void refreshContext(const EngineContext& context) { m_context = context; }

    Entity m_entity;
    EngineContext m_context;
};

// Name -> script factory directory. Registration is idempotent by
// name (re-registering replaces the factory — hot reload swaps
// implementations).
//
// The factory is a plain function pointer, never a std::function:
// a std::function created inside a game module keeps its
// type-erasure manager in that binary, and destroying it after the
// module was dlclosed would call into unmapped code. A function
// pointer destroys trivially; liveness is handled by unregistering
// (below) before the owning binary unloads.
class ENGINE_SCENE_API ScriptRegistry {
public:
    using Factory = INativeScript* (*)();

    [[nodiscard]] static ScriptRegistry& instance();

    void registerScript(std::string name, Factory factory);
    // Erases the entry only if it still holds `expected` — a
    // retiring module generation must not remove the factory its
    // replacement just registered under the same name.
    bool unregisterScript(std::string_view name, Factory expected);
    // New instance owned by the caller; nullptr for unknown names.
    [[nodiscard]] INativeScript* create(std::string_view name) const;
    [[nodiscard]] bool has(std::string_view name) const;
    [[nodiscard]] std::vector<std::string> scriptNames() const;

private:
    ScriptRegistry() = default;
    std::vector<std::pair<std::string, Factory>> m_factories;
};

namespace detail {
template <typename T>
INativeScript* createScriptInstance() {
    return new T();
}
} // namespace detail

template <typename T>
bool registerScriptType(const char* name) {
    ScriptRegistry::instance().registerScript(name, &detail::createScriptInstance<T>);
    return true;
}

} // namespace engine

// Self-registration for a script class, usable in any .cpp:
//
//   class SpaceFlightScript final : public engine::INativeScript { ... };
//   REGISTER_SCRIPT(SpaceFlightScript)
//
// The instance is created by the registry when an entity's
// NativeScriptComponent names this class.
#define REGISTER_SCRIPT(ScriptClass) \
    static const bool scriptRegistered_##ScriptClass = ::engine::registerScriptType<ScriptClass>(#ScriptClass);

namespace engine {

// Drives NativeScriptComponent lifecycles for one scene. Stateless:
// all per-script state lives in the components themselves.
class ENGINE_SCENE_API NativeScriptSystem {
public:
    static void update(EngineContext& context, float deltaSeconds);
    static void fixedUpdate(EngineContext& context, float fixedDeltaSeconds);
    // Runs onDestroy and deletes every live script instance in the
    // scene (orderly teardown; entity destruction does the same via
    // the component's on_destroy hook, installed on first use).
    static void shutdown(Scene& scene);

private:
    static void ensureLifecycleHooks(Scene& scene);
    static void prepareScripts(EngineContext& context);
};

} // namespace engine
