#pragma once
#include "Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace engine {

class Scene;

// Editor mode (Phase 31): the shell either authors the scene
// (Edit — simulation frozen, gizmos/inspector live) or runs it
// (Play — physics, game modules and native scripts execute).
enum class EditorMode : uint8_t { Edit, Play };

// ---------------------------------------------------------------------------
// PlaySession (Phase 31): the Edit/Play state machine plus the
// pre-play scene snapshot.
//
// enterPlay() captures the whole scene as in-memory GBQ bytes
// (Scene::saveToGbqBytes — component states, hierarchy, rigid
// bodies, joint links, script bindings) BEFORE any runtime code
// touches it. While playing, the runtime may move, spawn and
// destroy entities freely. exitPlay() restores the snapshot
// wholesale through the validating loader, returning the editor
// to the exact pre-play state; restored entities are new EnTT
// handles, so callers must drop cached entity references (the
// Application clears its editor selection on both transitions).
//
// Both transitions are guarded: entering twice or exiting while
// editing is a no-op returning false. If a restore ever failed,
// the session stays in Play (the live scene is untouched by the
// failed load) rather than stranding the editor in Edit mode
// over a mutated scene.
// ---------------------------------------------------------------------------
class ENGINE_SCENE_API PlaySession {
public:
    // Captures the snapshot and switches to Play. False (no-op)
    // when already playing.
    bool enterPlay(Scene& scene);
    // Restores the snapshot and switches back to Edit. False
    // (no-op) when not playing, or when the restore failed (the
    // session then stays in Play).
    bool exitPlay(Scene& scene);

    [[nodiscard]] EditorMode mode() const { return m_mode; }
    [[nodiscard]] bool isPlaying() const { return m_mode == EditorMode::Play; }
    [[nodiscard]] bool hasSnapshot() const { return !m_snapshot.empty(); }
    [[nodiscard]] size_t snapshotBytes() const { return m_snapshot.size(); }

private:
    EditorMode m_mode = EditorMode::Edit;
    std::vector<std::byte> m_snapshot;
};

} // namespace engine
