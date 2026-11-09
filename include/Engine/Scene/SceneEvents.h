#pragma once

#include <entt/entity/entity.hpp>

namespace engine {

// Published by Scene on its EventBus (see Scene::setEventBus)
// immediately BEFORE an entity is destroyed, while its components
// are still readable — the cleanup-hook point for systems that keep
// per-entity state elsewhere.
struct EntityDestroyedEvent {
    entt::entity entity = entt::null;
};

} // namespace engine
