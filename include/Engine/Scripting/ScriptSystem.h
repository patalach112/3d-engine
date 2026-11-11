#pragma once

#include "Core/Export.h"
#include "Scripting/DSLParser.h"

#include <entt/entity/entity.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

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
    // Declared value boundaries: health(max) = 100 /
    // health(lowest) = 0. Every write to a bounded variable
    // (instant or lerped) is clamped into [lowest, highest].
    struct VariableBounds {
        double lowest = 0.0;
        double highest = 0.0;
        bool hasLowest = false;
        bool hasHighest = false;
    };
    // A non-blocking linear interpolation spawned by
    // `target = value over Xs`: ticked every frame with dt until
    // the target is reached, then removed. A fresh write to the
    // same target replaces (or, without `over`, cancels) it.
    struct Lerp {
        enum class Target : uint8_t { Variable, PartProperty, EffectProperty };
        Target target = Target::Variable;
        std::string name;     // variable name / part target name / effect property
        std::string property; // part/effect property ("" for variables)
        std::string axis;     // scale axis for part targets
        double from = 0.0;
        double to = 0.0;
        double elapsed = 0.0;
        double duration = 0.0;
        bool dead = false;
    };
    struct Runtime {
        DSLProgram program;
        std::vector<std::string> errors;
        std::unordered_map<std::string, double> variables;
        std::unordered_map<std::string, std::string> textVariables;
        std::unordered_map<std::string, VariableBounds> bounds;
        std::vector<Lerp> lerps;
        std::vector<std::string> heldItems;
        std::vector<Timer> timers;
        std::vector<LatchedHandler> latched;
        std::string identityTag; // TagComponent name (scene-restore migration key)
        std::string source; // last source text compiled (recompile detector)
        bool setupDone = false;
        bool deadReported = false;
        uint32_t groupCounter = 0; // For-window group id allocator
        std::vector<const DSLNode*> finishedFors; // top-level windows never restart
    };

    // One live hudprint("...") instance: percentage-anchored
    // screen text with a lifetime; `alpha` is recomputed every
    // tick (fade in/out) for the host's HUD pass to draw with.
    struct ActiveHudPrint {
        std::string text;
        float xPercent = 50.0f;
        float yPercent = 50.0f;
        glm::vec3 color{1.0f};
        float alpha = 1.0f;
        double duration = 3.0;
        double elapsed = 0.0;
        bool fade = false;
        entt::entity source = entt::null; // script entity that printed it
    };
    // Global post-processing state scripts drive through
    // `effect.<property> = value` (the host mirrors it into the
    // renderer's PostSettings every frame).
    struct PostEffects {
        double chromaticAberration = 0.0;
    };

    std::unordered_map<entt::entity, Runtime> runtimes;
    std::vector<ActiveHudPrint> hudPrints;
    PostEffects effects;
    std::unordered_map<entt::entity, int64_t> fileStamps; // .dsla mtimes seen (AutoReload)
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
    // Release edge for on_input_release("ActionName") handlers.
    static void postInputRelease(ScriptSystemContext& context, const std::string& key);
    static void postInteract(ScriptSystemContext& context, const std::string& targetName, entt::entity actor);
    static void postEvent(ScriptSystemContext& context, DSLEvent event);

    static void update(Scene& scene, ScriptSystemContext& context, double deltaSeconds);
    static void update(Scene& scene, ScriptSystemContext& context, const Input& input, double deltaSeconds);

    // Live runtime state for one script entity (null until its
    // first update): the inspector reads compile errors / held
    // items / timer counts from here.
    [[nodiscard]] static const ScriptSystemContext::Runtime* findRuntime(const ScriptSystemContext& context, entt::entity script);
    [[nodiscard]] static std::optional<double> variable(ScriptSystemContext& context, entt::entity script, const std::string& name);
    [[nodiscard]] static std::optional<std::string> textVariable(ScriptSystemContext& context, entt::entity script, const std::string& name);
    [[nodiscard]] static bool holdsItem(ScriptSystemContext& context, entt::entity script, const std::string& item);
    static void clearRuntime(ScriptSystemContext& context, entt::entity script);
};

} // namespace engine
