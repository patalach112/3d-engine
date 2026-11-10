#include "Scene/NativeScript.h"

#include "Core/Logger.h"
#include "Scene/Components.h"

#include <exception>
#include <utility>

namespace engine {

// --- ScriptRegistry ---------------------------------------------------------

ScriptRegistry& ScriptRegistry::instance() {
    static ScriptRegistry registry;
    return registry;
}

void ScriptRegistry::registerScript(std::string name, Factory factory) {
    for (auto& [existingName, existingFactory] : m_factories) {
        if (existingName == name) {
            existingFactory = factory; // replace (hot reload)
            return;
        }
    }
    m_factories.emplace_back(std::move(name), factory);
}

bool ScriptRegistry::unregisterScript(std::string_view name, Factory expected) {
    for (auto it = m_factories.begin(); it != m_factories.end(); ++it) {
        if (it->first == name && it->second == expected) {
            m_factories.erase(it);
            return true;
        }
    }
    return false;
}

INativeScript* ScriptRegistry::create(std::string_view name) const {
    for (const auto& [existingName, factory] : m_factories) {
        if (existingName == name && factory) {
            return factory();
        }
    }
    return nullptr;
}

bool ScriptRegistry::has(std::string_view name) const {
    for (const auto& [existingName, factory] : m_factories) {
        if (existingName == name) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> ScriptRegistry::scriptNames() const {
    std::vector<std::string> names;
    names.reserve(m_factories.size());
    for (const auto& [name, factory] : m_factories) {
        names.push_back(name);
    }
    return names;
}

// --- NativeScriptSystem ------------------------------------------------------

namespace {

// Marker stored in the registry's context bag so the destruction
// hook is connected exactly once per scene.
struct ScriptHooksInstalled {};

void destroyScriptInstance(NativeScriptComponent& component) {
    if (component.instance == nullptr) {
        return;
    }
    try {
        component.instance->onDestroy();
    } catch (const std::exception& error) {
        ENGINE_LOG_WARNING("NativeScriptSystem: script onDestroy threw: " << error.what());
    } catch (...) {
        ENGINE_LOG_WARNING("NativeScriptSystem: script onDestroy threw an unknown exception");
    }
    delete component.instance;
    component.instance = nullptr;
    component.started = false;
}

void onScriptComponentDestroyed(Scene::Registry& registry, entt::entity entity) {
    destroyScriptInstance(registry.get<NativeScriptComponent>(entity));
}

// Instantiates (via the context's registry when the host provides
// one — game modules bridge their scripts in through
// EngineContext::scripts) and delivers pending awake/start hooks.
ScriptRegistry& activeRegistry(const EngineContext& context) {
    return context.scripts != nullptr ? *context.scripts : ScriptRegistry::instance();
}

} // namespace

void NativeScriptSystem::ensureLifecycleHooks(Scene& scene) {
    auto& registry = scene.registry();
    if (!registry.ctx().contains<ScriptHooksInstalled>()) {
        registry.ctx().emplace<ScriptHooksInstalled>();
        registry.on_destroy<NativeScriptComponent>().connect<&onScriptComponentDestroyed>();
    }
}

void NativeScriptSystem::prepareScripts(EngineContext& context) {
    Scene& scene = *context.scene;
    ensureLifecycleHooks(scene);
    ScriptRegistry& scripts = activeRegistry(context);
    auto view = scene.registry().view<NativeScriptComponent>();
    for (const entt::entity entity : view) {
        auto& component = view.get<NativeScriptComponent>(entity);
        if (component.instance == nullptr) {
            if (component.scriptName.empty()) {
                continue;
            }
            component.instance = scripts.create(component.scriptName);
            if (component.instance == nullptr) {
                continue; // unknown (or not yet registered) name: retry next tick
            }
            component.instance->bind(Entity{entity, &scene}, context);
            try {
                component.instance->onAwake();
            } catch (const std::exception& error) {
                ENGINE_LOG_WARNING("NativeScriptSystem: script '" << component.scriptName << "' onAwake threw: "
                                                                  << error.what());
            } catch (...) {
                ENGINE_LOG_WARNING("NativeScriptSystem: script '" << component.scriptName
                                                                  << "' onAwake threw an unknown exception");
            }
        }
        component.instance->refreshContext(context);
        if (!component.started) {
            component.started = true;
            try {
                component.instance->onStart();
            } catch (const std::exception& error) {
                ENGINE_LOG_WARNING("NativeScriptSystem: script '" << component.scriptName << "' onStart threw: "
                                                                  << error.what());
            } catch (...) {
                ENGINE_LOG_WARNING("NativeScriptSystem: script '" << component.scriptName
                                                                  << "' onStart threw an unknown exception");
            }
        }
    }
}

void NativeScriptSystem::update(EngineContext& context, float deltaSeconds) {
    if (context.scene == nullptr) {
        return;
    }
    prepareScripts(context);
    auto view = context.scene->registry().view<NativeScriptComponent>();
    for (const entt::entity entity : view) {
        auto& component = view.get<NativeScriptComponent>(entity);
        if (component.instance == nullptr) {
            continue;
        }
        try {
            component.instance->onUpdate(deltaSeconds);
        } catch (const std::exception& error) {
            ENGINE_LOG_WARNING("NativeScriptSystem: script '" << component.scriptName << "' onUpdate threw: "
                                                              << error.what());
        } catch (...) {
            ENGINE_LOG_WARNING("NativeScriptSystem: script '" << component.scriptName
                                                              << "' onUpdate threw an unknown exception");
        }
    }
}

void NativeScriptSystem::fixedUpdate(EngineContext& context, float fixedDeltaSeconds) {
    if (context.scene == nullptr) {
        return;
    }
    prepareScripts(context);
    auto view = context.scene->registry().view<NativeScriptComponent>();
    for (const entt::entity entity : view) {
        auto& component = view.get<NativeScriptComponent>(entity);
        if (component.instance == nullptr) {
            continue;
        }
        try {
            component.instance->onFixedUpdate(fixedDeltaSeconds);
        } catch (const std::exception& error) {
            ENGINE_LOG_WARNING("NativeScriptSystem: script '" << component.scriptName << "' onFixedUpdate threw: "
                                                              << error.what());
        } catch (...) {
            ENGINE_LOG_WARNING("NativeScriptSystem: script '" << component.scriptName
                                                              << "' onFixedUpdate threw an unknown exception");
        }
    }
}

void NativeScriptSystem::shutdown(Scene& scene) {
    ensureLifecycleHooks(scene);
    auto view = scene.registry().view<NativeScriptComponent>();
    for (const entt::entity entity : view) {
        destroyScriptInstance(view.get<NativeScriptComponent>(entity));
    }
}

} // namespace engine
