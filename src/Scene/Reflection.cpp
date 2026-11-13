#include "Scene/Reflection.h"

#include "Scene/ComponentReflection.h"

#include <algorithm>

namespace engine {

namespace {

bool registerBuiltinTypes(ReflectionRegistry& registry) {
    registry.registerType(reflect<TransformComponent>());
    registry.registerType(reflect<RigidBodyComponent>());
    registry.registerType(reflect<PointLightComponent>());
    registry.registerType(reflect<FlashlightComponent>());
    registry.registerType(reflect<SpotLightComponent>());
    registry.registerType(reflect<DirectionalLightComponent>());
    registry.registerType(reflect<SpinComponent>());
    return true;
}

} // namespace

ReflectionRegistry& ReflectionRegistry::instance() {
    static ReflectionRegistry registry;
    // Built-in component types register on first use, so name-based
    // lookup works without any explicit engine init call.
    static const bool builtinsRegistered = registerBuiltinTypes(registry);
    (void)builtinsRegistered;
    return registry;
}

void ReflectionRegistry::registerType(const TypeInfo& info) {
    const auto existing = std::find_if(m_types.begin(), m_types.end(), [&info](const TypeInfo* candidate) {
        return candidate != nullptr && candidate->name == info.name;
    });
    if (existing != m_types.end()) {
        *existing = &info; // replace: re-registration after a reload
        return;
    }
    m_types.push_back(&info);
}

bool ReflectionRegistry::unregisterType(const TypeInfo& expected) {
    const auto it = std::find(m_types.begin(), m_types.end(), &expected);
    if (it == m_types.end()) {
        return false;
    }
    m_types.erase(it);
    return true;
}

const TypeInfo* ReflectionRegistry::find(std::string_view name) const {
    for (const TypeInfo* info : m_types) {
        if (info != nullptr && name == info->name) {
            return info;
        }
    }
    return nullptr;
}

} // namespace engine
