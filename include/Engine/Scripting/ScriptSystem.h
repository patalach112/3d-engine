#pragma once

#include "Core/Export.h"
#include "Scripting/DSLParser.h"

#include <entt/entity/entity.hpp>
#include <glm/vec3.hpp>

#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

class EventBus;
class Input;
class Scene;

// ---------------------------------------------------------------------------
// ScriptSystem — the hybrid runtime for Backrooms DSL scripts.
//
// Static-system shape like the rest of the engine: call
// update(scene, context, deltaSeconds) once per frame. All mutable
// runtime state lives in ONE registry-level context
// (ScriptSystemContext) so scene saves can drop the component and
// the system still works; keep the context beside the scene.
//
// Frame flow (the spec's interpreter pipeline):
//   1. physics EventBus events accumulated by the bus subscription
//      (CollisionEvent -> on_collide, TriggerEvent -> on_touch,
//      both matched by TagComponent name) wait in the event queue;
//   2. input edges (on_input / on_hold) are latched from the host
//      loop into the input events table (postInputEvent);
//   3. for every ScriptComponent entity: run queued event handlers,
//      then tick the time manager (every/for timers, whose blocks
//      can themselves open new timers).
//
// Events carry an other-entity (the player usually). Handlers run
// with an evaluation context: `this` (the script's own entity),
// the current guard entity (part("x") { } / on_interact target)
// and a current position entity (position("zone") / on_touch self).
// Variable scope is per script entity; `inhand` items accumulate
// in a per-script inventory and are global to that script.
//
// Streaming the events from the bus: call attach(events) once;
// clearQueuedEvents() is provided for tools/tests.
// ---------------------------------------------------------------------------

struct DSLEvent {
    DSLEventKind kind = DSLEventKind::Interact;
    std::string target;               // part/zone/key name the event matched
    entt::entity targetEntity = entt::null; // resolved counterpart (the zone / collided mesh)
    entt::entity actor = entt::null;        // the entity that caused it (player)
    // Physics-sourced events carry both raw handles; names are
    // resolved against TagComponents at dispatch time (the bus
    // callback itself has no scene access).
    entt::entity entityA = entt::null;
    entt::entity entityB = entt::null;
};

class ENGINE_SCENE_API ScriptInterpreter {
public:
    // Evaluates `expr` against the variable map. Unset variables and
    // unresolved bare identifiers evaluate as their own text tag
    // (behaviour = stalker); `every`/`for` durations as numbers.
    [[nodiscard]] static double evaluate(const DSLExpr& expr, const std::unordered_map<std::string, double>& variables,
                                         const std::unordered_map<std::string, std::string>& textVariables);
};

struct ScriptSystemContext {
    // Runtime per script entity: parsed program (or parse errors),
    // variable map, text map (behavior = stalker), held items and
    // the active temporal timers. Populated lazily on first update.
    struct Timer {
        const DSLNode* node = nullptr; // Every or For node (owned by the parsed program)
        double elapsed = 0.0;
        double duration = 0.0;  // seconds; For uses it, Every fires at it repeatedly
        double nextTick = 0.0;  // Every only
        entt::entity self = entt::null;
        entt::entity contextEntity = entt::null;
        entt::entity positionEntity = entt::null;
        uint32_t group = 0;     // owning For window (0 = standalone)
        uint32_t ownsGroup = 0; // For only: group id of its window children
        bool dead = false;
    };
    // An event handler armed from inside another handler's body
    // (on_touch("safe_room") { on_collision(part("stalker")) {} }):
    // it latches — stays armed in later frames until recompile.
    struct LatchedHandler {
        const DSLNode* node = nullptr;
        entt::entity self = entt::null;
        entt::entity contextEntity = entt::null;
        entt::entity positionEntity = entt::null;
        uint32_t group = 0;
    };
    struct Runtime {
        DSLProgram program;
        std::vector<std::string> errors;
        std::unordered_map<std::string, double> variables;
        std::unordered_map<std::string, std::string> textVariables;
        std::vector<std::string> heldItems;
        std::vector<Timer> timers;
        std::vector<LatchedHandler> latched;
        std::string source; // last source text compiled (recompile detector)
        bool setupDone = false;
        bool deadReported = false;
        uint32_t groupCounter = 0; // For-window group id allocator
        std::vector<const DSLNode*> finishedFors; // top-level windows never restart
    };

    std::unordered_map<entt::entity, Runtime> runtimes;
    std::unordered_map<entt::entity, glm::vec3> spawnPositions; // first-seen transforms (position = reset)
    std::deque<DSLEvent> eventQueue;
    std::vector<std::string> log;   // print()/sound echoes for tests/tools
    std::vector<std::string> sounds; // play_sound()/emit_sound() ids, in order
    entt::entity focusedEntity = entt::null;
};

class ENGINE_SCENE_API ScriptSystem {
public:
    // Attaches the runtime to the scene's EventBus: collision and
    // trigger events become DSL touch/collide events automatically.
    static void attach(ScriptSystemContext& context, EventBus& bus);

    // Latches an input event for the current frame
    // ("LMB", "LeftShift", ...): kind InputPress consumes on
    // on_input, InputHold stays level-true while held (re-latch
    // each frame while the key is down).
    static void postInputEvent(ScriptSystemContext& context, const std::string& key, bool held);
    static void postInteract(ScriptSystemContext& context, const std::string& targetName, entt::entity actor);
    static void postEvent(ScriptSystemContext& context, DSLEvent event);

    static void update(Scene& scene, ScriptSystemContext& context, double deltaSeconds);
    static void update(Scene& scene, ScriptSystemContext& context, const Input& input, double deltaSeconds);

    [[nodiscard]] static std::optional<double> variable(ScriptSystemContext& context, entt::entity script, const std::string& name);
    [[nodiscard]] static std::optional<std::string> textVariable(ScriptSystemContext& context, entt::entity script, const std::string& name);
    [[nodiscard]] static bool holdsItem(ScriptSystemContext& context, entt::entity script, const std::string& item);
    static void clearRuntime(ScriptSystemContext& context, entt::entity script);
};

} // namespace engine
