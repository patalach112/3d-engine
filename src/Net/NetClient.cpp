#include "Net/NetClient.h"

#include "Scene/Components.h"
#include "Scripting/ScriptSystem.h"

#include <algorithm>
#include <cmath>
#include <glm/geometric.hpp>

namespace engine {
namespace {

constexpr std::size_t kMaxPredictedFrames = 1024;
constexpr float kMispredictThreshold = 0.01f;  // 1 cm: below this, prediction was right
constexpr float kSnapDistance = 2.0f;          // beyond this, corrections snap (teleports)
constexpr float kCorrectionDecayRate = 12.0f;  // visual offset decay, per second

PlayerMoveState authoritativeToMoveState(const EntitySnapshot& state, const MoveSettings& settings) {
    PlayerMoveState move;
    move.position = state.position;
    move.velocity = state.velocity;
    move.grounded = state.position.y <= settings.groundY + 1e-4f && state.velocity.y <= 0.0f;
    return move;
}

} // namespace

NetClient::NetClient() {
    m_transportEvents.reserve(64);
}

NetClient::~NetClient() {
    disconnect();
}

bool NetClient::connect(const std::string& host, std::uint16_t port, const std::string& playerName) {
    disconnect();
    m_playerName = playerName;
    if (!m_transport.createClient())
        return false;
    if (!m_transport.connect(host, port)) {
        m_transport.shutdown();
        return false;
    }
    m_connected = true;
    return true;
}

void NetClient::disconnect(const std::string& reason) {
    if (m_transport.active()) {
        if (m_connected && m_serverPeerId != 0) {
            // Queue the polite Disconnect, then force it (and the
            // ENet disconnect command) onto the wire BEFORE the
            // host dies, or the server would never learn we left.
            sendPacket(NetTextPacket{NetPacketType::Disconnect, reason}.serialize());
            m_transport.flush();
            m_transport.disconnectPeer(m_serverPeerId, true);
            m_transport.flush();
        }
        m_transport.shutdown();
    }
    m_connected = false;
    m_welcomed = false;
    m_helloSent = false;
    m_serverPeerId = 0;
}

void NetClient::tickOnce(const PlayerMoveInput& input) {
    m_transportEvents.clear();
    m_transport.poll(m_transportEvents, 0);
    for (const TransportEvent& event : m_transportEvents)
        handleTransportEvent(event);
    m_transportEvents.clear();

    if (!m_welcomed)
        return;

    const float dt = m_welcome.fixedDt > 0.0f ? m_welcome.fixedDt : 1.0f / 128.0f;
    // Decay the visual correction from the last reconcile.
    m_visualOffset *= std::exp(-kCorrectionDecayRate * dt);
    if (glm::length(m_visualOffset) < 1e-5f)
        m_visualOffset = glm::vec3{0.0f};

    ++m_tick;
    const PlayerMoveInput clean = sanitizeMoveInput(input);
    groundMove(m_predicted, clean, m_welcome.move, dt);
    m_predictedFrames.push_back(PredictedFrame{m_tick, clean, m_predicted});
    while (m_predictedFrames.size() > kMaxPredictedFrames)
        m_predictedFrames.pop_front();

    InputCommand command;
    command.tick = m_tick;
    command.lastAck = m_lastAckSent;
    command.input = clean;
    sendPacket(command.serialize());
    ++m_stats.inputsSent;
}

std::uint32_t NetClient::update(double realSeconds, const PlayerMoveInput& input) {
    const double dt = m_welcomed && m_welcome.fixedDt > 0.0f ? m_welcome.fixedDt : 1.0 / 128.0;
    m_accumulator += std::min(realSeconds, 0.25);
    std::uint32_t steps = 0;
    while (m_accumulator >= dt && steps < 16) {
        tickOnce(input);
        m_accumulator -= dt;
        ++steps;
    }
    if (steps == 16)
        m_accumulator = 0.0;
    return steps;
}

void NetClient::handleTransportEvent(const TransportEvent& event) {
    switch (event.type) {
    case TransportEventType::Connect: {
        m_serverPeerId = event.peerId;
        ClientHello hello;
        hello.playerName = m_playerName;
        sendPacket(hello.serialize());
        m_helloSent = true;
        break;
    }
    case TransportEventType::Receive:
        handlePacket(event.data.data(), event.data.size());
        break;
    case TransportEventType::Disconnect:
        if (m_connected) {
            m_connected = false;
            m_welcomed = false;
            if (m_callbacks.onDisconnected)
                m_callbacks.onDisconnected("connection closed by server");
        }
        break;
    case TransportEventType::None:
        break;
    }
}

void NetClient::handlePacket(const std::byte* data, std::size_t size) {
    if (data == nullptr || size == 0)
        return;
    const auto type = static_cast<NetPacketType>(static_cast<std::uint8_t>(data[0]));
    switch (type) {
    case NetPacketType::Welcome: {
        ServerWelcome welcome;
        if (ServerWelcome::deserialize(data, size, welcome)) {
            m_welcome = welcome;
            m_welcomed = true;
            m_predicted = PlayerMoveState{};
            m_predicted.grounded = true;
            if (m_callbacks.onWelcomed)
                m_callbacks.onWelcomed(m_welcome);
        }
        break;
    }
    case NetPacketType::Snapshot: {
        Snapshot snapshot;
        if (!m_welcomed || !Snapshot::deserialize(data, size, snapshot))
            break;
        if (m_lastSnapshotTick != 0 && !netTickNewer(snapshot.tick, m_lastSnapshotTick))
            break; // stale / reordered unreliable delivery
        m_lastSnapshotTick = snapshot.tick;
        m_lastAckSent = snapshot.tick;
        applySnapshot(m_world, snapshot);
        ++m_stats.snapshotsReceived;
        reconcile(snapshot);
        break;
    }
    case NetPacketType::GameEvent: {
        NetGameEvent event;
        if (NetGameEvent::deserialize(data, size, event)) {
            m_scriptEventQueue.push_back(event);
            if (m_callbacks.onGameEvent)
                m_callbacks.onGameEvent(event);
        }
        break;
    }
    case NetPacketType::StatusResponse: {
        NetStatus status;
        if (NetStatus::deserialize(data, size, status)) {
            status.pingMs = pingMs();
            m_lastStatus = status;
            if (m_callbacks.onStatus)
                m_callbacks.onStatus(status);
        }
        break;
    }
    case NetPacketType::ClientAction: {
        NetTextPacket packet;
        if (NetTextPacket::deserialize(data, size, NetPacketType::ClientAction, packet) && m_callbacks.onClientAction) {
            const std::size_t split = packet.text.find('\n');
            const std::string name = split == std::string::npos ? packet.text : packet.text.substr(0, split);
            const std::string payload = split == std::string::npos ? std::string{} : packet.text.substr(split + 1);
            m_callbacks.onClientAction(name, payload);
        }
        break;
    }
    case NetPacketType::Disconnect: {
        NetTextPacket packet;
        std::string reason = "disconnected by server";
        if (NetTextPacket::deserialize(data, size, NetPacketType::Disconnect, packet) && !packet.text.empty())
            reason = packet.text;
        m_connected = false;
        m_welcomed = false;
        if (m_callbacks.onDisconnected)
            m_callbacks.onDisconnected(reason);
        break;
    }
    default:
        break;
    }
}

void NetClient::reconcile(const Snapshot& snapshot) {
    // Anchor: this snapshot's authoritative copy of the local
    // player, when the delta included it. Deltas may omit an
    // unchanged local player — then the anchor from the last
    // snapshot that carried it remains the newest truth available.
    for (const EntitySnapshot& state : snapshot.entities) {
        if (state.entity == localEntityId()) {
            m_authoritative = authoritativeToMoveState(state, m_welcome.move);
            m_authoritativeInputTick = snapshot.lastInputTick;
            m_hasAuthoritative = true;
        }
    }
    if (!m_hasAuthoritative)
        return;

    // Where did prediction say we were, at the input the server
    // has just confirmed consuming?
    glm::vec3 predictedAtAnchor = m_authoritative.position;
    bool haveAnchorPrediction = false;
    for (const PredictedFrame& frame : m_predictedFrames) {
        if (frame.tick == m_authoritativeInputTick) {
            predictedAtAnchor = frame.after.position;
            haveAnchorPrediction = true;
            break;
        }
        if (netTickNewer(frame.tick, m_authoritativeInputTick))
            break;
    }
    const float error = haveAnchorPrediction ? glm::length(predictedAtAnchor - m_authoritative.position) : 0.0f;
    m_lastCorrectionDistance = error;
    ++m_stats.reconciliations;
    if (error > kMispredictThreshold) {
        ++m_stats.mispredictions;
        m_stats.lastCorrectionMeters = error;
    }

    // Rewind + replay every un-acked command on top of the truth.
    const glm::vec3 visualBefore = visualPosition();
    m_predicted = m_authoritative;
    const float dt = m_welcome.fixedDt > 0.0f ? m_welcome.fixedDt : 1.0f / 128.0f;
    for (const PredictedFrame& frame : m_predictedFrames) {
        if (netTickNewer(frame.tick, m_authoritativeInputTick))
            groundMove(m_predicted, frame.input, m_welcome.move, dt);
    }
    // Keep the render position continuous: carry the discrepancy
    // as an offset that decays exponentially. Teleports snap.
    m_visualOffset = error > kSnapDistance ? glm::vec3{0.0f} : visualBefore - m_predicted.position;
    if (glm::length(m_visualOffset) > 1.0f)
        m_visualOffset = glm::normalize(m_visualOffset);

    // Drop history the server has fully consumed.
    while (!m_predictedFrames.empty() && !netTickNewer(m_predictedFrames.front().tick, m_authoritativeInputTick) &&
           m_predictedFrames.front().tick != m_authoritativeInputTick)
        m_predictedFrames.pop_front();
}

glm::vec3 NetClient::visualPosition() const {
    return m_predicted.position + m_visualOffset;
}

std::optional<EntitySnapshot> NetClient::localAuthoritative() const {
    const auto it = m_world.find(localEntityId());
    if (it == m_world.end())
        return std::nullopt;
    return it->second;
}

std::uint32_t NetClient::pingMs() const {
    return m_serverPeerId != 0 ? m_transport.roundTripTime(m_serverPeerId) : 0;
}

void NetClient::requestStatus() {
    if (!m_connected)
        return;
    sendPacket(NetTextPacket{NetPacketType::StatusRequest, ""}.serialize());
}

bool NetClient::sendAction(const std::string& name, const std::string& payload) {
    if (!m_connected || !m_welcomed || name.empty() || name.size() > 64 ||
        name.find('\n') != std::string::npos || payload.size() > 512)
        return false;
    sendPacket(NetTextPacket{NetPacketType::ActionRequest, name + "\n" + payload}.serialize());
    return true;
}

void NetClient::sendPacket(const std::vector<std::byte>& packet) {
    if (packet.empty() || m_serverPeerId == 0)
        return;
    const auto type = static_cast<NetPacketType>(static_cast<std::uint8_t>(packet[0]));
    const std::uint8_t channel = channelOf(type);
    (void)m_transport.send(m_serverPeerId, packet.data(), packet.size(), channel, channel == 0);
}

std::size_t NetClient::forwardEventsToScripts(ScriptSystemContext& context) {
    std::size_t forwarded = 0;
    for (const NetGameEvent& event : m_scriptEventQueue) {
        DSLEvent dsl;
        switch (event.kind) {
        case NetGameEventKind::Trigger:
            dsl.kind = DSLEventKind::Trigger;
            break;
        case NetGameEventKind::Touch:
            dsl.kind = DSLEventKind::Touch;
            break;
        default:
            continue; // health/damage/death are data (worldState), not DSL events
        }
        dsl.target = event.text;
        ScriptSystem::postEvent(context, std::move(dsl));
        ++forwarded;
    }
    m_scriptEventQueue.clear();
    return forwarded;
}

void NetClient::applyToScene(Scene& scene) {
    auto& registry = scene.registry();
    // Erase mirrors whose entities left the world.
    for (auto it = m_mirrorEntities.begin(); it != m_mirrorEntities.end();) {
        if (m_world.find(it->first) == m_world.end() || !registry.valid(it->second)) {
            if (registry.valid(it->second))
                scene.destroyEntity(Entity{it->second, &scene});
            it = m_mirrorEntities.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto& [networkId, state] : m_world) {
        if (networkId == localEntityId())
            continue; // the local player renders from prediction
        auto it = m_mirrorEntities.find(networkId);
        entt::entity entity = entt::null;
        if (it == m_mirrorEntities.end() || !registry.valid(it->second)) {
            Entity created = scene.createEntity("NetMirror");
            created.addComponent<TransformComponent>();
            created.addComponent<NetMirrorComponent>(NetMirrorComponent{networkId});
            entity = created.handle();
            m_mirrorEntities[networkId] = entity;
        } else {
            entity = it->second;
        }
        auto& transform = registry.get<TransformComponent>(entity);
        transform.position = state.position;
        transform.rotationDegrees.y = state.yaw;
    }
}

} // namespace engine
