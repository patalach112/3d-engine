#pragma once
#include "Net/FrictionController.h"

#include <cstdint>
#include <string>
#include <glm/vec3.hpp>

namespace engine {

// ---------------------------------------------------------------------------
// Network components (EngineNet-owned, plain data in dense EnTT
// storage — the Data-Oriented replication set).
//
// The server registry and the client mirror registry both store
// these; EngineNet code is the only writer of NetPlayerComponent
// state on the server, which is what makes gameplay server-
// authoritative: clients hold mirrors, never masters.
// ---------------------------------------------------------------------------

// Marks an entity for snapshot replication. Entities without this
// component are server-local (or client-local) and never cross
// the wire.
struct ReplicatedComponent {
    bool replicate = true;
};

// Server-side per-player state for a connected peer: the peer <->
// entity binding created at handshake, the authoritative movement
// state the friction controller integrates, and respawn data.
struct NetPlayerComponent {
    std::uint32_t peerId = 0;
    std::string playerName;
    glm::vec3 spawnPosition{0.0f};
    PlayerMoveState move{};          // authoritative position/velocity/grounded
    PlayerMoveInput lastInput{};     // latest consumed command (input persistence)
    std::uint32_t lastInputTick = 0; // client tick of lastInput (reconcile anchor)
    std::uint32_t respawnTick = 0;   // server tick at which a dead player respawns
};

// Server-authoritative health. Only NetServer (applyDamage /
// applyHeal / respawn) mutates this in networked play; clients
// receive the value inside snapshots and GameEvents.
struct HealthComponent {
    std::int32_t health = 100;
    std::int32_t maxHealth = 100;
    bool alive = true;
};

// Client-side mirror bookkeeping: which wire entity this local
// entity mirrors. Written by NetClient::applyToScene.
struct NetMirrorComponent {
    std::uint32_t networkId = 0;
};

} // namespace engine
