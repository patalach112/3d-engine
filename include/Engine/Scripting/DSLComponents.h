#pragma once

#include <entt/entity/entity.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <string>

namespace engine {

// ---------------------------------------------------------------------------
// Scripting components (hybrid DSL + native architecture)
//
// The Backrooms DSL attaches to an entity through ScriptComponent
// (the spec's "ScriptComponent"): it carries the human-readable
// script source; the parsed AST, variable map and timer instances
// live in the ScriptRuntime the ScriptSystem owns per entity --
// the same split NativeScriptComponent uses (name in the
// component, instance in the system), so components stay plain
// serializable data.
// ---------------------------------------------------------------------------

// Compilation/linkage flags for an entity-linked script (bitmask,
// serialized with the component). AutoReload watches filePath on
// disk: when the file changes, ScriptSystem reloads it into
// `source` and recompiles. PreferFileSource makes the file the
// authority at setup time (the editor's Save & Compile writes both
// the component buffer and the file, so the two never drift in the
// other direction).
enum class ScriptCompileFlags : uint32_t {
    None = 0,
    AutoReload = 1u << 0,
    PreferFileSource = 1u << 1,
};
[[nodiscard]] constexpr ScriptCompileFlags operator|(ScriptCompileFlags a, ScriptCompileFlags b) {
    return static_cast<ScriptCompileFlags>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
}
[[nodiscard]] constexpr bool hasFlag(ScriptCompileFlags flags, ScriptCompileFlags flag) {
    return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) != 0;
}

struct ScriptComponent {
    std::string filePath; // linked .dsla file ("" = inline-only script)
    std::string source;   // DSL program text (from start() .. script(end))
    ScriptCompileFlags compileFlags = ScriptCompileFlags::None;
    bool enabled = true;  // disabled components are skipped by ScriptSystem
    bool started = false; // system-managed: setup statements have run
};

// High-level AI tag (the DSL's `behavior = stalker`). Written by
// native code or by a DSL `behavior = ...` assignment; consumed by
// the native StalkerAISystem, which supplies the raycast/steering
// obstacle avoidance so scripts never write raw steering math.
enum class AIBehavior : uint8_t { None = 0, Stalker = 1 };

struct AIBehaviorComponent {
    AIBehavior behavior = AIBehavior::None;
    entt::entity target = entt::null; // null = track the entity tagged "Player"
    float moveSpeed = 2.2f;           // metres per second
    float stoppingDistance = 1.1f;    // halts this far from the target
    float avoidanceRadius = 2.6f;     // obstacle sense radius
    glm::vec3 steering{0.0f};         // last computed velocity (debug/AI viz)
    bool active = true;
};

// Spatial audio sink (the DSL's play_sound / emit_sound): a sound
// request lands here (pendingSound) for an audio backend to
// consume; playCount lets tests/tools verify requests landed.
struct AudioComponent {
    std::string pendingSound; // most recent requested sound id ("" = none)
    float volume = 1.0f;
    float range = 14.0f; // attenuation range in metres when spatial
    bool spatial = true;
    uint32_t playCount = 0;
};

// Frozen by a DSL `part("x").freeze()`: gameplay systems honour
// this tag (the stalker does; physics bodies keep their material
// state -- freeze is a gameplay gate, not a solver state).
struct FrozenComponent {
    bool frozen = true;
};

} // namespace engine
