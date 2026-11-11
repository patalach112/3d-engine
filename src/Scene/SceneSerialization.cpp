// Scene serialization (Phase 19): the whole Scene as one JSON
// document (see Core/Json.h for the format machinery and Scene.h for
// the contract). Entities are referenced by their index in the
// document's entity array — EnTT handles are never persisted, since
// they recycle. Hierarchy is stored as the parent index only and
// rebuilt through Scene::setParent; joint `other` links are
// remapped to fresh handles after every entity exists.

#include "Scene/Scene.h"
#include "Scripting/DSLComponents.h"

#include "Core/Json.h"
#include "Core/Logger.h"

#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace engine {
namespace {

using json::Value;

constexpr int kSceneFormatVersion = 1;

// --- JSON writers -----------------------------------------------------------

Value vec2Json(const glm::vec2& value) {
    Value array = Value::array();
    array.push(Value::number(value.x));
    array.push(Value::number(value.y));
    return array;
}

Value vec3Json(const glm::vec3& value) {
    Value array = Value::array();
    array.push(Value::number(value.x));
    array.push(Value::number(value.y));
    array.push(Value::number(value.z));
    return array;
}

Value quatJson(const glm::quat& value) { // [w, x, y, z]
    Value array = Value::array();
    array.push(Value::number(value.w));
    array.push(Value::number(value.x));
    array.push(Value::number(value.y));
    array.push(Value::number(value.z));
    return array;
}

Value mat3Json(const glm::mat3& value) { // column-major, 9 numbers
    Value array = Value::array();
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            array.push(Value::number(value[column][row]));
        }
    }
    return array;
}

const char* colliderTypeName(ColliderComponent::Type type) {
    switch (type) {
        case ColliderComponent::Type::Box: return "box";
        case ColliderComponent::Type::Sphere: return "sphere";
        case ColliderComponent::Type::Plane: return "plane";
        case ColliderComponent::Type::Obb: return "obb";
        case ColliderComponent::Type::Capsule: return "capsule";
        case ColliderComponent::Type::ConvexHull: return "convexHull";
    }
    return "box";
}

bool colliderTypeFromName(const std::string& name, ColliderComponent::Type& out) {
    if (name == "box") { out = ColliderComponent::Type::Box; return true; }
    if (name == "sphere") { out = ColliderComponent::Type::Sphere; return true; }
    if (name == "plane") { out = ColliderComponent::Type::Plane; return true; }
    if (name == "obb") { out = ColliderComponent::Type::Obb; return true; }
    if (name == "capsule") { out = ColliderComponent::Type::Capsule; return true; }
    if (name == "convexHull") { out = ColliderComponent::Type::ConvexHull; return true; }
    return false;
}

const char* jointTypeName(JointType type) {
    switch (type) {
        case JointType::Distance: return "distance";
        case JointType::Hinge: return "hinge";
        case JointType::Fixed: return "fixed";
        case JointType::SpringDamper: return "springDamper";
    }
    return "distance";
}

bool jointTypeFromName(const std::string& name, JointType& out) {
    if (name == "distance") { out = JointType::Distance; return true; }
    if (name == "hinge") { out = JointType::Hinge; return true; }
    if (name == "fixed") { out = JointType::Fixed; return true; }
    if (name == "springDamper") { out = JointType::SpringDamper; return true; }
    return false;
}

// --- JSON readers (tolerant: missing/mistyped fields keep defaults) ----------

float getFloat(const Value& object, const char* key, float fallback) {
    const Value* value = object.find(key);
    return value != nullptr ? static_cast<float>(value->asNumber(fallback)) : fallback;
}

bool getBool(const Value& object, const char* key, bool fallback) {
    const Value* value = object.find(key);
    return value != nullptr ? value->asBool(fallback) : fallback;
}

std::string getString(const Value& object, const char* key, const std::string& fallback) {
    const Value* value = object.find(key);
    return value != nullptr && value->type() == Value::Type::String ? value->asString() : fallback;
}

uint32_t getUInt(const Value& object, const char* key, uint32_t fallback) {
    const Value* value = object.find(key);
    return value != nullptr ? static_cast<uint32_t>(value->asNumber(fallback)) : fallback;
}

glm::vec2 getVec2(const Value& object, const char* key, const glm::vec2& fallback) {
    const Value* value = object.find(key);
    if (value == nullptr || value->items().size() < 2) {
        return fallback;
    }
    return glm::vec2{static_cast<float>(value->items()[0].asNumber(fallback.x)),
                     static_cast<float>(value->items()[1].asNumber(fallback.y))};
}

glm::vec3 getVec3(const Value& object, const char* key, const glm::vec3& fallback) {
    const Value* value = object.find(key);
    if (value == nullptr || value->items().size() < 3) {
        return fallback;
    }
    return glm::vec3{static_cast<float>(value->items()[0].asNumber(fallback.x)),
                     static_cast<float>(value->items()[1].asNumber(fallback.y)),
                     static_cast<float>(value->items()[2].asNumber(fallback.z))};
}

glm::quat getQuat(const Value& object, const char* key, const glm::quat& fallback) {
    const Value* value = object.find(key);
    if (value == nullptr || value->items().size() < 4) {
        return fallback;
    }
    return glm::quat{static_cast<float>(value->items()[0].asNumber(fallback.w)),
                     static_cast<float>(value->items()[1].asNumber(fallback.x)),
                     static_cast<float>(value->items()[2].asNumber(fallback.y)),
                     static_cast<float>(value->items()[3].asNumber(fallback.z))};
}

glm::mat3 getMat3(const Value& object, const char* key, const glm::mat3& fallback) {
    const Value* value = object.find(key);
    if (value == nullptr || value->items().size() < 9) {
        return fallback;
    }
    glm::mat3 result{1.0f};
    size_t index = 0;
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row, ++index) {
            result[column][row] = static_cast<float>(value->items()[index].asNumber(fallback[column][row]));
        }
    }
    return result;
}

// Index reference stored for a related entity, or -1 when the stored
// value says "none" (null/missing/out of range).
int getEntityIndex(const Value& object, const char* key) {
    const Value* value = object.find(key);
    if (value == nullptr || value->type() != Value::Type::Number) {
        return -1;
    }
    return static_cast<int>(value->asNumber(-1.0));
}

} // namespace

std::string Scene::saveToString() const {
    // Every entity carrying at least one serialized component.
    std::vector<entt::entity> entities;
    std::unordered_map<uint32_t, int> indexOf;
    const auto collect = [&](const auto& view) {
        for (const entt::entity entity : view) {
            if (indexOf.find(entt::to_integral(entity)) == indexOf.end()) {
                indexOf[entt::to_integral(entity)] = static_cast<int>(entities.size());
                entities.push_back(entity);
            }
        }
    };
    collect(m_registry.view<TagComponent>());
    collect(m_registry.view<TransformComponent>());
    collect(m_registry.view<CameraComponent>());
    collect(m_registry.view<MeshComponent>());
    collect(m_registry.view<SpinComponent>());
    collect(m_registry.view<RigidBodyComponent>());
    collect(m_registry.view<ColliderComponent>());
    collect(m_registry.view<GravityFieldComponent>());
    collect(m_registry.view<JointComponent>());
    collect(m_registry.view<ThrusterComponent>());
    collect(m_registry.view<ParentComponent>());
    collect(m_registry.view<ChildrenComponent>());

    const auto entityIndex = [&](entt::entity entity) -> Value {
        const auto found = indexOf.find(entt::to_integral(entity));
        return found != indexOf.end() ? Value::number(found->second) : Value::null();
    };

    Value entityArray = Value::array();
    for (const entt::entity entity : entities) {
        Value entityObject = Value::object();

        if (const ParentComponent* parent = m_registry.try_get<ParentComponent>(entity);
            parent != nullptr && parent->parent != entt::null && m_registry.valid(parent->parent)) {
            entityObject.set("parent", entityIndex(parent->parent));
        } else {
            entityObject.set("parent", Value::null());
        }

        Value components = Value::object();
        if (const TagComponent* tag = m_registry.try_get<TagComponent>(entity); tag != nullptr) {
            components.set("tag", Value::string(tag->tag));
        }
        if (const TransformComponent* transform = m_registry.try_get<TransformComponent>(entity);
            transform != nullptr) {
            Value object = Value::object();
            object.set("position", vec3Json(transform->position));
            object.set("rotationDegrees", vec3Json(transform->rotationDegrees));
            object.set("scale", vec3Json(transform->scale));
            object.set("orientation", quatJson(transform->orientation));
            object.set("useQuaternionOrientation", Value::boolean(transform->useQuaternionOrientation));
            components.set("transform", std::move(object));
        }
        if (const CameraComponent* component = m_registry.try_get<CameraComponent>(entity);
            component != nullptr) {
            const CameraConfig& config = component->camera.config();
            Value camera = Value::object();
            camera.set("startPosition", vec3Json(config.startPosition));
            camera.set("startYawDegrees", Value::number(config.startYawDegrees));
            camera.set("startPitchDegrees", Value::number(config.startPitchDegrees));
            camera.set("movementSpeed", Value::number(config.movementSpeed));
            camera.set("boostMultiplier", Value::number(config.boostMultiplier));
            camera.set("mouseSensitivity", Value::number(config.mouseSensitivity));
            camera.set("invertY", Value::boolean(config.invertY));
            camera.set("lookSmoothingSeconds", Value::number(config.lookSmoothingSeconds));
            camera.set("movementSmoothingSeconds", Value::number(config.movementSmoothingSeconds));
            camera.set("pitchLimitDegrees", Value::number(config.pitchLimitDegrees));
            camera.set("fieldOfViewDegrees", Value::number(config.fieldOfViewDegrees));
            camera.set("nearPlane", Value::number(config.nearPlane));
            camera.set("farPlane", Value::number(config.farPlane));
            Value object = Value::object();
            object.set("isActive", Value::boolean(component->isActive));
            object.set("config", std::move(camera));
            object.set("position", vec3Json(component->camera.position()));
            object.set("yawDegrees", Value::number(component->camera.yawDegrees()));
            object.set("pitchDegrees", Value::number(component->camera.pitchDegrees()));
            components.set("camera", std::move(object));
        }
        if (const MeshComponent* mesh = m_registry.try_get<MeshComponent>(entity); mesh != nullptr) {
            Value bounds = Value::object();
            bounds.set("min", vec3Json(mesh->localBounds.min));
            bounds.set("max", vec3Json(mesh->localBounds.max));
            Value object = Value::object();
            object.set("assetId", Value::string(mesh->assetId));
            object.set("materialName", Value::string(mesh->materialName));
            object.set("visible", Value::boolean(mesh->visible));
            object.set("localBounds", std::move(bounds));
            components.set("mesh", std::move(object));
        }
        if (const SpinComponent* spin = m_registry.try_get<SpinComponent>(entity); spin != nullptr) {
            Value object = Value::object();
            object.set("degreesPerSecond", vec3Json(spin->degreesPerSecond));
            components.set("spin", std::move(object));
        }
        if (const ScriptComponent* script = m_registry.try_get<ScriptComponent>(entity); script != nullptr) {
            Value object = Value::object();
            object.set("filePath", Value::string(script->filePath));
            object.set("source", Value::string(script->source));
            object.set("compileFlags", Value::number(static_cast<double>(static_cast<uint32_t>(script->compileFlags))));
            object.set("enabled", Value::boolean(script->enabled));
            components.set("script", std::move(object));
        }
        if (const RigidBodyComponent* body = m_registry.try_get<RigidBodyComponent>(entity);
            body != nullptr) {
            Value object = Value::object();
            object.set("velocity", vec3Json(body->velocity));
            object.set("angularVelocity", vec3Json(body->angularVelocity));
            object.set("angularMomentum", vec3Json(body->angularMomentum));
            object.set("orientation", quatJson(body->orientation));
            object.set("inertiaTensor", mat3Json(body->inertiaTensor));
            object.set("inverseInertiaTensor", mat3Json(body->inverseInertiaTensor));
            object.set("mass", Value::number(body->mass));
            object.set("inverseMass", Value::number(body->inverseMass));
            object.set("restitution", Value::number(body->restitution));
            object.set("friction", Value::number(body->friction));
            object.set("anisotropicFriction", vec2Json(body->anisotropicFriction));
            object.set("gravityScale", Value::number(body->gravityScale));
            object.set("isStatic", Value::boolean(body->isStatic));
            object.set("useGravity", Value::boolean(body->useGravity));
            object.set("autoInertia", Value::boolean(body->autoInertia));
            object.set("orientationInitialized", Value::boolean(body->orientationInitialized));
            object.set("sleepTimer", Value::number(body->sleepTimer));
            object.set("sleeping", Value::boolean(body->sleeping));
            object.set("allowSleep", Value::boolean(body->allowSleep));
            components.set("rigidBody", std::move(object));
        }
        if (const ColliderComponent* collider = m_registry.try_get<ColliderComponent>(entity);
            collider != nullptr) {
            Value hullPoints = Value::array();
            for (size_t i = 0; i < collider->hullPoints.size(); ++i) {
                hullPoints.push(vec3Json(collider->hullPoints[i]));
            }
            Value object = Value::object();
            object.set("type", Value::string(colliderTypeName(collider->type)));
            object.set("halfExtents", vec3Json(collider->halfExtents));
            object.set("centerOffset", vec3Json(collider->centerOffset));
            object.set("radius", Value::number(collider->radius));
            object.set("capsuleHalfHeight", Value::number(collider->capsuleHalfHeight));
            object.set("planeNormal", vec3Json(collider->planeNormal));
            object.set("planeOffset", Value::number(collider->planeOffset));
            object.set("hullPoints", std::move(hullPoints));
            object.set("hullPointCount", Value::number(collider->hullPointCount));
            object.set("isTrigger", Value::boolean(collider->isTrigger));
            object.set("layer", Value::number(collider->layer));
            object.set("mask", Value::number(collider->mask));
            components.set("collider", std::move(object));
        }
        if (const GravityFieldComponent* field = m_registry.try_get<GravityFieldComponent>(entity);
            field != nullptr) {
            Value object = Value::object();
            object.set("gravitationalParameter", Value::number(field->gravitationalParameter));
            object.set("radius", Value::number(field->radius));
            object.set("enabled", Value::boolean(field->enabled));
            components.set("gravityField", std::move(object));
        }
        if (const JointComponent* joint = m_registry.try_get<JointComponent>(entity); joint != nullptr) {
            Value object = Value::object();
            object.set("type", Value::string(jointTypeName(joint->type)));
            object.set("other",
                       joint->other != entt::null && m_registry.valid(joint->other)
                           ? entityIndex(joint->other)
                           : Value::null());
            object.set("anchorA", vec3Json(joint->anchorA));
            object.set("anchorB", vec3Json(joint->anchorB));
            object.set("hingeAxis", vec3Json(joint->hingeAxis));
            object.set("restLength", Value::number(joint->restLength));
            object.set("minDistance", Value::number(joint->minDistance));
            object.set("maxDistance", Value::number(joint->maxDistance));
            object.set("minAngleDegrees", Value::number(joint->minAngleDegrees));
            object.set("maxAngleDegrees", Value::number(joint->maxAngleDegrees));
            object.set("enableLimits", Value::boolean(joint->enableLimits));
            object.set("compliance", Value::number(joint->compliance));
            object.set("stiffness", Value::number(joint->stiffness));
            object.set("damping", Value::number(joint->damping));
            object.set("enabled", Value::boolean(joint->enabled));
            components.set("joint", std::move(object));
        }
        if (const ThrusterComponent* thruster = m_registry.try_get<ThrusterComponent>(entity);
            thruster != nullptr) {
            Value object = Value::object();
            object.set("force", vec3Json(thruster->force));
            object.set("torque", vec3Json(thruster->torque));
            object.set("localPosition", vec3Json(thruster->localPosition));
            object.set("throttle", Value::number(thruster->throttle));
            object.set("enabled", Value::boolean(thruster->enabled));
            components.set("thruster", std::move(object));
        }

        entityObject.set("components", std::move(components));
        entityArray.push(std::move(entityObject));
    }

    Value root = Value::object();
    root.set("format", Value::string("engine-scene"));
    root.set("version", Value::number(kSceneFormatVersion));
    root.set("hierarchyDestroyMode",
             Value::string(m_hierarchyDestroyMode == HierarchyDestroyMode::Cascade ? "cascade" : "orphan"));
    root.set("entities", std::move(entityArray));
    return json::stringify(root);
}

bool Scene::loadFromString(std::string_view text) {
    std::string error;
    Value root;
    if (!json::parse(text, root, &error)) {
        ENGINE_LOG_WARNING("Scene: load rejected, invalid JSON: " << error);
        return false;
    }
    if (root.type() != Value::Type::Object || getString(root, "format", "") != "engine-scene") {
        ENGINE_LOG_WARNING("Scene: load rejected, not an engine-scene document");
        return false;
    }
    if (const Value* version = root.find("version");
        version == nullptr || static_cast<int>(version->asNumber(-1.0)) != kSceneFormatVersion) {
        ENGINE_LOG_WARNING("Scene: load rejected, unsupported format version");
        return false;
    }
    const Value* entityArray = root.find("entities");
    if (entityArray == nullptr || entityArray->type() != Value::Type::Array) {
        ENGINE_LOG_WARNING("Scene: load rejected, missing entity array");
        return false;
    }

    // Wholesale replace: no EntityDestroyedEvents — the old world
    // ends silently, exactly as if the Scene had been destroyed.
    m_registry.clear();
    m_entityCount = 0;
    m_raycastTree.clear();
    m_raycastProxies.clear();
    m_hierarchyDestroyMode =
        getString(root, "hierarchyDestroyMode", "orphan") == "cascade" ? HierarchyDestroyMode::Cascade
                                                                        : HierarchyDestroyMode::Orphan;

    // Pass 1: create every entity and apply its own components.
    // Hierarchy links and joint `other` references resolve in pass 2,
    // so the document's entity order does not matter.
    std::vector<entt::entity> handles;
    handles.reserve(entityArray->items().size());
    std::vector<std::pair<entt::entity, int>> jointFixups;
    for (const Value& entityObject : entityArray->items()) {
        Entity entity = createEntity("Entity");
        handles.push_back(entity.handle());
        const Value* components = entityObject.find("components");
        if (components == nullptr) {
            continue;
        }
        if (const Value* tag = components->find("tag");
            tag != nullptr && tag->type() == Value::Type::String) {
            entity.getComponent<TagComponent>().tag = tag->asString();
        }
        if (const Value* object = components->find("transform"); object != nullptr) {
            TransformComponent& transform = entity.addComponent<TransformComponent>();
            transform.position = getVec3(*object, "position", transform.position);
            transform.rotationDegrees = getVec3(*object, "rotationDegrees", transform.rotationDegrees);
            transform.scale = getVec3(*object, "scale", transform.scale);
            transform.orientation = getQuat(*object, "orientation", transform.orientation);
            transform.useQuaternionOrientation =
                getBool(*object, "useQuaternionOrientation", transform.useQuaternionOrientation);
        }
        if (const Value* object = components->find("camera"); object != nullptr) {
            CameraConfig config;
            if (const Value* camera = object->find("config"); camera != nullptr) {
                config.startPosition = getVec3(*camera, "startPosition", config.startPosition);
                config.startYawDegrees = getFloat(*camera, "startYawDegrees", config.startYawDegrees);
                config.startPitchDegrees = getFloat(*camera, "startPitchDegrees", config.startPitchDegrees);
                config.movementSpeed = getFloat(*camera, "movementSpeed", config.movementSpeed);
                config.boostMultiplier = getFloat(*camera, "boostMultiplier", config.boostMultiplier);
                config.mouseSensitivity = getFloat(*camera, "mouseSensitivity", config.mouseSensitivity);
                config.invertY = getBool(*camera, "invertY", config.invertY);
                config.lookSmoothingSeconds =
                    getFloat(*camera, "lookSmoothingSeconds", config.lookSmoothingSeconds);
                config.movementSmoothingSeconds =
                    getFloat(*camera, "movementSmoothingSeconds", config.movementSmoothingSeconds);
                config.pitchLimitDegrees = getFloat(*camera, "pitchLimitDegrees", config.pitchLimitDegrees);
                config.fieldOfViewDegrees =
                    getFloat(*camera, "fieldOfViewDegrees", config.fieldOfViewDegrees);
                config.nearPlane = getFloat(*camera, "nearPlane", config.nearPlane);
                config.farPlane = getFloat(*camera, "farPlane", config.farPlane);
            }
            // The live pose wins over the config's start pose.
            config.startPosition = getVec3(*object, "position", config.startPosition);
            config.startYawDegrees = getFloat(*object, "yawDegrees", config.startYawDegrees);
            config.startPitchDegrees = getFloat(*object, "pitchDegrees", config.startPitchDegrees);
            CameraComponent& component = entity.addComponent<CameraComponent>(config);
            component.isActive = getBool(*object, "isActive", component.isActive);
        }
        if (const Value* object = components->find("mesh"); object != nullptr) {
            MeshComponent& mesh = entity.addComponent<MeshComponent>();
            mesh.assetId = getString(*object, "assetId", mesh.assetId);
            mesh.materialName = getString(*object, "materialName", mesh.materialName);
            mesh.visible = getBool(*object, "visible", mesh.visible);
            if (const Value* bounds = object->find("localBounds"); bounds != nullptr) {
                mesh.localBounds.min = getVec3(*bounds, "min", mesh.localBounds.min);
                mesh.localBounds.max = getVec3(*bounds, "max", mesh.localBounds.max);
            }
        }
        if (const Value* object = components->find("spin"); object != nullptr) {
            SpinComponent& spin = entity.addComponent<SpinComponent>();
            spin.degreesPerSecond = getVec3(*object, "degreesPerSecond", spin.degreesPerSecond);
        }
        if (const Value* object = components->find("script"); object != nullptr) {
            ScriptComponent& script = entity.addComponent<ScriptComponent>();
            script.filePath = getString(*object, "filePath", script.filePath);
            script.source = getString(*object, "source", script.source);
            script.compileFlags =
                static_cast<ScriptCompileFlags>(static_cast<uint32_t>(getFloat(*object, "compileFlags", 0.0f)));
            script.enabled = getBool(*object, "enabled", script.enabled);
        }
        if (const Value* object = components->find("rigidBody"); object != nullptr) {
            RigidBodyComponent& body = entity.addComponent<RigidBodyComponent>();
            body.velocity = getVec3(*object, "velocity", body.velocity);
            body.angularVelocity = getVec3(*object, "angularVelocity", body.angularVelocity);
            body.angularMomentum = getVec3(*object, "angularMomentum", body.angularMomentum);
            body.orientation = getQuat(*object, "orientation", body.orientation);
            body.inertiaTensor = getMat3(*object, "inertiaTensor", body.inertiaTensor);
            body.inverseInertiaTensor = getMat3(*object, "inverseInertiaTensor", body.inverseInertiaTensor);
            body.mass = getFloat(*object, "mass", body.mass);
            body.inverseMass = getFloat(*object, "inverseMass", body.inverseMass);
            body.restitution = getFloat(*object, "restitution", body.restitution);
            body.friction = getFloat(*object, "friction", body.friction);
            body.anisotropicFriction = getVec2(*object, "anisotropicFriction", body.anisotropicFriction);
            body.gravityScale = getFloat(*object, "gravityScale", body.gravityScale);
            body.isStatic = getBool(*object, "isStatic", body.isStatic);
            body.useGravity = getBool(*object, "useGravity", body.useGravity);
            body.autoInertia = getBool(*object, "autoInertia", body.autoInertia);
            body.orientationInitialized = getBool(*object, "orientationInitialized", body.orientationInitialized);
            body.sleepTimer = getFloat(*object, "sleepTimer", body.sleepTimer);
            body.sleeping = getBool(*object, "sleeping", body.sleeping);
            body.allowSleep = getBool(*object, "allowSleep", body.allowSleep);
        }
        if (const Value* object = components->find("collider"); object != nullptr) {
            ColliderComponent& collider = entity.addComponent<ColliderComponent>();
            ColliderComponent::Type type = collider.type;
            if (colliderTypeFromName(getString(*object, "type", "box"), type)) {
                collider.type = type;
            }
            collider.halfExtents = getVec3(*object, "halfExtents", collider.halfExtents);
            collider.centerOffset = getVec3(*object, "centerOffset", collider.centerOffset);
            collider.radius = getFloat(*object, "radius", collider.radius);
            collider.capsuleHalfHeight = getFloat(*object, "capsuleHalfHeight", collider.capsuleHalfHeight);
            collider.planeNormal = getVec3(*object, "planeNormal", collider.planeNormal);
            collider.planeOffset = getFloat(*object, "planeOffset", collider.planeOffset);
            collider.hullPoints.fill(glm::vec3{0.0f});
            if (const Value* hull = object->find("hullPoints"); hull != nullptr) {
                size_t count = 0;
                for (const Value& point : hull->items()) {
                    if (count >= collider.hullPoints.size() || point.items().size() < 3) {
                        break;
                    }
                    collider.hullPoints[count++] = glm::vec3{static_cast<float>(point.items()[0].asNumber()),
                                                             static_cast<float>(point.items()[1].asNumber()),
                                                             static_cast<float>(point.items()[2].asNumber())};
                }
            }
            collider.hullPointCount = static_cast<uint8_t>(getUInt(*object, "hullPointCount", 0u));
            collider.isTrigger = getBool(*object, "isTrigger", collider.isTrigger);
            collider.layer = getUInt(*object, "layer", collider.layer);
            collider.mask = getUInt(*object, "mask", collider.mask);
        }
        if (const Value* object = components->find("gravityField"); object != nullptr) {
            GravityFieldComponent& field = entity.addComponent<GravityFieldComponent>();
            field.gravitationalParameter =
                getFloat(*object, "gravitationalParameter", field.gravitationalParameter);
            field.radius = getFloat(*object, "radius", field.radius);
            field.enabled = getBool(*object, "enabled", field.enabled);
        }
        if (const Value* object = components->find("joint"); object != nullptr) {
            JointComponent& joint = entity.addComponent<JointComponent>();
            JointType type = joint.type;
            if (jointTypeFromName(getString(*object, "type", "distance"), type)) {
                joint.type = type;
            }
            joint.anchorA = getVec3(*object, "anchorA", joint.anchorA);
            joint.anchorB = getVec3(*object, "anchorB", joint.anchorB);
            joint.hingeAxis = getVec3(*object, "hingeAxis", joint.hingeAxis);
            joint.restLength = getFloat(*object, "restLength", joint.restLength);
            joint.minDistance = getFloat(*object, "minDistance", joint.minDistance);
            joint.maxDistance = getFloat(*object, "maxDistance", joint.maxDistance);
            joint.minAngleDegrees = getFloat(*object, "minAngleDegrees", joint.minAngleDegrees);
            joint.maxAngleDegrees = getFloat(*object, "maxAngleDegrees", joint.maxAngleDegrees);
            joint.enableLimits = getBool(*object, "enableLimits", joint.enableLimits);
            joint.compliance = getFloat(*object, "compliance", joint.compliance);
            joint.stiffness = getFloat(*object, "stiffness", joint.stiffness);
            joint.damping = getFloat(*object, "damping", joint.damping);
            joint.enabled = getBool(*object, "enabled", joint.enabled);
            jointFixups.emplace_back(entity.handle(), getEntityIndex(*object, "other"));
        }
        if (const Value* object = components->find("thruster"); object != nullptr) {
            ThrusterComponent& thruster = entity.addComponent<ThrusterComponent>();
            thruster.force = getVec3(*object, "force", thruster.force);
            thruster.torque = getVec3(*object, "torque", thruster.torque);
            thruster.localPosition = getVec3(*object, "localPosition", thruster.localPosition);
            thruster.throttle = getFloat(*object, "throttle", thruster.throttle);
            thruster.enabled = getBool(*object, "enabled", thruster.enabled);
        }
    }

    // Pass 2: hierarchy links (via setParent, so both sides of every
    // link are built consistently) and joint partner remapping.
    for (size_t i = 0; i < entityArray->items().size(); ++i) {
        const int parentIndex = getEntityIndex(entityArray->items()[i], "parent");
        if (parentIndex >= 0 && static_cast<size_t>(parentIndex) < handles.size()) {
            setParent(Entity{handles[i], this}, Entity{handles[static_cast<size_t>(parentIndex)], this});
        }
    }
    for (const auto& [jointEntity, otherIndex] : jointFixups) {
        JointComponent& joint = m_registry.get<JointComponent>(jointEntity);
        joint.other = otherIndex >= 0 && static_cast<size_t>(otherIndex) < handles.size()
                          ? handles[static_cast<size_t>(otherIndex)]
                          : entt::null;
    }

    ENGINE_LOG_INFO("Scene: loaded " << handles.size() << " entities from serialized document");
    return true;
}

bool Scene::saveToFile(const std::string& filepath) const {
    std::ofstream file{filepath, std::ios::trunc};
    if (!file) {
        ENGINE_LOG_WARNING("Scene: could not open '" << filepath << "' for writing");
        return false;
    }
    file << saveToString();
    return static_cast<bool>(file);
}

bool Scene::loadFromFile(const std::string& filepath) {
    std::ifstream file{filepath};
    if (!file) {
        ENGINE_LOG_WARNING("Scene: could not open '" << filepath << "' for reading");
        return false;
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    return loadFromString(buffer.str());
}

} // namespace engine
