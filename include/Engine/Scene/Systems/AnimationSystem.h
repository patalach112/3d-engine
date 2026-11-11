#pragma once

#include "Core/Export.h"
#include "Scene/AnimationComponents.h"

#include <entt/entity/entity.hpp>

#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace engine {

class Scene;

// ---------------------------------------------------------------------------
// AnimationSystem — evaluates AnimationPlayerComponents against
// RigComponents every tick: advances playheads, crossfades between
// clips (the runtime side of `play.animation("x") blend 0.5s` /
// `over Xs`), samples keyframe tracks and writes node LOCAL
// transforms. While a rig's ragdoll is active the physics solver
// owns the nodes and the player freezes; deactivating the ragdoll
// restores the captured bind pose exactly.
//
// Ragdoll reconciliation lives here too: the component's `enabled`
// flag is the desired state (editor toggle, DSL enable.ragdoll()),
// and update() spawns/removes the rigid bodies, colliders and hinge
// joints that bind the rig into the physics world.
//
// Editor preview uses AnimationContext: startPreview snapshots the
// playback state + node transforms, updatePreview ticks the player
// in Edit mode, stopPreview restores the snapshot verbatim — the
// authored scene is never polluted by previewing.
// ---------------------------------------------------------------------------

struct AnimationContext {
    struct PreviewState {
        AnimationPlayerComponent playback; // player state to restore
        std::vector<std::pair<entt::entity, TransformComponent>> transforms;
    };
    std::unordered_map<entt::entity, PreviewState> previews;
};

// Easing curves shared by keyframe sampling and clip crossfades.
[[nodiscard]] ENGINE_SCENE_API double animEase(AnimInterpolation mode, double t);

// Samples one track at `time` (seconds, already wrapped/clamped by
// the caller). Returns false when the track has no keys.
[[nodiscard]] ENGINE_SCENE_API bool sampleTrack(const AnimTrack& track, double time, AnimPose& out);

// Samples one node's pose from a clip (loop wrap / end clamp per
// the clip's flags). Returns false when the clip has no such track.
[[nodiscard]] ENGINE_SCENE_API bool sampleClip(const AnimationClip& clip, const std::string& node, double time,
                                              AnimPose& out);

// Keyframe editing (studio + tests share these exact operations):
// inserts keeping keys sorted (an existing key at the same time is
// replaced), and extends the clip duration to cover the key.
ENGINE_SCENE_API void addKeyframe(AnimationClip& clip, const std::string& node, const AnimKeyframe& key);
[[nodiscard]] ENGINE_SCENE_API bool removeKeyframe(AnimationClip& clip, const std::string& node, double time);

class ENGINE_SCENE_API AnimationSystem {
public:
    // Play-mode tick: advance players, apply ragdoll transitions,
    // write node transforms. Call between ScriptSystem::update and
    // the physics step so script commands land the same frame.
    static void update(Scene& scene, double deltaSeconds);

    // Editor Anim Preview (Edit mode): same evaluation for rigs
    // registered with startPreview, without touching Play state.
    static void startPreview(Scene& scene, AnimationContext& context, entt::entity rig);
    static void stopPreview(Scene& scene, AnimationContext& context, entt::entity rig);
    static void stopAllPreviews(Scene& scene, AnimationContext& context);
    [[nodiscard]] static bool previewActive(const AnimationContext& context, entt::entity rig);
    static void updatePreview(Scene& scene, AnimationContext& context, double deltaSeconds);

    // Starts a clip on the rig's player (optionally crossfading
    // over blendSeconds). Returns false when the rig has no player
    // or the clip name is unknown. Restart-current restarts at 0.
    static bool playClip(Scene& scene, entt::entity rig, std::string_view clipName, double blendSeconds = 0.0);
    static void stopClip(Scene& scene, entt::entity rig);

    // Flips the ragdoll's desired state AND applies the transition
    // immediately (bodies/joints spawn or restore now). Safe to
    // call from the editor in Edit mode and from tests.
    static bool setRagdollEnabled(Scene& scene, entt::entity rig, bool enabled);
    [[nodiscard]] static bool ragdollActive(Scene& scene, entt::entity rig);
};

namespace AnimationRigging {
// Auto-rigging: maps a scene subtree into a RigComponent — the
// root becomes node 0, every descendant one articulation node with
// its parent index recorded, names taken from TagComponents
// (duplicates suffixed). The result is meant to be stored on the
// root entity; articulation points are then refined (joint axis,
// limits) in the VibeAnim hierarchy panel.
[[nodiscard]] ENGINE_SCENE_API RigComponent buildFromHierarchy(Scene& scene, entt::entity root);
} // namespace AnimationRigging

} // namespace engine
