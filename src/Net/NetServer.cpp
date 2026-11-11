#include "Net/NetServer.h"

#include "Core/Logger.h"
#include "Physics/PhysicsComponents.h"
#include "Physics/PhysicsEvents.h"
#include "Scene/Components.h"

#include <algorithm>
#include <cmath>

namespace engine {
namespace {

std::uint32_t wireId(entt::entity entity) {
    return static_cast<std::uint32_t>(entt::to_integral(entity));
}

entt::entity entityFromWire(std::uint32_t id) {
    return static_cast<entt::entity>(id);
}

std::string sanitizePlayerName(const std::string& raw) {
    std::string name;
    name.reserve(std::min<std::size_t>(raw.size(), 32));
    for (const char c : raw) {
        if (name.size() >= 32)
            break;
        const auto uc = static_cast<unsigned char>(c);
        if (uc >= 32 && uc != 127)
            name.push_back(c);
    }
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    return name.empty() ? std::string("Player") : name;
}

std::string tagOf(Scene& scene, entt::entity entity) {
    auto& registry = scene.registry();
    if (entity != entt::null && registry.valid(entity) && registry.all_of<TagComponent>(entity))
        return registry.get<TagComponent>(entity).tag;
    return {};
}

} // namespace

NetServer::NetServer() : m_timestep(LoopConfig{}) {
    m_transportEvents.reserve(64);
    m_currentStates.reserve(256);
    m_pendingEvents.reserve(64);
    m_packetScratch.reserve(4096);
}

NetServer::~NetServer() {
    stop();
}

bool NetServer::start(const ServerSettings& settings) {
    stop();
    m_settings = settings;
    m_settings.clampToValid();
    m_movement.setSettings(m_settings.move);

    LoopConfig loop;
    loop.fixedDeltaSeconds = m_settings.fixedDt();
    loop.maxFixedStepsPerFrame = 32;
    loop.maxFrameSeconds = 0.5;
    m_timestep = FixedTimestep(loop);

    m_scene.setEventBus(&m_eventBus);
    m_physics.setEventBus(&m_eventBus);
    ScriptSystem::attach(m_scriptContext, m_eventBus);

    // Physics -> reliable game events: the server's trigger and
    // contact truth is republished to every client; client scripts
    // may react for presentation, never for gameplay.
    m_triggerSubscription = m_eventBus.subscribeScoped<TriggerEvent>([this](const TriggerEvent& event) {
        if (!event.entered)
            return;
        NetGameEvent out;
        out.kind = NetGameEventKind::Trigger;
        out.entity = wireId(event.otherEntity);
        out.other = wireId(event.triggerEntity);
        out.text = tagOf(m_scene, event.triggerEntity);
        out.serverTick = m_tick;
        queueEvent(std::move(out));
    });
    m_collisionSubscription = m_eventBus.subscribeScoped<CollisionEvent>([this](const CollisionEvent& event) {
        NetGameEvent out;
        out.kind = NetGameEventKind::Touch;
        out.entity = wireId(event.entityA);
        out.other = wireId(event.entityB);
        out.value = static_cast<std::int32_t>(event.impulse);
        out.text = tagOf(m_scene, event.entityB);
        out.serverTick = m_tick;
        queueEvent(std::move(out));
    });

    const std::size_t maxPeers = static_cast<std::size_t>(m_settings.maxPlayers) + 8;
    if (!m_transport.createServer(m_settings.port, maxPeers)) {
        ENGINE_LOG_ERROR("NetServer: failed to bind port " << m_settings.port);
        return false;
    }
    m_tick = 0;
    m_running = true;
    ENGINE_LOG_INFO("NetServer: '" << m_settings.serverName << "' listening on port " << port() << " at "
                                   << m_settings.tickRate << " Hz, max " << m_settings.maxPlayers << " players");
    return true;
}

void NetServer::stop() {
    if (!m_running && !m_transport.active())
        return;
    m_triggerSubscription = EventBus::Subscription{};
    m_collisionSubscription = EventBus::Subscription{};
    m_transport.shutdown();
    m_peers.clear();
    m_running = false;
}

std::uint16_t NetServer::port() const {
    return m_transport.localPort();
}

void NetServer::step() {
    if (!m_running)
        return;
    m_transportEvents.clear();
    m_transport.poll(m_transportEvents, 0);
    for (const TransportEvent& event : m_transportEvents)
        handleTransportEvent(event);
    m_transportEvents.clear();

    ++m_tick;
    const float dt = m_settings.fixedDt();
    simulatePlayers(dt);
    m_physics.update(m_scene, dt, m_physicsSettings, nullptr);
    ScriptSystem::update(m_scene, m_scriptContext, dt);
    updateRespawns();
    gatherCurrentStates();
    sendSnapshots();
    flushPendingEvents();
}

std::uint32_t NetServer::advance(double realSeconds) {
    if (!m_running)
        return 0;
    const FixedTimestep::Result result = m_timestep.advance(realSeconds);
    for (std::uint32_t i = 0; i < result.steps; ++i)
        step();
    return result.steps;
}

// --- transport / packet handling ----------------------------------------------

void NetServer::handleTransportEvent(const TransportEvent& event) {
    switch (event.type) {
    case TransportEventType::Connect: {
        PeerSession peer;
        peer.peerId = event.peerId;
        m_peers.push_back(std::move(peer));
        break;
    }
    case TransportEventType::Receive:
        handlePacket(event.peerId, event.data.data(), event.data.size());
        break;
    case TransportEventType::Disconnect:
        removePeer(event.peerId);
        break;
    case TransportEventType::None:
        break;
    }
}

void NetServer::handlePacket(std::uint32_t peerId, const std::byte* data, std::size_t size) {
    if (data == nullptr || size == 0)
        return;
    PeerSession* peer = findPeer(peerId);
    if (peer == nullptr)
        return;
    const auto type = static_cast<NetPacketType>(static_cast<std::uint8_t>(data[0]));
    switch (type) {
    case NetPacketType::Hello: {
        ClientHello hello;
        if (!peer->welcomed && ClientHello::deserialize(data, size, hello))
            handleHello(*peer, hello);
        break;
    }
    case NetPacketType::InputCommand: {
        InputCommand command;
        if (peer->welcomed && InputCommand::deserialize(data, size, command)) {
            // No sanitisation here: playerMove() clamps every command into the legal envelope as it consumes it.
            peer->baseline.onAck(command.lastAck);
            if (peer->inputs.size() >= 64)
                peer->inputs.pop_front();
            peer->inputs.push_back(command);
        }
        break;
    }
    case NetPacketType::StatusRequest: {
        NetStatus reply = status();
        reply.pingMs = m_transport.roundTripTime(peerId);
        sendToPeer(peerId, reply.serialize());
        break;
    }
    case NetPacketType::ActionRequest: {
        NetTextPacket packet;
        if (peer->welcomed && NetTextPacket::deserialize(data, size, NetPacketType::ActionRequest, packet))
            handleActionRequest(*peer, packet.text);
        break;
    }
    case NetPacketType::Disconnect: {
        NetTextPacket packet;
        if (NetTextPacket::deserialize(data, size, NetPacketType::Disconnect, packet))
            ENGINE_LOG_INFO("NetServer: client " << peerId << " disconnecting: " << packet.text);
        removePeer(peerId);
        m_transport.disconnectPeer(peerId, true);
        break;
    }
    default:
        break; // Welcome/Snapshot/GameEvent/... are server->client only
    }
}

void NetServer::handleHello(PeerSession& peer, const ClientHello& hello) {
    std::size_t welcomedCount = 0;
    for (const PeerSession& other : m_peers) {
        if (other.welcomed)
            ++welcomedCount;
    }
    if (welcomedCount >= static_cast<std::size_t>(m_settings.maxPlayers)) {
        sendToPeer(peer.peerId, NetTextPacket{NetPacketType::Disconnect, "server full"}.serialize());
        m_transport.disconnectPeer(peer.peerId, false);
        return;
    }

    peer.name = sanitizePlayerName(hello.playerName);
    const std::uint32_t slot = m_nextSpawnSlot++;
    const glm::vec3 spawn{static_cast<float>(slot) * 2.5f, m_settings.move.groundY, 0.0f};

    Entity entity = m_scene.createEntity(peer.name);
    entity.addComponent<TransformComponent>(TransformComponent{spawn});
    NetPlayerComponent& player = entity.addComponent<NetPlayerComponent>();
    player.peerId = peer.peerId;
    player.playerName = peer.name;
    player.spawnPosition = spawn;
    player.move.position = spawn;
    player.move.grounded = true;
    entity.addComponent<HealthComponent>();
    entity.addComponent<ReplicatedComponent>();
    peer.entity = entity.handle();
    peer.welcomed = true;

    ServerWelcome welcome;
    welcome.entityId = wireId(peer.entity);
    welcome.serverTick = m_tick;
    welcome.tickRate = m_settings.tickRate;
    welcome.maxPlayers = m_settings.maxPlayers;
    welcome.fixedDt = m_settings.fixedDt();
    welcome.move = m_settings.move;
    welcome.serverName = m_settings.serverName;
    sendToPeer(peer.peerId, welcome.serialize());

    ENGINE_LOG_INFO("NetServer: player '" << peer.name << "' joined (peer " << peer.peerId << ", entity "
                                          << welcome.entityId << ")");
    if (m_callbacks.onPlayerJoined) {
        if (const auto info = playerInfo(peer))
            m_callbacks.onPlayerJoined(*info);
    }
}

void NetServer::handleActionRequest(PeerSession& peer, const std::string& text) {
    const std::size_t split = text.find('\n');
    const std::string name = split == std::string::npos ? text : text.substr(0, split);
    const std::string payload = split == std::string::npos ? std::string{} : text.substr(split + 1);

    // Per-peer rate limit: 20 actions per second of server time.
    const std::uint32_t window = static_cast<std::uint32_t>(m_settings.tickRate);
    if (m_tick - peer.actionWindowTick >= window) {
        peer.actionWindowTick = m_tick;
        peer.actionsInWindow = 0;
    }
    bool denied = name.empty() || name.size() > 64 || ++peer.actionsInWindow > 20;
    const auto it = denied ? m_actions.end() : m_actions.find(name);
    if (it == m_actions.end())
        denied = true;

    if (denied) {
        ++m_stats.actionsDenied;
        NetGameEvent event;
        event.kind = NetGameEventKind::Message;
        event.entity = wireId(peer.entity);
        event.text = "action denied: " + name;
        event.serverTick = m_tick;
        sendToPeer(peer.peerId, event.serialize());
        return;
    }
    std::string reply;
    if (it->second(peer.peerId, payload, reply)) {
        ++m_stats.actionsAccepted;
        sendToPeer(peer.peerId,
                   NetTextPacket{NetPacketType::ClientAction, name + "\n" + reply}.serialize());
    } else {
        ++m_stats.actionsDenied;
        NetGameEvent event;
        event.kind = NetGameEventKind::Message;
        event.entity = wireId(peer.entity);
        event.text = "action rejected: " + name;
        event.serverTick = m_tick;
        sendToPeer(peer.peerId, event.serialize());
    }
}

void NetServer::removePeer(std::uint32_t peerId) {
    for (std::size_t i = 0; i < m_peers.size(); ++i) {
        if (m_peers[i].peerId != peerId)
            continue;
        PeerSession peer = std::move(m_peers[i]);
        m_peers.erase(m_peers.begin() + static_cast<std::ptrdiff_t>(i));
        if (peer.welcomed && peer.entity != entt::null && m_scene.isValid(Entity{peer.entity, &m_scene})) {
            m_scene.destroyEntity(Entity{peer.entity, &m_scene});
            ENGINE_LOG_INFO("NetServer: player '" << peer.name << "' left (peer " << peerId << ")");
        }
        if (peer.welcomed && m_callbacks.onPlayerLeft)
            m_callbacks.onPlayerLeft(peerId);
        return;
    }
}

NetServer::PeerSession* NetServer::findPeer(std::uint32_t peerId) {
    for (PeerSession& peer : m_peers) {
        if (peer.peerId == peerId)
            return &peer;
    }
    return nullptr;
}

// --- simulation -------------------------------------------------------------------

void NetServer::simulatePlayers(float dt) {
    auto& registry = m_scene.registry();
    for (PeerSession& peer : m_peers) {
        if (!peer.welcomed || peer.entity == entt::null || !registry.valid(peer.entity))
            continue;
        auto& player = registry.get<NetPlayerComponent>(peer.entity);
        const HealthComponent* health =
            registry.all_of<HealthComponent>(peer.entity) ? &registry.get<HealthComponent>(peer.entity) : nullptr;
        const bool alive = health == nullptr || health->alive;
        if (alive) {
            if (!peer.inputs.empty()) {
                while (!peer.inputs.empty()) {
                    const InputCommand command = peer.inputs.front();
                    peer.inputs.pop_front();
                    // Sequence discipline: a command at or below the last consumed
                    // cmd_number is a duplicate/straggler; never simulate it twice.
                    if (player.lastCmdNumber != 0 && !netTickNewer(command.cmdNumber, player.lastCmdNumber))
                        continue;
                    player.lastInput = command.cmd;
                    player.lastCmdNumber = command.cmdNumber;
                    m_movement.playerMove(command.cmd, player.move, dt);
                    ++m_stats.inputCommandsConsumed;
                }
            } else {
                // Input persistence (Source behaviour): starved of
                // fresh commands, the server replays the last one —
                // the same assumption client prediction makes.
                m_movement.playerMove(player.lastInput, player.move, dt);
            }
        } else {
            peer.inputs.clear();
        }
        if (registry.all_of<TransformComponent>(peer.entity)) {
            auto& transform = registry.get<TransformComponent>(peer.entity);
            transform.position = player.move.position;
            transform.rotationDegrees.y = player.lastInput.yawDegrees;
        }
    }
}

void NetServer::updateRespawns() {
    auto& registry = m_scene.registry();
    for (PeerSession& peer : m_peers) {
        if (!peer.welcomed || peer.entity == entt::null || !registry.valid(peer.entity))
            continue;
        if (!registry.all_of<NetPlayerComponent, HealthComponent>(peer.entity))
            continue;
        auto& player = registry.get<NetPlayerComponent>(peer.entity);
        auto& health = registry.get<HealthComponent>(peer.entity);
        if (health.alive || m_tick < player.respawnTick)
            continue;
        health.health = health.maxHealth;
        health.alive = true;
        player.move.position = player.spawnPosition;
        player.move.velocity = glm::vec3{0.0f};
        player.move.grounded = true;
        if (registry.all_of<TransformComponent>(peer.entity))
            registry.get<TransformComponent>(peer.entity).position = player.spawnPosition;
        NetGameEvent event;
        event.kind = NetGameEventKind::Respawn;
        event.entity = wireId(peer.entity);
        event.value = health.health;
        event.serverTick = m_tick;
        queueEvent(event);
    }
}

// --- replication ---------------------------------------------------------------------

void NetServer::gatherCurrentStates() {
    m_currentStates.clear();
    auto& registry = m_scene.registry();
    const auto view = registry.view<ReplicatedComponent, TransformComponent>();
    for (const entt::entity entity : view) {
        if (!view.get<ReplicatedComponent>(entity).replicate)
            continue;
        EntitySnapshot state;
        state.entity = wireId(entity);
        const auto& transform = view.get<TransformComponent>(entity);
        state.position = transform.position;
        state.yaw = transform.rotationDegrees.y;
        if (registry.all_of<NetPlayerComponent>(entity)) {
            state.velocity = registry.get<NetPlayerComponent>(entity).move.velocity;
        } else if (registry.all_of<RigidBodyComponent>(entity)) {
            state.velocity = registry.get<RigidBodyComponent>(entity).velocity;
        }
        if (registry.all_of<HealthComponent>(entity)) {
            const auto& health = registry.get<HealthComponent>(entity);
            state.hasHealth = true;
            state.health = static_cast<std::int16_t>(std::clamp(health.health, -32768, 32767));
            state.alive = health.alive;
        }
        m_currentStates.push_back(state);
    }
    std::sort(m_currentStates.begin(), m_currentStates.end(),
              [](const EntitySnapshot& a, const EntitySnapshot& b) { return a.entity < b.entity; });
}

void NetServer::sendSnapshots() {
    for (PeerSession& peer : m_peers) {
        if (!peer.welcomed)
            continue;
        std::uint32_t lastCmdNumber = 0;
        auto& registry = m_scene.registry();
        if (peer.entity != entt::null && registry.valid(peer.entity) &&
            registry.all_of<NetPlayerComponent>(peer.entity))
            lastCmdNumber = registry.get<NetPlayerComponent>(peer.entity).lastCmdNumber;
        const Snapshot snapshot = buildSnapshot(m_currentStates, peer.baseline, m_tick, lastCmdNumber);
        m_stats.snapshotEntitiesSent += snapshot.entities.size();
        m_stats.snapshotEntitiesSkipped += m_currentStates.size() > snapshot.entities.size()
                                               ? m_currentStates.size() - snapshot.entities.size()
                                               : 0;
        sendToPeer(peer.peerId, snapshot.serialize());
        peer.baseline.recordSent(m_tick, m_currentStates);
        ++m_stats.snapshotsSent;
    }
}

void NetServer::flushPendingEvents() {
    // Swap first: event callbacks may queue further events, and
    // those must wait for the next tick rather than mutate the
    // vector being broadcast.
    m_flushingEvents.clear();
    m_flushingEvents.swap(m_pendingEvents);
    for (const NetGameEvent& event : m_flushingEvents) {
        m_packetScratch = event.serialize();
        m_transport.broadcast(m_packetScratch.data(), m_packetScratch.size(),
                              static_cast<std::uint8_t>(NetChannel::Reliable), true);
        ++m_stats.gameEventsSent;
        if (m_callbacks.onGameEvent)
            m_callbacks.onGameEvent(event);
    }
    m_flushingEvents.clear();
}

void NetServer::sendToPeer(std::uint32_t peerId, const std::vector<std::byte>& packet) {
    if (packet.empty())
        return;
    const auto type = static_cast<NetPacketType>(static_cast<std::uint8_t>(packet[0]));
    const std::uint8_t channel = channelOf(type);
    (void)m_transport.send(peerId, packet.data(), packet.size(), channel, channel == 0);
}

void NetServer::queueEvent(NetGameEvent event) {
    m_pendingEvents.push_back(std::move(event));
}

// --- public queries / authoritative API -----------------------------------------------

NetStatus NetServer::status() const {
    NetStatus status;
    status.alive = m_running;
    std::size_t count = 0;
    for (const PeerSession& peer : m_peers) {
        if (peer.welcomed)
            ++count;
    }
    status.playerCount = static_cast<int>(count);
    status.maxPlayers = m_settings.maxPlayers;
    status.serverTick = m_tick;
    status.tickRate = m_settings.tickRate;
    status.serverName = m_settings.serverName;
    return status;
}

std::optional<ServerPlayerInfo> NetServer::playerInfo(const PeerSession& peer) const {
    if (!peer.welcomed)
        return std::nullopt;
    ServerPlayerInfo info;
    info.peerId = peer.peerId;
    info.entityId = wireId(peer.entity);
    info.name = peer.name;
    info.pingMs = m_transport.roundTripTime(peer.peerId);
    const auto& registry = m_scene.registry();
    if (peer.entity != entt::null && registry.valid(peer.entity) &&
        registry.all_of<HealthComponent>(peer.entity))
        info.alive = registry.get<HealthComponent>(peer.entity).alive;
    return info;
}

std::vector<ServerPlayerInfo> NetServer::players() const {
    std::vector<ServerPlayerInfo> out;
    for (const PeerSession& peer : m_peers) {
        if (const auto info = playerInfo(peer))
            out.push_back(*info);
    }
    return out;
}

bool NetServer::applyDamage(std::uint32_t entityId, std::int32_t amount, std::uint32_t attackerEntityId) {
    auto& registry = m_scene.registry();
    const entt::entity entity = entityFromWire(entityId);
    if (!registry.valid(entity) || !registry.all_of<HealthComponent>(entity) || amount <= 0)
        return false;
    auto& health = registry.get<HealthComponent>(entity);
    if (!health.alive)
        return false;
    health.health = std::max(0, health.health - amount);
    NetGameEvent damage;
    damage.kind = NetGameEventKind::Damage;
    damage.entity = entityId;
    damage.other = attackerEntityId;
    damage.value = amount;
    damage.serverTick = m_tick;
    queueEvent(damage);
    NetGameEvent changed;
    changed.kind = NetGameEventKind::HealthChanged;
    changed.entity = entityId;
    changed.value = health.health;
    changed.serverTick = m_tick;
    queueEvent(changed);
    if (health.health == 0) {
        health.alive = false;
        NetGameEvent death;
        death.kind = NetGameEventKind::Death;
        death.entity = entityId;
        death.other = attackerEntityId;
        death.serverTick = m_tick;
        queueEvent(death);
        if (registry.all_of<NetPlayerComponent>(entity)) {
            auto& player = registry.get<NetPlayerComponent>(entity);
            player.respawnTick = m_tick + static_cast<std::uint32_t>(m_settings.tickRate) * 3;
            player.move.velocity = glm::vec3{0.0f};
        }
    }
    return true;
}

bool NetServer::applyHeal(std::uint32_t entityId, std::int32_t amount) {
    auto& registry = m_scene.registry();
    const entt::entity entity = entityFromWire(entityId);
    if (!registry.valid(entity) || !registry.all_of<HealthComponent>(entity) || amount <= 0)
        return false;
    auto& health = registry.get<HealthComponent>(entity);
    if (!health.alive)
        return false;
    health.health = std::min(health.maxHealth, health.health + amount);
    NetGameEvent changed;
    changed.kind = NetGameEventKind::HealthChanged;
    changed.entity = entityId;
    changed.value = health.health;
    changed.serverTick = m_tick;
    queueEvent(changed);
    return true;
}

bool NetServer::teleportEntity(std::uint32_t entityId, const glm::vec3& position) {
    auto& registry = m_scene.registry();
    const entt::entity entity = entityFromWire(entityId);
    if (!registry.valid(entity))
        return false;
    if (registry.all_of<TransformComponent>(entity))
        registry.get<TransformComponent>(entity).position = position;
    if (registry.all_of<NetPlayerComponent>(entity)) {
        auto& player = registry.get<NetPlayerComponent>(entity);
        player.move.position = position;
        player.move.velocity = glm::vec3{0.0f};
        player.move.grounded = position.y <= m_settings.move.groundY + 1e-4f;
    }
    return true;
}

void NetServer::broadcastGameEvent(const NetGameEvent& event) {
    NetGameEvent copy = event;
    copy.serverTick = m_tick;
    queueEvent(std::move(copy));
}

void NetServer::registerAction(std::string name, ActionHandler handler) {
    if (!name.empty() && name.size() <= 64 && handler)
        m_actions[std::move(name)] = std::move(handler);
}

} // namespace engine
