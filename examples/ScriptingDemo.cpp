// ---------------------------------------------------------------------------
// scripting-demo — the hybrid Backrooms scripting showcase.
//
// Builds the Scenario 18 scene headlessly (no window, no Vulkan):
// a player, an Almond Water pickup, a safe room with lights, and a
// stalker. One entity carries the Scenario 18 DSL script
// (ScriptComponent); the SAME frame loop also runs the native
// StalkerAISystem — the DSL assigns `behavior = stalker`, native
// code does the tracking/steering/avoidance. A scripted timeline
// (pickup -> LMB use -> enter safe room -> stalker collision)
// prints a trace and self-checks every beat, exiting nonzero on
// any failure. Run: scripting-demo
// ---------------------------------------------------------------------------

#include "Physics/PhysicsComponents.h"
#include "Scene/Components.h"
#include "Scene/Scene.h"
#include "Scripting/DSLComponents.h"
#include "Scripting/DSLParser.h"
#include "Scripting/Scenario18.h"
#include "Scripting/ScriptSystem.h"
#include "Scripting/StalkerAISystem.h"

#include <cmath>
#include <iostream>

namespace {

int g_failures = 0;

void expect(bool condition, const std::string& what) {
    std::cout << (condition ? "  [ok] " : "  [FAIL] ") << what << '\n';
    if (!condition) {
        ++g_failures;
    }
}

} // namespace

int main() {
    using namespace engine;

    std::cout << "Scenario 18 - hybrid DSL + native demo\n";
    const DSLParseResult parsed = parseBackroomsScript(kScenario18Script);
    expect(parsed.ok(), "Scenario 18 script parses cleanly");
    if (!parsed.ok()) {
        for (const std::string& error : parsed.errors) {
            std::cout << "    " << error << '\n';
        }
        return 1;
    }

    Scene scene;
    ScriptSystemContext scripts;

    auto player = scene.createEntity("Player");
    player.addComponent<TransformComponent>().position = glm::vec3{0.0f, 0.0f, 6.0f};
    player.addComponent<ColliderComponent>(ColliderComponent::capsule(0.3f, 0.45f));

    auto water = scene.createEntity("almond_water");
    water.addComponent<TransformComponent>().position = glm::vec3{1.0f, 0.0f, 5.0f};

    auto safeRoom = scene.createEntity("safe_room");
    safeRoom.addComponent<TransformComponent>().position = glm::vec3{0.0f, 0.0f, -6.0f};
    auto& zoneCollider = safeRoom.addComponent<ColliderComponent>(ColliderComponent::box(glm::vec3{3.0f, 2.0f, 3.0f}));
    zoneCollider.isTrigger = true;

    auto lights = scene.createEntity("safe_lights");
    lights.addComponent<TransformComponent>().position = glm::vec3{0.0f, 2.5f, -6.0f};
    lights.addComponent<PointLightComponent>().intensity = 0.0f;

    auto stalker = scene.createEntity("stalker");
    stalker.addComponent<TransformComponent>().position = glm::vec3{8.0f, 0.0f, 8.0f};
    stalker.addComponent<ColliderComponent>(ColliderComponent::capsule(0.35f, 0.6f));

    auto scenario = scene.createEntity("Scenario18");
    scenario.addComponent<ScriptComponent>().source = kScenario18Script;

    auto& registry = scene.registry();
    const entt::entity scenarioHandle = scenario.handle();
    const glm::vec3 stalkerSpawn = registry.get<TransformComponent>(stalker.handle()).position;

    auto frame = [&](double dt) {
        ScriptSystem::update(scene, scripts, dt);
        StalkerAISystem::update(scene, static_cast<float>(dt));
    };
    auto var = [&](const char* name) { return ScriptSystem::variable(scripts, scenarioHandle, name).value_or(-1.0); };

    // -- t=0: setup -------------------------------------------------
    frame(0.1);
    expect(var("sanity") == 100.0 && var("health") == 100.0, "setup: sanity = health = 100");
    expect(registry.all_of<AIBehaviorComponent>(stalker.handle()) &&
               registry.get<AIBehaviorComponent>(stalker.handle()).behavior == AIBehavior::Stalker,
           "setup: DSL assigned behavior = stalker (native AI component)");

    // -- pickup -------------------------------------------------------
    ScriptSystem::postInteract(scripts, "almond_water", player.handle());
    frame(0.1);
    expect(ScriptSystem::holdsItem(scripts, scenarioHandle, "almond_water"), "interact: almond_water in hand");
    expect(!registry.valid(water.handle()), "interact: world almond_water destroyed");

    // -- use it (guarded LMB) -----------------------------------------
    const double healthBefore = var("health");
    const double sanityBefore = var("sanity");
    ScriptSystem::postInputEvent(scripts, "LMB", false);
    frame(0.1); // click lands, regen window opens
    for (int i = 0; i < 35; ++i) {
        frame(0.1); // 3.5 simulated seconds
    }
    expect(var("health") == healthBefore + 6.0, "almond water: +2 HP/s for 3 ticks = +6 HP");
    expect(var("sanity") == sanityBefore + 150.0, "almond water: +50 sanity/s for 3 ticks = +150");

    // Let the Almond Water window fully expire first so its heal
    // ticks cannot pollute the safe-room accounting below.
    for (int i = 0; i < 70; ++i) {
        frame(0.1);
    }

    // -- safe room ------------------------------------------------------
    ScriptSystem::postEvent(scripts, DSLEvent{DSLEventKind::Trigger, "safe_room", safeRoom.handle(), player.handle(),
                                              safeRoom.handle(), player.handle()});
    frame(0.1);
    const double healthRoom = var("health");
    expect(std::abs(registry.get<PointLightComponent>(lights.handle()).intensity - 1.0f) < 1e-6f,
           "safe room: safe_lights intensity = 1.0");
    for (int i = 0; i < 65; ++i) {
        frame(0.1); // 6.5 simulated seconds: ticks at 3s and 6s
    }
    expect(var("health") == healthRoom + 4.0, "safe room: +2 HP every 3s, two ticks in 6.5s = +4 HP");

    // -- the stalker hunts natively, then gets reset ----------------------
    const glm::vec3 hunted = registry.get<TransformComponent>(stalker.handle()).position;
    expect(glm::distance(hunted, stalkerSpawn) > 0.5f, "native stalker AI tracked the player across the scene");
    ScriptSystem::postEvent(scripts, DSLEvent{DSLEventKind::Collide, "stalker", stalker.handle(), safeRoom.handle(),
                                              stalker.handle(), safeRoom.handle()});
    frame(0.1);
    const glm::vec3 resetPos = registry.get<TransformComponent>(stalker.handle()).position;
    expect(glm::distance(resetPos, stalkerSpawn) < 0.75f, "stalker touched the safe room: position = reset (back at spawn)");

    std::cout << "script log lines: " << scripts.log.size() << ", sounds: " << scripts.sounds.size() << '\n';
    std::cout << (g_failures == 0 ? "scripting-demo: ALL CHECKS PASSED\n" : "scripting-demo: FAILURES\n");
    return g_failures == 0 ? 0 : 1;
}
