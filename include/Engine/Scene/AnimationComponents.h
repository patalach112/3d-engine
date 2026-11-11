#pragma once

#include "Scene/Components.h"

#include <glm/gtc/quaternion.hpp>
#include <glm/vec3.hpp>

#include <entt/entity/entity.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// Animation & rigging components (VibeAnim studio data model)
//
// A rig is a subtree of ordinary scene entities: the root carries
// RigComponent (the articulation map) plus AnimationPlayerComponent
// (the clip library + playback state), and every descendant entity
// is one articulation point (shoulder, elbow, knee, ...) whose LOCAL
// TransformComponent the AnimationSystem drives. Animation data is
// plain serializable component data — clips, tracks and keyframes
// live in the player component, so GBQ/JSON saves, Play snapshots
// and the editor all see the same asset, and `.dsla` scripts refer
// to clips by name (play.animation("walk")).
//
// Keyframes store full TRS (position / Euler rotation degrees /
// scale) captured from the node's local transform; rotation is
// slerped through quaternions at sample time so interpolation never
// hits Euler wrap. A keyframe's `interpolation` shapes the segment
// FROM that key to the next one.
// ---------------------------------------------------------------------------

enum class AnimInterpolation : uint8_t { Linear, EaseInOut, Step };

struct AnimKeyframe {
    double time = 0.0; // seconds into the clip
    glm::vec3 position{0.0f};
    glm::vec3 rotationDegrees{0.0f}; // Euler Y-X-Z, same order as TransformComponent
    glm::vec3 scale{1.0f};
    AnimInterpolation interpolation = AnimInterpolation::Linear; // segment to the NEXT key
};

// One node's keyframe lane inside a clip. Keys stay sorted by time.
struct AnimTrack {
    std::string node; // rig node name (the descendant entity's tag)
    std::vector<AnimKeyframe> keys;
};

struct AnimationClip {
    std::string name;
    double duration = 1.0; // seconds; sampling clamps/wraps to this
    bool loop = true;
    std::vector<AnimTrack> tracks;

    [[nodiscard]] const AnimTrack* findTrack(const std::string& node) const {
        for (const AnimTrack& track : tracks) {
            if (track.node == node) {
                return &track;
            }
        }
        return nullptr;
    }
    [[nodiscard]] AnimTrack& trackFor(const std::string& node) {
        for (AnimTrack& track : tracks) {
            if (track.node == node) {
                return track;
            }
        }
        tracks.push_back(AnimTrack{node, {}});
        return tracks.back();
    }
};

// One articulation point of a rig. `entity` carries the node's local
// transform (and, while ragdolled, its rigid body); `parent` indexes
// RigComponent::nodes (-1 = directly under the rig root's own node 0).
struct RigNode {
    std::string name;
    entt::entity entity = entt::null;
    int32_t parent = -1;
    glm::vec3 jointAxis{0.0f, 0.0f, 1.0f}; // ragdoll hinge axis (local)
    float minAngleDegrees = -120.0f;       // ragdoll hinge limits
    float maxAngleDegrees = 120.0f;
};

struct RigComponent {
    std::vector<RigNode> nodes; // node 0 is the rig root entity itself

    [[nodiscard]] const RigNode* findNode(const std::string& name) const {
        for (const RigNode& node : nodes) {
            if (node.name == name) {
                return &node;
            }
        }
        return nullptr;
    }
    [[nodiscard]] const RigNode* findNode(entt::entity entity) const {
        for (const RigNode& node : nodes) {
            if (node.entity == entity) {
                return &node;
            }
        }
        return nullptr;
    }
};

// Clip library + playback state on the rig root. All fields are
// data (the AnimationSystem advances them), so mid-playback state
// serializes and Play/Stop snapshots stay consistent.
struct AnimationPlayerComponent {
    std::vector<AnimationClip> clips;
    std::string currentClip;   // "" = nothing playing
    std::string blendFromClip; // clip crossfading out ("" = no blend)
    double time = 0.0;         // playhead inside currentClip
    double blendFromTime = 0.0;
    double blendElapsed = 0.0;
    double blendDuration = 0.0; // > 0 while a crossfade runs
    float speed = 1.0f;
    bool playing = false;

    [[nodiscard]] const AnimationClip* findClip(const std::string& name) const {
        for (const AnimationClip& clip : clips) {
            if (clip.name == name) {
                return &clip;
            }
        }
        return nullptr;
    }
    [[nodiscard]] AnimationClip* findClip(const std::string& name) {
        for (AnimationClip& clip : clips) {
            if (clip.name == name) {
                return &clip;
            }
        }
        return nullptr;
    }
};

// Sampled pose for one node at one instant (rotation as quaternion).
struct AnimPose {
    glm::vec3 position{0.0f};
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
};

// Ragdoll state on the rig root. `enabled` is the desired state
// (the editor toggle and enable.ragdoll()/disable.ragdoll() write
// it); `active` is system-managed and true while node bodies/joints
// exist. The bind pose (local transforms at activation, parallel to
// RigComponent::nodes) is restored verbatim on deactivation.
struct RagdollComponent {
    bool enabled = false;
    bool active = false;
    float bodyMass = 1.0f;
    float compliance = 0.05f; // joint softness (0 hard .. 1 soft)
    std::vector<TransformComponent> bindTransforms; // runtime-captured
};

// Marker the ragdoll system stamps on node entities it equipped, so
// deactivation removes exactly the components it added and never
// strips physics an author placed by hand. Runtime-only.
struct RagdollNodeComponent {
    entt::entity rig = entt::null;
    bool addedBody = false;
    bool addedCollider = false;
    bool addedJoint = false;
};

} // namespace engine
