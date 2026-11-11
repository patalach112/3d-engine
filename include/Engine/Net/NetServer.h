#pragma once
#include "Core/Export.h"

#include "Core/EventBus.h"
#include "Core/GameLoop.h"
#include "Net/NetComponents.h"
#include "Net/NetProtocol.h"
#include "Net/NetTransport.h"
#include "Net/ServerConfig.h"
#include "Net/SnapshotDelta.h"
#include "Scene/Scene.h"
#include "Scene/Systems/PhysicsSystem.h"
#include "Scripting/ScriptSystem.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// NetServer: the server-authoritative simulation host.
//
// Owns the ONLY master copy of the world (Scene + EnTT registry),
// a ScriptSystemContext running server-side .dsla logic, the
// PhysicsSystem (whose Collision/Trigger events are republished
// to clients as reliable NetGameEvents), and the ENet transport.
//
// Fixed-step model: step() advances exactly one tick of
// ServerSettings::fixedDt() (1/128 s by default — the classic
// Source dedicated-server loop); advance() drives step() from a
// wall clock through a FixedTimestep accumulator for real hosts.
//
// Per tick: pump transport -> consume each player's queued input
// commands through the shared ground-friction controller (the
// identical math the client predicts with) -> physics -> server
// scripts -> respawns -> per-client delta snapshots (unreliable
// channel) -> flush reliable game events.
//
// Gameplay mutation is server-only API: applyDamage / applyHeal /
// teleportEntity. Clients can merely REQUEST actions, which run
// exclusively through handlers registered with registerAction().
// ---------------------------------------------------------------------------

struct ServerPlayerInfo {
    std::uint32_t peerId = 0;
    std::uint32_t entityId = 0;
    std::string name;
    std::uint32_t pingMs = 0;
    bool alive = true;
};

struct NetServerStats {
    std::uint64_t snapshotsSent = 0;
    std::uint64_t snapshotEntitiesSent = 0;    // after delta compression
    std::uint64_t snapshotEntitiesSkipped = 0; // unchanged vs acked baseline
    std::uint64_t gameEventsSent = 0;
    std::uint64_t inputCommandsConsumed = 0;
    std::uint64_t actionsAccepted = 0;
    std::uint64_t actionsDenied = 0;
};

class ENGINE_NET_API NetServer {
public:
    struct Callbacks {
        std::function<void(const ServerPlayerInfo&)> onPlayerJoined;
        std::function<void(std::uint32_t peerId)> onPlayerLeft;
        std::function<void(const NetGameEvent&)> onGameEvent; // local tap (tools/tests)
    };
    // Return true to accept; fill `reply` to answer the client
    // with a ClientAction packet. Handlers run on the server tick.
    using ActionHandler =
        std::function<bool(std::uint32_t peerId, const std::string& payload, std::string& reply)>;

    NetServer();
    ~NetServer();
    NetServer(const NetServer&) = delete;
    NetServer& operator=(const NetServer&) = delete;

    [[nodiscard]] bool start(const ServerSettings& settings);
    void stop();
    [[nodiscard]] bool running() const { return m_running; }

    // Exactly one deterministic fixed tick.
    void step();
    // Wall-clock driver (FixedTimestep accumulator). Returns the
    // number of ticks executed.
    std::uint32_t advance(double realSeconds);

    [[nodiscard]] std::uint32_t tick() const { return m_tick; }
    [[nodiscard]] const ServerSettings& settings() const { return m_settings; }
    [[nodiscard]] std::uint16_t port() const;

    // The authoritative world. Hosts may author entities here
    // (spawn props, triggers, server scripts) before/between steps.
    [[nodiscard]] Scene& scene() { return m_scene; }
    [[nodiscard]] ScriptSystemContext& scriptContext() { return m_scriptContext; }
    [[nodiscard]] EventBus& eventBus() { return m_eventBus; }

    [[nodiscard]] NetStatus status() const;
    [[nodiscard]] std::vector<ServerPlayerInfo> players() const;
    [[nodiscard]] const NetServerStats& stats() const { return m_stats; }

    // --- Server-authoritative gameplay API -------------------------
    // Damage/heal clamp into [0, maxHealth]; lethal damage kills
    // (alive=false, Death event, player respawn scheduled).
    bool applyDamage(std::uint32_t entityId, std::int32_t amount, std::uint32_t attackerEntityId = 0);
    bool applyHeal(std::uint32_t entityId, std::int32_t amount);
    bool teleportEntity(std::uint32_t entityId, const glm::vec3& position);
    // Reliable event to every connected client.
    void broadcastGameEvent(const NetGameEvent& event);

    void registerAction(std::string name, ActionHandler handler);
    void setCallbacks(Callbacks callbacks) { m_callbacks = std::move(callbacks); }

private:
    struct PeerSession {
        std::uint32_t peerId = 0;
        entt::entity entity = entt::null;
        std::string name;
        bool welcomed = false;
        std::deque<InputCommand> inputs;
        SnapshotBaseline baseline;
        std::uint32_t actionWindowTick = 0;
        int actionsInWindow = 0;
    };

    void handleTransportEvent(const TransportEvent& event);
    void handlePacket(std::uint32_t peerId, const std::byte* data, std::size_t size);
    void handleHello(PeerSession& peer, const ClientHello& hello);
    void handleActionRequest(PeerSession& peer, const std::string& text);
    void removePeer(std::uint32_t peerId);
    PeerSession* findPeer(std::uint32_t peerId);
    void simulatePlayers(float dt);
    void updateRespawns();
    void gatherCurrentStates();
    void sendSnapshots();
    void flushPendingEvents();
    void sendToPeer(std::uint32_t peerId, const std::vector<std::byte>& packet);
    void queueEvent(NetGameEvent event);
    [[nodiscard]] std::optional<ServerPlayerInfo> playerInfo(const PeerSession& peer) const;

    ServerSettings m_settings;
    NetTransport m_transport;
    Scene m_scene;
    EventBus m_eventBus;
    PhysicsSystem m_physics;
    PhysicsSettings m_physicsSettings;
    ScriptSystemContext m_scriptContext;
    FixedTimestep m_timestep;
    Callbacks m_callbacks;
    NetServerStats m_stats;

    bool m_running = false;
    std::uint32_t m_tick = 0;
    std::uint32_t m_nextSpawnSlot = 0;
    std::vector<PeerSession> m_peers;
    std::unordered_map<std::string, ActionHandler> m_actions;

    // Scratch buffers reused every tick (zero-allocation hot loop).
    std::vector<TransportEvent> m_transportEvents;
    std::vector<EntitySnapshot> m_currentStates;
    std::vector<NetGameEvent> m_pendingEvents;
    std::vector<NetGameEvent> m_flushingEvents;
    std::vector<std::byte> m_packetScratch;

    EventBus::Subscription m_triggerSubscription;
    EventBus::Subscription m_collisionSubscription;
};

} // namespace engine
