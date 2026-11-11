#pragma once
#include "Core/Export.h"

#include "Net/NetComponents.h"
#include "Net/NetProtocol.h"
#include "Net/NetTransport.h"
#include "Net/SnapshotDelta.h"
#include "Scene/Scene.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

namespace engine {

struct ScriptSystemContext; // Scripting/ScriptSystem.h (EngineScene)


// ---------------------------------------------------------------------------
// NetClient: the client half of the Source-style model.
//
// Strictly non-authoritative. The client:
//   * sends one InputCommand per local tick (unreliable channel),
//   * PREDICTS its own movement immediately with the identical
//     ground-friction controller the server runs (zero input lag),
//   * RECONCILES on every snapshot: rewind to the authoritative
//     state at the last input the server consumed, replay the
//     un-acked commands, and expose the divergence as a decaying
//     visual offset so corrections glide instead of snapping,
//   * merges delta snapshots into a read-only world mirror.
//
// Gameplay truth (health, damage, triggers) arrives only as
// server GameEvents / snapshot fields. The remote-query API —
// requestStatus() and sendAction() — is the whole client->server
// surface besides input: the server answers status queries and
// runs ONLY editor/game-registered action handlers.
// ---------------------------------------------------------------------------

struct NetClientStats {
    std::uint64_t snapshotsReceived = 0;
    std::uint64_t reconciliations = 0;   // rewinds performed
    std::uint64_t mispredictions = 0;    // rewinds where prediction had visibly drifted
    std::uint64_t inputsSent = 0;
    float lastCorrectionMeters = 0.0f;   // divergence at the most recent reconcile
};

class ENGINE_NET_API NetClient {
public:
    struct Callbacks {
        std::function<void(const ServerWelcome&)> onWelcomed;
        std::function<void(const NetGameEvent&)> onGameEvent;
        // Server-approved action replies (presentation only).
        std::function<void(const std::string& name, const std::string& payload)> onClientAction;
        std::function<void(const NetStatus&)> onStatus;
        std::function<void(const std::string& reason)> onDisconnected;
    };

    NetClient();
    ~NetClient();
    NetClient(const NetClient&) = delete;
    NetClient& operator=(const NetClient&) = delete;

    [[nodiscard]] bool connect(const std::string& host, std::uint16_t port, const std::string& playerName);
    void disconnect(const std::string& reason = "client disconnect");
    [[nodiscard]] bool connected() const { return m_connected; }
    [[nodiscard]] bool welcomed() const { return m_welcomed; }
    [[nodiscard]] const ServerWelcome& welcome() const { return m_welcome; }
    [[nodiscard]] std::uint32_t localEntityId() const { return m_welcome.entityId; }
    [[nodiscard]] std::uint32_t tick() const { return m_tick; }

    // One deterministic client tick: pump transport, then (once
    // welcomed) predict + send one input command. Tests and fixed-
    // rate hosts drive this directly; update() wraps it in an
    // accumulator for render-rate hosts.
    // Stamps `input` with the next cmd_number, predicts it, sends it.
    void tickOnce(const InputCmd& input);
    std::uint32_t update(double realSeconds, const InputCmd& input);

    // Predicted local state (what gameplay-feel uses this frame).
    [[nodiscard]] const PlayerState& predictedState() const { return m_predicted; }
    // Render position: predicted + smoothly decaying correction.
    [[nodiscard]] glm::vec3 visualPosition() const;
    [[nodiscard]] float correctionDistance() const { return m_lastCorrectionDistance; }

    // Read-only authoritative mirror (remote entities + own
    // entity's last authoritative state).
    [[nodiscard]] const std::unordered_map<std::uint32_t, EntitySnapshot>& worldState() const { return m_world; }
    [[nodiscard]] std::optional<EntitySnapshot> localAuthoritative() const;
    [[nodiscard]] const NetClientStats& stats() const { return m_stats; }
    [[nodiscard]] std::uint32_t pingMs() const;

    // --- Remote-query API (the secure client->server surface) -----
    // Asks the server for its status; the answer arrives through
    // Callbacks::onStatus and lastStatus().
    void requestStatus();
    [[nodiscard]] const std::optional<NetStatus>& lastStatus() const { return m_lastStatus; }
    // Requests a named server action with a text payload. The
    // server executes only registered handlers; the reply (if
    // accepted) arrives via Callbacks::onClientAction.
    [[nodiscard]] bool sendAction(const std::string& name, const std::string& payload = {});

    // Mirrors remote entities into a client Scene (Transform +
    // Health-driven alive state), creating/destroying mirror
    // entities as the world changes. The local player is NOT
    // mirrored (use visualPosition() for it). Apply once per frame.
    void applyToScene(Scene& scene);

    // .dsla bridge: drains the game events received since the
    // last call into a client-side script context as DSL events
    // (server Trigger -> on_trigger("zone"), Touch -> on_touch),
    // so client scripts can react for PRESENTATION (HUD, sound)
    // to authoritative outcomes. DSL events carry no authority:
    // nothing a client script does here can mutate gameplay.
    // Returns the number of events forwarded.
    std::size_t forwardEventsToScripts(ScriptSystemContext& context);

    void setCallbacks(Callbacks callbacks) { m_callbacks = std::move(callbacks); }

private:
    struct PredictedFrame {
        InputCmd cmd{};    // carries its cmdNumber (the sequence key)
        PlayerState after{}; // predicted state right after simulating cmd
    };

    void handleTransportEvent(const TransportEvent& event);
    void handlePacket(const std::byte* data, std::size_t size);
    void reconcile(const Snapshot& snapshot);
    void sendPacket(const std::vector<std::byte>& packet);

    NetTransport m_transport;
    Callbacks m_callbacks;
    NetClientStats m_stats;
    ServerWelcome m_welcome;

    bool m_connected = false;
    bool m_welcomed = false;
    bool m_helloSent = false;
    std::uint32_t m_serverPeerId = 0;
    std::string m_playerName;

    std::uint32_t m_tick = 0;
    std::uint32_t m_lastAckSent = 0;      // highest snapshot tick applied (echoed upstream)
    std::uint32_t m_lastSnapshotTick = 0; // staleness guard for unreliable delivery
    double m_accumulator = 0.0;

    MovementSystem m_movement; // settings arrive with the Welcome packet
    PlayerState m_predicted{};
    glm::vec3 m_visualOffset{0.0f};
    float m_lastCorrectionDistance = 0.0f;
    std::deque<PredictedFrame> m_predictedFrames;

    // Last authoritative state received for the local player plus
    // the input tick it corresponds to (reconcile anchor).
    bool m_hasAuthoritative = false;
    PlayerState m_authoritative{};
    std::uint32_t m_authoritativeCmdNumber = 0;

    std::vector<NetGameEvent> m_scriptEventQueue; // for forwardEventsToScripts
    std::unordered_map<std::uint32_t, EntitySnapshot> m_world;
    std::unordered_map<std::uint32_t, entt::entity> m_mirrorEntities;
    std::optional<NetStatus> m_lastStatus;

    std::vector<TransportEvent> m_transportEvents;
};

} // namespace engine
