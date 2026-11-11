#include "Scripting/ScriptSystem.h"

#include "Core/EventBus.h"
#include "Core/Logger.h"
#include "Physics/PhysicsEvents.h"
#include "Platform/Input.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scripting/DSLComponents.h"

#include <glm/geometric.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <utility>

namespace engine {

using Runtime = ScriptSystemContext::Runtime;
using Timer = ScriptSystemContext::Timer;
using Lerp = ScriptSystemContext::Lerp;
using ActiveHudPrint = ScriptSystemContext::ActiveHudPrint;

namespace {

[[nodiscard]] entt::entity findByName(Scene::Registry& registry, const std::string& name) {
    entt::entity found = entt::null;
    registry.view<TagComponent>().each([&](entt::entity e, const TagComponent& tag) {
        if (found == entt::null && tag.tag == name) {
            found = e;
        }
    });
    return found;
}

[[nodiscard]] std::string nameOf(Scene::Registry& registry, entt::entity e) {
    if (e != entt::null && registry.valid(e) && registry.all_of<TagComponent>(e)) {
        return registry.get<TagComponent>(e).tag;
    }
    return {};
}

struct Frame {
    Scene& scene;
    ScriptSystemContext& context;
    Runtime& runtime;
    entt::entity self = entt::null;
    entt::entity contextEntity = entt::null;  // part("x") { } scope
    entt::entity positionEntity = entt::null; // on_touch/position("zone") scope
    const DSLEvent* event = nullptr;
    uint32_t group = 0; // temporal group (For window owns its children)
    std::vector<entt::entity>& destroyQueue;
    std::vector<std::pair<const DSLNode*, Frame>>* armed = nullptr; // nested handler arming
    bool dedupeTimers = false; // arming contexts spawn standing timers once
    std::vector<ScriptSystemContext::LatchedHandler>* latchSink = nullptr; // event-in-event latching
};

[[nodiscard]] bool holdsItemIn(const Runtime& rt, const std::string& item) {
    return std::find(rt.heldItems.begin(), rt.heldItems.end(), item) != rt.heldItems.end();
}

[[nodiscard]] double evaluateExpr(const DSLExpr& expr, const Runtime& rt) {
    return ScriptInterpreter::evaluate(expr, rt.variables, rt.textVariables);
}

// Text-side evaluation: strings and bare identifiers-as-tags.
[[nodiscard]] std::optional<std::string> evaluateText(const DSLExpr& expr, const Runtime& rt) {
    if (expr.kind == DSLExpr::Kind::Text) {
        return expr.text;
    }
    if (expr.kind == DSLExpr::Kind::Variable) {
        const auto it = rt.textVariables.find(expr.text);
        if (it != rt.textVariables.end()) {
            return it->second;
        }
        if (rt.variables.find(expr.text) == rt.variables.end()) {
            return expr.text; // bare tag: behavior = stalker
        }
    }
    return std::nullopt;
}

void applyBehavior(Frame& frame, entt::entity target, const std::string& behaviorName) {
    auto& registry = frame.scene.registry();
    if (target == entt::null || !registry.valid(target)) {
        return;
    }
    auto& ai = registry.get_or_emplace<AIBehaviorComponent>(target);
    if (behaviorName == "stalker") {
        ai.behavior = AIBehavior::Stalker;
        ai.active = true;
    } else if (behaviorName == "none") {
        ai.behavior = AIBehavior::None;
    } else {
        ENGINE_LOG_WARNING("Script: unknown behavior '" << behaviorName << "' (expected 'stalker')");
        ai.behavior = AIBehavior::None;
    }
}

// ---------------------------------------------------------------------------
// Bounds, part properties and lerp plumbing (DSL expansion)
// ---------------------------------------------------------------------------

[[nodiscard]] double clampToBounds(const Runtime& rt, const std::string& name, double value) {
    const auto it = rt.bounds.find(name);
    if (it == rt.bounds.end()) {
        return value;
    }
    if (it->second.hasLowest) {
        value = std::max(value, it->second.lowest);
    }
    if (it->second.hasHighest) {
        value = std::min(value, it->second.highest);
    }
    return value;
}

bool applyPartProperty(Scene::Registry& registry, entt::entity target, const std::string& property,
                       const std::string& axis, double value) {
    if (target == entt::null || !registry.valid(target)) {
        return false;
    }
    if (property == "intensity") {
        if (registry.all_of<PointLightComponent>(target)) {
            registry.get<PointLightComponent>(target).intensity = static_cast<float>(value);
            return true;
        }
        if (registry.all_of<SpotLightComponent>(target)) {
            registry.get<SpotLightComponent>(target).intensity = static_cast<float>(value);
            return true;
        }
        if (registry.all_of<DirectionalLightComponent>(target)) {
            registry.get<DirectionalLightComponent>(target).intensity = static_cast<float>(value);
            return true;
        }
        return false;
    }
    if (property == "scale") {
        if (!registry.all_of<TransformComponent>(target)) {
            return false;
        }
        auto& scale = registry.get<TransformComponent>(target).scale;
        if (axis == "x") {
            scale.x = static_cast<float>(value);
        } else if (axis == "y") {
            scale.y = static_cast<float>(value);
        } else if (axis == "z") {
            scale.z = static_cast<float>(value);
        } else {
            return false;
        }
        return true;
    }
    if (property == "range") {
        if (registry.all_of<PointLightComponent>(target)) {
            registry.get<PointLightComponent>(target).range = static_cast<float>(value);
            return true;
        }
        if (registry.all_of<AudioComponent>(target)) {
            registry.get<AudioComponent>(target).range = static_cast<float>(value);
            return true;
        }
        return false;
    }
    if (property == "volume") {
        registry.get_or_emplace<AudioComponent>(target).volume = static_cast<float>(value);
        return true;
    }
    return false;
}

[[nodiscard]] std::optional<double> readPartProperty(Scene::Registry& registry, entt::entity target,
                                                     const std::string& property, const std::string& axis) {
    if (target == entt::null || !registry.valid(target)) {
        return std::nullopt;
    }
    if (property == "intensity") {
        if (registry.all_of<PointLightComponent>(target)) {
            return registry.get<PointLightComponent>(target).intensity;
        }
        if (registry.all_of<SpotLightComponent>(target)) {
            return registry.get<SpotLightComponent>(target).intensity;
        }
        if (registry.all_of<DirectionalLightComponent>(target)) {
            return registry.get<DirectionalLightComponent>(target).intensity;
        }
    } else if (property == "scale") {
        if (registry.all_of<TransformComponent>(target)) {
            const auto& scale = registry.get<TransformComponent>(target).scale;
            if (axis == "x") return scale.x;
            if (axis == "y") return scale.y;
            if (axis == "z") return scale.z;
        }
    } else if (property == "range") {
        if (registry.all_of<PointLightComponent>(target)) {
            return registry.get<PointLightComponent>(target).range;
        }
        if (const AudioComponent* audio = registry.try_get<AudioComponent>(target)) {
            return audio->range;
        }
    } else if (property == "volume") {
        if (const AudioComponent* audio = registry.try_get<AudioComponent>(target)) {
            return audio->volume;
        }
    }
    return std::nullopt;
}

void applyLerpTarget(Scene& scene, ScriptSystemContext& context, Runtime& rt, const Lerp& lerp, double value) {
    switch (lerp.target) {
    case Lerp::Target::Variable:
        rt.variables[lerp.name] = clampToBounds(rt, lerp.name, value);
        return;
    case Lerp::Target::PartProperty:
        applyPartProperty(scene.registry(), findByName(scene.registry(), lerp.name), lerp.property, lerp.axis, value);
        return;
    case Lerp::Target::EffectProperty:
        if (lerp.name == "chromatic_aberration") {
            context.effects.chromaticAberration = value;
        }
        return;
    }
}

void cancelLerps(Runtime& rt, Lerp::Target target, const std::string& name, const std::string& property,
                 const std::string& axis) {
    rt.lerps.erase(std::remove_if(rt.lerps.begin(), rt.lerps.end(),
                                  [&](const Lerp& lerp) {
                                      return lerp.target == target && lerp.name == name &&
                                             lerp.property == property && lerp.axis == axis;
                                  }),
                   rt.lerps.end());
}

// Spawns (or replaces) a non-blocking interpolation toward `to`.
// The start value is whatever the target reads right now, so
// re-issuing an `over` assignment mid-flight bends smoothly from
// the current position instead of snapping back.
void startLerp(Frame& frame, Lerp::Target target, const std::string& name, const std::string& property,
               const std::string& axis, double to, double duration) {
    Runtime& rt = frame.runtime;
    cancelLerps(rt, target, name, property, axis);
    double from = to;
    if (target == Lerp::Target::Variable) {
        to = clampToBounds(rt, name, to);
        const auto it = rt.variables.find(name);
        from = it != rt.variables.end() ? it->second : 0.0;
    } else if (target == Lerp::Target::PartProperty) {
        auto& registry = frame.scene.registry();
        if (const auto current = readPartProperty(registry, findByName(registry, name), property, axis)) {
            from = *current;
        }
    } else if (name == "chromatic_aberration") {
        from = frame.context.effects.chromaticAberration;
    }
    if (duration <= 0.0 || from == to) {
        Lerp instant;
        instant.target = target;
        instant.name = name;
        instant.property = property;
        instant.axis = axis;
        applyLerpTarget(frame.scene, frame.context, rt, instant, to);
        return;
    }
    Lerp lerp;
    lerp.target = target;
    lerp.name = name;
    lerp.property = property;
    lerp.axis = axis;
    lerp.from = from;
    lerp.to = to;
    lerp.duration = duration;
    rt.lerps.push_back(lerp);
}

void execBlock(Frame& frame, const std::vector<DSLNode>& body);
void spawnTimer(Frame& frame, const DSLNode& node);
[[nodiscard]] bool insideZone(Frame& frame, const std::string& zoneName, entt::entity& zoneOut);

void execNode(Frame& frame, const DSLNode& node, bool arming) {
    auto& registry = frame.scene.registry();
    auto& rt = frame.runtime;
    switch (node.kind) {
    case DSLNodeKind::Assign: {
        if (arming) {
            return;
        }
        if (node.name == "position" && node.property == "reset") {
            entt::entity target = entt::null;
            if (frame.event != nullptr && frame.event->targetEntity != entt::null) {
                target = frame.event->targetEntity;
            } else if (frame.positionEntity != entt::null) {
                target = frame.positionEntity;
            } else if (frame.contextEntity != entt::null) {
                target = frame.contextEntity;
            } else {
                target = frame.self;
            }
            if (target != entt::null && registry.valid(target) && registry.all_of<TransformComponent>(target)) {
                const auto it = frame.context.spawnPositions.find(target);
                if (it != frame.context.spawnPositions.end()) {
                    registry.get<TransformComponent>(target).position = it->second;
                }
            }
            return;
        }
        if (node.name == "behavior") {
            if (const auto text = evaluateText(node.expr, rt)) {
                rt.textVariables["behavior"] = *text;
                applyBehavior(frame, frame.contextEntity != entt::null ? frame.contextEntity : frame.self, *text);
            }
            return;
        }
        if (const auto text = evaluateText(node.expr, rt); text.has_value() && node.expr.kind != DSLExpr::Kind::Variable) {
            rt.textVariables[node.name] = *text;
            return;
        }
        if (node.expr.kind == DSLExpr::Kind::Variable && evaluateText(node.expr, rt).has_value() &&
            rt.variables.find(node.expr.text) == rt.variables.end()) {
            rt.textVariables[node.name] = *evaluateText(node.expr, rt);
            return;
        }
        if (node.hasLerp) {
            startLerp(frame, Lerp::Target::Variable, node.name, "", "", evaluateExpr(node.expr, rt),
                      node.lerpDuration);
            return;
        }
        cancelLerps(rt, Lerp::Target::Variable, node.name, "", "");
        rt.variables[node.name] = clampToBounds(rt, node.name, evaluateExpr(node.expr, rt));
        return;
    }
    case DSLNodeKind::BoundDecl: {
        if (arming) {
            return;
        }
        ScriptSystemContext::VariableBounds& bounds = rt.bounds[node.name];
        const double value = evaluateExpr(node.expr, rt);
        if (node.property == "max") {
            bounds.highest = value;
            bounds.hasHighest = true;
        } else {
            bounds.lowest = value;
            bounds.hasLowest = true;
        }
        // Declaring a bound also re-clamps whatever the variable
        // already holds, so declaration order never leaks a
        // pre-bound overflow into the game.
        if (const auto it = rt.variables.find(node.name); it != rt.variables.end()) {
            it->second = clampToBounds(rt, node.name, it->second);
        }
        return;
    }
    case DSLNodeKind::EffectAssign: {
        if (arming) {
            return;
        }
        if (node.name != "chromatic_aberration") {
            ENGINE_LOG_WARNING("Script: unknown effect property '" << node.name
                                                                    << "' (supported: chromatic_aberration)");
            return;
        }
        if (node.hasLerp) {
            startLerp(frame, Lerp::Target::EffectProperty, node.name, node.property, "", evaluateExpr(node.expr, rt),
                      node.lerpDuration);
            return;
        }
        cancelLerps(rt, Lerp::Target::EffectProperty, node.name, node.property, "");
        frame.context.effects.chromaticAberration = evaluateExpr(node.expr, rt);
        return;
    }
    case DSLNodeKind::HudPrint: {
        if (arming) {
            return;
        }
        ActiveHudPrint print;
        print.text = node.name;
        print.xPercent = node.hudXPercent;
        print.yPercent = node.hudYPercent;
        print.color = glm::vec3{node.hudRed, node.hudGreen, node.hudBlue};
        print.duration = node.hudDuration;
        print.fade = node.hudFade;
        print.source = frame.self;
        print.alpha = print.fade ? 0.0f : 1.0f;
        // Re-printing the same text refreshes it in place.
        auto& prints = frame.context.hudPrints;
        prints.erase(std::remove_if(prints.begin(), prints.end(),
                                    [&](const ActiveHudPrint& other) {
                                        return other.source == print.source && other.text == print.text;
                                    }),
                     prints.end());
        prints.push_back(print);
        return;
    }
    case DSLNodeKind::PartAssign: {
        if (arming) {
            return;
        }
        if (node.hasLerp) {
            startLerp(frame, Lerp::Target::PartProperty, node.name, node.property, node.extra,
                      evaluateExpr(node.expr, rt), node.lerpDuration);
            return;
        }
        cancelLerps(rt, Lerp::Target::PartProperty, node.name, node.property, node.extra);
        applyPartProperty(registry, findByName(registry, node.name), node.property, node.extra,
                          evaluateExpr(node.expr, rt));
        return;
    }
    case DSLNodeKind::PartCall: {
        if (arming) {
            return;
        }
        const entt::entity target = findByName(registry, node.name);
        if (target == entt::null) {
            return;
        }
        if (node.extra == "destroy") {
            frame.destroyQueue.push_back(target);
        } else if (node.extra == "freeze") {
            registry.get_or_emplace<FrozenComponent>(target).frozen = true;
        }
        return;
    }
    case DSLNodeKind::PartBlock: {
        Frame inner = frame;
        inner.contextEntity = findByName(registry, node.name);
        execBlock(inner, node.body);
        return;
    }
    case DSLNodeKind::InhandCall: {
        if (arming) {
            return;
        }
        if (!holdsItemIn(rt, node.name)) {
            rt.heldItems.push_back(node.name);
        }
        return;
    }
    case DSLNodeKind::InhandBlock: {
        if (holdsItemIn(rt, node.name)) {
            execBlock(frame, node.body);
        }
        return;
    }
    case DSLNodeKind::PositionBlock: {
        entt::entity zone = entt::null;
        if (insideZone(frame, node.name, zone)) {
            Frame inner = frame;
            inner.positionEntity = zone;
            execBlock(inner, node.body);
        }
        return;
    }
    case DSLNodeKind::If: {
        if (!node.hasCondition || evaluateExpr(node.expr, rt) != 0.0) {
            execBlock(frame, node.body);
        }
        return;
    }
    case DSLNodeKind::EventBlock: {
        if (node.event == DSLEventKind::Start) {
            // on_start() { ... }: runs once at setup; handlers
            // nested inside latch so they arm from this frame on.
            if (!arming) {
                Frame inner = frame;
                inner.latchSink = &rt.latched;
                execBlock(inner, node.body);
            }
            return;
        }
        if (frame.armed != nullptr) {
            frame.armed->push_back({&node, frame});
        }
        if (frame.latchSink != nullptr) {
            // Event-in-event: latch so the handler survives into
            // later frames (deduped by node + owner).
            bool exists = false;
            for (const auto& latched : *frame.latchSink) {
                if (latched.node == &node && latched.self == frame.self) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                frame.latchSink->push_back({&node, frame.self, frame.contextEntity, frame.positionEntity, frame.group});
            }
        }
        return;
    }
    case DSLNodeKind::Every:
    case DSLNodeKind::For: {
        spawnTimer(frame, node);
        return;
    }
    case DSLNodeKind::SoundCall: {
        if (arming) {
            return;
        }
        if (node.extra == "print") {
            frame.context.log.push_back(node.name);
            ENGINE_LOG_INFO("[script] " << node.name);
            return;
        }
        const entt::entity target = frame.contextEntity != entt::null ? frame.contextEntity : frame.self;
        if (target != entt::null && registry.valid(target)) {
            auto& audio = registry.get_or_emplace<AudioComponent>(target);
            audio.pendingSound = node.name;
            audio.spatial = node.extra == "emit_sound";
            ++audio.playCount;
        }
        frame.context.sounds.push_back(node.name);
        return;
    }
    case DSLNodeKind::Program:
        return;
    }
}

void execBlock(Frame& frame, const std::vector<DSLNode>& body) {
    for (const DSLNode& node : body) {
        execNode(frame, node, false);
    }
}

void spawnTimer(Frame& frame, const DSLNode& node) {
    auto& rt = frame.runtime;
    if (frame.dedupeTimers) {
        for (const Timer& t : rt.timers) {
            if (!t.dead && t.node == &node && t.self == frame.self) {
                return; // already standing in this arming context
            }
        }
    }
    Timer timer;
    timer.node = &node;
    timer.duration = std::max(0.0, evaluateExpr(node.expr, rt));
    timer.nextTick = timer.duration;
    timer.self = frame.self;
    timer.contextEntity = frame.contextEntity;
    timer.positionEntity = frame.positionEntity;
    timer.group = frame.group;
    if (node.kind == DSLNodeKind::For) {
        timer.ownsGroup = ++rt.groupCounter;
    }
    rt.timers.push_back(timer);
    if (node.kind == DSLNodeKind::For) {
        // Running the body once now spawns the window's children
        // (every/for) under this timer's group; they die with it.
        Frame inner = frame;
        inner.group = rt.timers.back().ownsGroup;
        execBlock(inner, node.body);
    }
}

// Zone test shared by the interpreter and the arming walk:
// `subject` (focused entity, else "Player", else the script
// entity) within the zone's collider radius (2.5 m fallback).
[[nodiscard]] bool insideZone(Frame& frame, const std::string& zoneName, entt::entity& zoneOut) {
    auto& registry = frame.scene.registry();
    zoneOut = findByName(registry, zoneName);
    entt::entity subject = frame.context.focusedEntity;
    if (subject == entt::null || !registry.valid(subject)) {
        subject = findByName(registry, "Player");
    }
    if (subject == entt::null) {
        subject = frame.self;
    }
    if (zoneOut == entt::null || subject == entt::null || !registry.valid(zoneOut) || !registry.valid(subject) ||
        !registry.all_of<TransformComponent>(zoneOut) || !registry.all_of<TransformComponent>(subject)) {
        return false;
    }
    float radius = 2.5f;
    if (registry.all_of<ColliderComponent>(zoneOut)) {
        radius = glm::length(registry.get<ColliderComponent>(zoneOut).halfExtents);
    }
    return glm::distance(registry.get<TransformComponent>(zoneOut).position,
                         registry.get<TransformComponent>(subject).position) <= radius;
}

} // namespace

double ScriptInterpreter::evaluate(const DSLExpr& expr, const std::unordered_map<std::string, double>& variables,
                                   const std::unordered_map<std::string, std::string>& textVariables) {
    (void)textVariables;
    switch (expr.kind) {
    case DSLExpr::Kind::Number:
        return expr.number;
    case DSLExpr::Kind::Text:
        return 0.0;
    case DSLExpr::Kind::Variable: {
        const auto it = variables.find(expr.text);
        return it != variables.end() ? it->second : 0.0;
    }
    case DSLExpr::Kind::Unary: {
        const double v = evaluate(expr.args[0], variables, textVariables);
        return expr.op == "-" ? -v : (v == 0.0 ? 1.0 : 0.0);
    }
    case DSLExpr::Kind::Binary: {
        const double l = evaluate(expr.args[0], variables, textVariables);
        const double r = evaluate(expr.args[1], variables, textVariables);
        if (expr.op == "+") return l + r;
        if (expr.op == "-") return l - r;
        if (expr.op == "*") return l * r;
        if (expr.op == "/") return r != 0.0 ? l / r : 0.0;
        if (expr.op == "%") return r != 0.0 ? std::fmod(l, r) : 0.0;
        if (expr.op == "==") return l == r ? 1.0 : 0.0;
        if (expr.op == "!=") return l != r ? 1.0 : 0.0;
        if (expr.op == ">=") return l >= r ? 1.0 : 0.0;
        if (expr.op == "<=") return l <= r ? 1.0 : 0.0;
        if (expr.op == ">") return l > r ? 1.0 : 0.0;
        if (expr.op == "<") return l < r ? 1.0 : 0.0;
        if (expr.op == "&&") return (l != 0.0 && r != 0.0) ? 1.0 : 0.0;
        if (expr.op == "||") return (l != 0.0 || r != 0.0) ? 1.0 : 0.0;
        return 0.0;
    }
    }
    return 0.0;
}

void ScriptSystem::attach(ScriptSystemContext& context, EventBus& bus) {
    // Physical (solid) impacts are on_touch / on_collide events:
    // one CollisionEvent feeds both spellings, each handler kind
    // still matches exactly once.
    bus.subscribe<CollisionEvent>([&context](const CollisionEvent& e) {
        for (const DSLEventKind kind : {DSLEventKind::Touch, DSLEventKind::Collide}) {
            DSLEvent event;
            event.kind = kind;
            event.entityA = e.entityA;
            event.entityB = e.entityB;
            context.eventQueue.push_back(event);
        }
    });
    // Ghost volumes are on_trigger events (entered edges only).
    bus.subscribe<TriggerEvent>([&context](const TriggerEvent& e) {
        if (!e.entered) {
            return;
        }
        DSLEvent event;
        event.kind = DSLEventKind::Trigger;
        event.entityA = e.triggerEntity;
        event.entityB = e.otherEntity;
        event.targetEntity = e.triggerEntity;
        event.actor = e.otherEntity;
        context.eventQueue.push_back(event);
    });
}

void ScriptSystem::postInputEvent(ScriptSystemContext& context, const std::string& key, bool held) {
    DSLEvent event;
    event.kind = held ? DSLEventKind::InputHold : DSLEventKind::InputPress;
    event.target = key;
    context.eventQueue.push_back(event);
}

void ScriptSystem::postInputRelease(ScriptSystemContext& context, const std::string& key) {
    DSLEvent event;
    event.kind = DSLEventKind::InputRelease;
    event.target = key;
    context.eventQueue.push_back(event);
}

void ScriptSystem::postInteract(ScriptSystemContext& context, const std::string& targetName, entt::entity actor) {
    DSLEvent event;
    event.kind = DSLEventKind::Interact;
    event.target = targetName;
    event.actor = actor;
    context.eventQueue.push_back(event);
}

void ScriptSystem::postEvent(ScriptSystemContext& context, DSLEvent event) {
    context.eventQueue.push_back(event);
}

const ScriptSystemContext::Runtime* ScriptSystem::findRuntime(const ScriptSystemContext& context, entt::entity script) {
    const auto it = context.runtimes.find(script);
    return it != context.runtimes.end() ? &it->second : nullptr;
}

std::optional<double> ScriptSystem::variable(ScriptSystemContext& context, entt::entity script, const std::string& name) {
    const auto it = context.runtimes.find(script);
    if (it == context.runtimes.end()) {
        return std::nullopt;
    }
    const auto var = it->second.variables.find(name);
    return var != it->second.variables.end() ? std::optional<double>(var->second) : std::nullopt;
}

std::optional<std::string> ScriptSystem::textVariable(ScriptSystemContext& context, entt::entity script, const std::string& name) {
    const auto it = context.runtimes.find(script);
    if (it == context.runtimes.end()) {
        return std::nullopt;
    }
    const auto var = it->second.textVariables.find(name);
    return var != it->second.textVariables.end() ? std::optional<std::string>(var->second) : std::nullopt;
}

bool ScriptSystem::holdsItem(ScriptSystemContext& context, entt::entity script, const std::string& item) {
    const auto it = context.runtimes.find(script);
    return it != context.runtimes.end() && holdsItemIn(it->second, item);
}

void ScriptSystem::clearRuntime(ScriptSystemContext& context, entt::entity script) {
    context.runtimes.erase(script);
}

void ScriptSystem::update(Scene& scene, ScriptSystemContext& context, double deltaSeconds) {
    auto& registry = scene.registry();
    std::vector<entt::entity> destroyQueue;

    // Record spawn positions (first sight) for position = reset.
    registry.view<TransformComponent>().each([&](entt::entity e, const TransformComponent& transform) {
        if (context.spawnPositions.find(e) == context.spawnPositions.end()) {
            context.spawnPositions[e] = transform.position;
        }
    });

    // Scene-restore migration: Play Stop (GBQ snapshot restore),
    // scene loads and other registry rebuilds invalidate every
    // handle, but the entities come back with the same tags. A
    // runtime whose entity is gone is first offered to a restored
    // script entity carrying its recorded tag -- variables, timers
    // and inventory move across, so Stop -> Play never resets
    // progress (the source-change detector below still applies).
    // Only runtimes with no tag match are pruned.
    {
        std::vector<entt::entity> candidates;
        for (const entt::entity e : registry.view<ScriptComponent>()) {
            if (context.runtimes.find(e) == context.runtimes.end()) {
                candidates.push_back(e);
            }
        }
        for (auto it = context.runtimes.begin(); it != context.runtimes.end();) {
            const entt::entity old = it->first;
            if (registry.valid(old) && registry.all_of<ScriptComponent>(old)) {
                ++it;
                continue;
            }
            entt::entity target = entt::null;
            if (!it->second.identityTag.empty()) {
                // Prefer a tag match whose source also matches
                // (duplicate tags cannot steal each other's state),
                // then fall back to the first plain tag match.
                for (int pass = 0; pass < 2 && target == entt::null; ++pass) {
                    for (size_t i = 0; i < candidates.size(); ++i) {
                        const TagComponent* tag = registry.try_get<TagComponent>(candidates[i]);
                        if (tag == nullptr || tag->tag != it->second.identityTag) {
                            continue;
                        }
                        if (pass == 0) {
                            const ScriptComponent& candidate = registry.get<ScriptComponent>(candidates[i]);
                            if (candidate.source != it->second.source) {
                                continue;
                            }
                        }
                        target = candidates[i];
                        candidates.erase(candidates.begin() + static_cast<std::ptrdiff_t>(i));
                        break;
                    }
                }
            }
            if (target == entt::null) {
                context.fileStamps.erase(old);
                context.spawnPositions.erase(old);
                it = context.runtimes.erase(it);
                continue;
            }
            Runtime moved = std::move(it->second);
            it = context.runtimes.erase(it);
            for (Timer& timer : moved.timers) {
                if (timer.self == old) {
                    timer.self = target;
                }
            }
            for (auto& latched : moved.latched) {
                if (latched.self == old) {
                    latched.self = target;
                }
            }
            if (const auto fit = context.fileStamps.find(old); fit != context.fileStamps.end()) {
                context.fileStamps[target] = fit->second;
                context.fileStamps.erase(fit);
            }
            context.spawnPositions.erase(old); // the restored entity re-records its own spawn above
            context.runtimes.emplace(target, std::move(moved));
        }
    }

    // Compile + set up scripts.
    std::vector<entt::entity> scripts;
    registry.view<ScriptComponent>().each([&](entt::entity e, ScriptComponent&) { scripts.push_back(e); });

    std::vector<std::pair<const DSLNode*, Frame>> armed;
    for (const entt::entity e : scripts) {
        if (!registry.valid(e)) {
            continue;
        }
        auto& component = registry.get<ScriptComponent>(e);
        Runtime& rt = context.runtimes[e];
        if (const TagComponent* tag = registry.try_get<TagComponent>(e); tag != nullptr && !tag->tag.empty()) {
            rt.identityTag = tag->tag; // migration key for scene restores
        }
        // Entity-linked script files: PreferFileSource loads the
        // file before first compile; AutoReload keeps watching it
        // and streams edits into `source` (the recompile detector
        // below does the rest). Missing files are ignored quietly
        // so inline-only scripts never spam the log.
        if (!component.filePath.empty() &&
            (hasFlag(component.compileFlags, ScriptCompileFlags::AutoReload) || !rt.setupDone)) {
            std::error_code ec;
            const auto stamp = std::filesystem::last_write_time(component.filePath, ec);
            if (!ec) {
                const int64_t ticks = static_cast<int64_t>(stamp.time_since_epoch().count());
                int64_t& seen = context.fileStamps[e];
                if (seen != ticks) {
                    seen = ticks;
                    std::ifstream file(component.filePath, std::ios::binary);
                    if (file) {
                        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
                        if (content != component.source) {
                            component.source = std::move(content);
                        }
                    }
                }
            }
        }
        if (component.source != rt.source) {
            DSLParseResult parsed = parseBackroomsScript(component.source);
            rt.source = component.source;
            rt.errors = parsed.errors;
            rt.program = std::move(parsed.program);
            rt.timers.clear();
            rt.setupDone = false;
            rt.deadReported = false;
            rt.variables.clear();
            rt.textVariables.clear();
            rt.bounds.clear();
            rt.lerps.clear();
            rt.heldItems.clear();
            if (!rt.errors.empty()) {
                for (const std::string& error : rt.errors) {
                    ENGINE_LOG_ERROR("Script(" << nameOf(registry, e) << "): " << error);
                }
                rt.deadReported = true;
            }
        }
        if (!component.enabled || !rt.errors.empty()) {
            continue;
        }
        Frame frame{scene, context, rt, e, entt::null, entt::null, nullptr, 0, destroyQueue, nullptr};
        if (!rt.setupDone) {
            // armed == nullptr here: event blocks arm in the arming
            // walk below, never during setup (no double-firing).
            execBlock(frame, rt.program.statements);
            rt.setupDone = true;
            component.started = true;
        } else if (!component.started) {
            component.started = true; // persisted runtime, fresh component (snapshot restore)
        }
        frame.armed = &armed;
        frame.dedupeTimers = true;
        // Arming pass: descend guards, register this frame's
        // handlers and make sure standing timers exist. Guard
        // blocks run their non-event children in arming mode too
        // (assignments inside guards are one-shot at setup; the
        // arming walk only arms/dedupes — execNode(arming=true)
        // skips mutations).
        struct ArmWalk {
            static void walk(Frame& f, const std::vector<DSLNode>& body) {
                for (const DSLNode& node : body) {
                    switch (node.kind) {
                    case DSLNodeKind::EventBlock:
                        f.armed->push_back({&node, f});
                        break;
                    case DSLNodeKind::Every:
                    case DSLNodeKind::For: {
                        bool exists = false;
                        for (const Timer& t : f.runtime.timers) {
                            if (!t.dead && t.node == &node && t.self == f.self) {
                                exists = true;
                                break;
                            }
                        }
                        if (node.kind == DSLNodeKind::For) {
                            // A finished top-level window stays
                            // finished: for() is one-shot unless an
                            // event re-triggers it.
                            for (const DSLNode* done : f.runtime.finishedFors) {
                                if (done == &node) {
                                    exists = true;
                                    break;
                                }
                            }
                        }
                        if (!exists) {
                            spawnTimer(f, node);
                        }
                        break;
                    }
                    case DSLNodeKind::If:
                        if (!node.hasCondition || evaluateExpr(node.expr, f.runtime) != 0.0) {
                            walk(f, node.body);
                        }
                        break;
                    case DSLNodeKind::InhandBlock:
                        if (holdsItemIn(f.runtime, node.name)) {
                            walk(f, node.body);
                        }
                        break;
                    case DSLNodeKind::PartBlock: {
                        Frame inner = f;
                        inner.contextEntity = findByName(f.scene.registry(), node.name);
                        walk(inner, node.body);
                        break;
                    }
                    case DSLNodeKind::PositionBlock: {
                        // Spatial blocks are live: while the
                        // subject is inside, the body runs every
                        // frame (nested events arm, timers dedupe).
                        entt::entity zone = entt::null;
                        if (insideZone(f, node.name, zone)) {
                            Frame inner = f;
                            inner.positionEntity = zone;
                            execBlock(inner, node.body);
                        }
                        break;
                    }
                    default:
                        break;
                    }
                }
            }
        };
        ArmWalk::walk(frame, rt.program.statements);
        // Seed handlers latched by earlier event bodies.
        for (const auto& latched : rt.latched) {
            armed.push_back({latched.node, Frame{scene, context, rt, latched.self, latched.contextEntity,
                                                  latched.positionEntity, nullptr, latched.group, destroyQueue, &armed}});
        }
    }

    // Dispatch queued events to armed handlers.
    std::vector<DSLEvent> events(context.eventQueue.begin(), context.eventQueue.end());
    context.eventQueue.clear();
    for (DSLEvent& event : events) {
        // Resolve physics handle pairs into names.
        std::string nameA = nameOf(registry, event.entityA);
        std::string nameB = nameOf(registry, event.entityB);
        for (size_t i = 0; i < armed.size(); ++i) {
            const DSLNode* node = armed[i].first;
            Frame& frame = armed[i].second;
            if (node->event != event.kind) {
                continue;
            }
            bool match = false;
            entt::entity resolvedTarget = event.targetEntity;
            if (event.kind == DSLEventKind::InputPress || event.kind == DSLEventKind::InputHold ||
                event.kind == DSLEventKind::InputRelease) {
                match = node->name == event.target;
            } else if (event.kind == DSLEventKind::Interact) {
                match = node->name == event.target;
                if (match && resolvedTarget == entt::null) {
                    resolvedTarget = findByName(registry, node->name);
                }
            } else {
                if (!nameA.empty() && node->name == nameA) {
                    match = true;
                    resolvedTarget = event.entityA;
                } else if (!nameB.empty() && node->name == nameB) {
                    match = true;
                    resolvedTarget = event.entityB;
                }
            }
            if (!match) {
                continue;
            }
            DSLEvent fired = event;
            fired.targetEntity = resolvedTarget;
            if (fired.actor == entt::null) {
                fired.actor = resolvedTarget == event.entityA ? event.entityB : event.entityA;
            }
            Frame exec = frame;
            exec.event = &fired;
            exec.latchSink = &exec.runtime.latched;
            if (exec.positionEntity == entt::null &&
                (event.kind == DSLEventKind::Touch || event.kind == DSLEventKind::Trigger ||
                 event.kind == DSLEventKind::Collide)) {
                exec.positionEntity = resolvedTarget;
            }
            execBlock(exec, node->body);
        }
    }

    // Tick timers (time manager): For windows expire and take
    // their group children with them; Every timers fire in place.
    // Index-based throughout: firing a timer can spawn new ones.
    for (auto& [scriptEntity, rt] : context.runtimes) {
        (void)scriptEntity;
        const size_t snapshot = rt.timers.size();
        for (size_t i = 0; i < snapshot; ++i) {
            if (rt.timers[i].dead || rt.timers[i].node == nullptr) {
                continue;
            }
            rt.timers[i].elapsed += deltaSeconds;
            if (rt.timers[i].node->kind == DSLNodeKind::For) {
                if (rt.timers[i].elapsed >= rt.timers[i].duration) {
                    if (std::find(rt.finishedFors.begin(), rt.finishedFors.end(), rt.timers[i].node) == rt.finishedFors.end()) {
                        rt.finishedFors.push_back(rt.timers[i].node);
                    }
                    // Expire the window and (transitively) every
                    // timer the window owned.
                    std::vector<uint32_t> deadGroups{rt.timers[i].ownsGroup};
                    rt.timers[i].dead = true;
                    for (size_t g = 0; g < deadGroups.size(); ++g) {
                        for (Timer& other : rt.timers) {
                            if (!other.dead && other.group == deadGroups[g]) {
                                other.dead = true;
                                if (other.ownsGroup != 0) {
                                    deadGroups.push_back(other.ownsGroup);
                                }
                            }
                        }
                    }
                }
                continue;
            }
            while (!rt.timers[i].dead && rt.timers[i].duration > 0.0 && rt.timers[i].elapsed >= rt.timers[i].nextTick) {
                rt.timers[i].nextTick += rt.timers[i].duration;
                const DSLNode* node = rt.timers[i].node;
                Frame frame{scene, context, rt, rt.timers[i].self, rt.timers[i].contextEntity,
                            rt.timers[i].positionEntity, nullptr, rt.timers[i].group, destroyQueue, &armed};
                execBlock(frame, node->body);
            }
        }
        rt.timers.erase(std::remove_if(rt.timers.begin(), rt.timers.end(), [](const Timer& t) { return t.dead; }),
                        rt.timers.end());
    }

    // Tick lerps (the `over Xs` interpolation tasks): every frame
    // advances each task by dt and writes the interpolated value;
    // reaching the target completes and removes the task.
    for (auto& [scriptEntity, rt] : context.runtimes) {
        (void)scriptEntity;
        for (Lerp& lerp : rt.lerps) {
            lerp.elapsed += deltaSeconds;
            const double t = lerp.duration > 0.0 ? std::min(1.0, lerp.elapsed / lerp.duration) : 1.0;
            applyLerpTarget(scene, context, rt, lerp, lerp.from + (lerp.to - lerp.from) * t);
            lerp.dead = t >= 1.0;
        }
        rt.lerps.erase(std::remove_if(rt.lerps.begin(), rt.lerps.end(),
                                      [](const Lerp& lerp) { return lerp.dead; }),
                       rt.lerps.end());
    }

    // Tick HUD prints: age them, compute the fade alpha (equal
    // fade-in/out windows capped at half the lifetime), expire.
    for (ActiveHudPrint& print : context.hudPrints) {
        print.elapsed += deltaSeconds;
        if (!print.fade) {
            print.alpha = 1.0f;
        } else {
            const double window = std::min(0.5, print.duration * 0.5);
            const double in = window > 0.0 ? print.elapsed / window : 1.0;
            const double out = window > 0.0 ? (print.duration - print.elapsed) / window : 1.0;
            print.alpha = static_cast<float>(std::clamp(std::min(in, out), 0.0, 1.0));
        }
    }
    context.hudPrints.erase(std::remove_if(context.hudPrints.begin(), context.hudPrints.end(),
                                           [](const ActiveHudPrint& print) {
                                               return print.duration <= 0.0 || print.elapsed >= print.duration;
                                           }),
                            context.hudPrints.end());

    // Deferred destroys.
    for (const entt::entity target : destroyQueue) {
        if (registry.valid(target)) {
            context.runtimes.erase(target);
            context.spawnPositions.erase(target);
            scene.destroyEntity(Entity{target, &scene});
        }
    }
    destroyQueue.clear();
}

void ScriptSystem::update(Scene& scene, ScriptSystemContext& context, const Input& input, double deltaSeconds) {
    // Translate the engine's real input state into DSL input
    // events: press edges feed on_input, levels feed on_hold.
    struct KeyName {
        const char* name;
        Key key;
        bool isKey;
        MouseButton button;
    };
    static constexpr KeyName kKeys[] = {
        {"LMB", Key::Unknown, false, MouseButton::Left},
        {"RMB", Key::Unknown, false, MouseButton::Right},
        {"MMB", Key::Unknown, false, MouseButton::Middle},
        {"LeftShift", Key::LeftShift, true, MouseButton::Left},
        {"RightShift", Key::RightShift, true, MouseButton::Left},
        {"LeftControl", Key::LeftControl, true, MouseButton::Left},
        {"Space", Key::Space, true, MouseButton::Left},
        {"Enter", Key::Enter, true, MouseButton::Left},
        {"Escape", Key::Escape, true, MouseButton::Left},
        {"Tab", Key::Tab, true, MouseButton::Left},
        {"E", Key::E, true, MouseButton::Left},
        {"F", Key::F, true, MouseButton::Left},
        {"Q", Key::Q, true, MouseButton::Left},
        {"R", Key::R, true, MouseButton::Left},
    };
    for (const KeyName& entry : kKeys) {
        const bool pressed = entry.isKey ? input.wasKeyPressed(entry.key) : input.wasMouseButtonPressed(entry.button);
        const bool released = entry.isKey ? input.wasKeyReleased(entry.key) : input.wasMouseButtonReleased(entry.button);
        const bool down = entry.isKey ? input.isKeyDown(entry.key) : input.isMouseButtonDown(entry.button);
        if (pressed) {
            postInputEvent(context, entry.name, false);
        }
        if (released) {
            postInputRelease(context, entry.name);
        }
        if (down) {
            postInputEvent(context, entry.name, true);
        }
    }
    update(scene, context, deltaSeconds);
}

} // namespace engine
