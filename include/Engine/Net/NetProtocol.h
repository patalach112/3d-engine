#pragma once
#include "Core/Export.h"
#include "Net/MovementSystem.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// Wire protocol shared by vibe_server and net clients.
//
// Two channel families over ENet:
//   * Channel 0 — RELIABLE: handshake, welcome, game events
//     (triggers / health / damage), status request+response,
//     script action requests, disconnects.
//   * Channel 1 — UNRELIABLE: input commands upstream, state
//     snapshots downstream (latest-state-wins; a dropped snapshot
//     is superseded by the next one).
//
// Packets are fixed little-endian structs; every packet begins with
// a NetPacketType byte. Tick numbers are uint32 and wrap (compare
// with netTickNewer).
//
// Bounded/zeroed-receiver rule: every decode validates length first
// and zero-fills absent optional fields — malformed packets are
// dropped, never partially applied.
// ---------------------------------------------------------------------------

enum class NetPacketType : std::uint8_t {
    Hello = 1,          // C->S reliable: handshake + player name
    Welcome = 2,        // S->C reliable: assigned entity, tick rate, rules
    InputCommand = 3,   // C->S unreliable: one move command (seq-stamped)
    Snapshot = 4,       // S->C unreliable: delta state snapshot
    GameEvent = 5,      // S->C reliable: trigger/health/damage event
    StatusRequest = 6,  // C->S reliable: remote-query API probe
    StatusResponse = 7, // S->C reliable: remote-query answer
    ActionRequest = 8,  // C->S reliable: .dsla/UI asks for an action
    ClientAction = 9,   // S->C reliable: server-approved client-local action
    Disconnect = 10,    // either way reliable: polite close + reason
};

enum class NetChannel : std::uint8_t {
    Reliable = 0,
    Unreliable = 1,
};

constexpr std::uint8_t kNetProtocolVersion = 1;
constexpr std::uint16_t kDefaultServerPort = 27015;
constexpr int kDefaultTickRate = 128;
constexpr int kDefaultMaxPlayers = 16;
[[nodiscard]] inline constexpr std::uint8_t channelOf(NetPacketType type) noexcept {
    return (type == NetPacketType::InputCommand || type == NetPacketType::Snapshot)
               ? static_cast<std::uint8_t>(NetChannel::Unreliable)
               : static_cast<std::uint8_t>(NetChannel::Reliable);
}

// Wrapping tick comparison: true when a is strictly newer than b.
[[nodiscard]] inline bool netTickNewer(std::uint32_t a, std::uint32_t b) noexcept {
    return static_cast<std::int32_t>(a - b) > 0;
}

namespace detail {
inline void writeU32(std::vector<std::byte>& out, std::uint32_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFu));
    out.push_back(static_cast<std::byte>((value >> 8u) & 0xFFu));
    out.push_back(static_cast<std::byte>((value >> 16u) & 0xFFu));
    out.push_back(static_cast<std::byte>((value >> 24u) & 0xFFu));
}
inline void writeU16(std::vector<std::byte>& out, std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFu));
    out.push_back(static_cast<std::byte>((value >> 8u) & 0xFFu));
}
inline void writeF32(std::vector<std::byte>& out, float value) {
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    writeU32(out, bits);
}
inline void writeI16(std::vector<std::byte>& out, std::int16_t value) {
    writeU16(out, static_cast<std::uint16_t>(value));
}
} // namespace detail

struct NetReader {
    const std::byte* data = nullptr;
    std::size_t size = 0;
    std::size_t cursor = 0;

    [[nodiscard]] bool ok() const { return cursor <= size; }
    [[nodiscard]] std::size_t remaining() const { return size - cursor; }
    std::uint8_t u8() {
        if (remaining() < 1) {
            cursor = size + 1;
            return 0;
        }
        return static_cast<std::uint8_t>(data[cursor++]);
    }
    std::uint16_t u16() {
        const std::uint16_t lo = u8();
        const std::uint16_t hi = u8();
        return static_cast<std::uint16_t>(lo | static_cast<std::uint16_t>(hi << 8u));
    }
    std::uint32_t u32() {
        std::uint32_t value = u8();
        value |= static_cast<std::uint32_t>(u8()) << 8u;
        value |= static_cast<std::uint32_t>(u8()) << 16u;
        value |= static_cast<std::uint32_t>(u8()) << 24u;
        return value;
    }
    std::int16_t i16() { return static_cast<std::int16_t>(u16()); }
    float f32() {
        const std::uint32_t bits = u32();
        float value = 0.0f;
        std::memcpy(&value, &bits, 4);
        return value;
    }
    std::string str(std::size_t maxLength) {
        const std::uint16_t length = u16();
        if (length > maxLength || remaining() < length) {
            cursor = size + 1;
            return {};
        }
        std::string out;
        out.resize(length);
        if (length > 0)
            std::memcpy(out.data(), data + cursor, length);
        cursor += length;
        return out;
    }
};

// --- Packets ---------------------------------------------------------------

struct ClientHello {
    std::string playerName;
    [[nodiscard]] ENGINE_NET_API std::vector<std::byte> serialize() const;
    [[nodiscard]] static ENGINE_NET_API bool deserialize(const std::byte* data, std::size_t size, ClientHello& out);
};

struct ServerWelcome {
    std::uint32_t entityId = 0; // server registry entity for this client
    std::uint32_t serverTick = 0;
    int tickRate = kDefaultTickRate;
    int maxPlayers = kDefaultMaxPlayers;
    float fixedDt = 1.0f / 128.0f;
    MoveSettings move; // the rules both sides simulate with
    std::string serverName;
    std::array<std::byte, 3> reserved{};
    [[nodiscard]] ENGINE_NET_API std::vector<std::byte> serialize() const;
    [[nodiscard]] static ENGINE_NET_API bool deserialize(const std::byte* data, std::size_t size, ServerWelcome& out);
};

struct InputCommand {
    std::uint32_t cmdNumber = 0; // == cmd.cmdNumber (reconcile key, echoed for framing)
    std::uint32_t lastAck = 0;   // last snapshot tick the client applied
    InputCmd cmd;                // the sequenced movement command
    [[nodiscard]] ENGINE_NET_API std::vector<std::byte> serialize() const;
    [[nodiscard]] static ENGINE_NET_API bool deserialize(const std::byte* data, std::size_t size, InputCommand& out);
};

// One entity's replicated state in a snapshot. Fields are full
// values; the delta layer (SnapshotState) decides which entities
// appear at all. hasHealth separates "no health component" from 0.
struct EntitySnapshot {
    std::uint32_t entity = 0;
    glm::vec3 position{0.0f};
    glm::vec3 velocity{0.0f};
    float yaw = 0.0f;
    std::int16_t health = 0;
    bool hasHealth = false;
    bool alive = true;
};

struct Snapshot {
    std::uint32_t tick = 0;       // server tick this state is authoritative at
    std::uint32_t baseline = 0;   // tick the delta was computed against (ack)
    std::uint32_t lastCmdNumber = 0; // last cmd_number the server consumed (reconcile)
    bool full = false;            // true => replace, do not merge
    std::vector<EntitySnapshot> entities;
    std::vector<std::uint32_t> removed; // entities deleted vs baseline
    [[nodiscard]] ENGINE_NET_API std::vector<std::byte> serialize() const;
    [[nodiscard]] static ENGINE_NET_API bool deserialize(const std::byte* data, std::size_t size, Snapshot& out);
};

enum class NetGameEventKind : std::uint8_t {
    Touch = 1,
    Trigger = 2,
    HealthChanged = 3,
    Damage = 4,
    Death = 5,
    Respawn = 6,
    Message = 7,
};

struct NetGameEvent {
    NetGameEventKind kind = NetGameEventKind::Message;
    std::uint32_t entity = 0; // subject (player)
    std::uint32_t other = 0;  // trigger zone / attacker
    std::int32_t value = 0;   // new health / damage amount
    std::uint32_t serverTick = 0;
    std::string text; // trigger name / message
    [[nodiscard]] ENGINE_NET_API std::vector<std::byte> serialize() const;
    [[nodiscard]] static ENGINE_NET_API bool deserialize(const std::byte* data, std::size_t size, NetGameEvent& out);
};

struct NetStatus {
    bool alive = false;
    std::uint32_t pingMs = 0;
    int playerCount = 0;
    int maxPlayers = 0;
    std::uint32_t serverTick = 0;
    int tickRate = 0;
    std::string serverName;
    [[nodiscard]] ENGINE_NET_API std::vector<std::byte> serialize() const;
    [[nodiscard]] static ENGINE_NET_API bool deserialize(const std::byte* data, std::size_t size, NetStatus& out);
};

// Generic request/response string packets (status + action requests).
struct NetTextPacket {
    NetPacketType type = NetPacketType::StatusRequest;
    std::string text;
    [[nodiscard]] ENGINE_NET_API std::vector<std::byte> serialize() const;
    [[nodiscard]] static ENGINE_NET_API bool deserialize(const std::byte* data, std::size_t size, NetPacketType type,
                                                          NetTextPacket& out);
};

} // namespace engine
