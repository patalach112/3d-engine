#include "Net/NetProtocol.h"

#include <algorithm>

namespace engine {
namespace {

void writeHeader(std::vector<std::byte>& out, NetPacketType type) {
    out.clear();
    out.push_back(static_cast<std::byte>(type));
}

bool readHeader(const std::byte* data, std::size_t size, NetPacketType type, NetReader& reader) {
    if (data == nullptr || size < 1)
        return false;
    reader.data = data;
    reader.size = size;
    reader.cursor = 0;
    return reader.u8() == static_cast<std::uint8_t>(type);
}

void writeString(std::vector<std::byte>& out, std::string_view text, std::size_t maxLength) {
    const std::size_t length = std::min(text.size(), maxLength);
    detail::writeU16(out, static_cast<std::uint16_t>(length));
    for (std::size_t i = 0; i < length; ++i)
        out.push_back(static_cast<std::byte>(text[i]));
}

} // namespace

// --- ClientHello -----------------------------------------------------------

std::vector<std::byte> ClientHello::serialize() const {
    std::vector<std::byte> out;
    out.reserve(3 + playerName.size());
    writeHeader(out, NetPacketType::Hello);
    writeString(out, playerName, 64);
    return out;
}

bool ClientHello::deserialize(const std::byte* data, std::size_t size, ClientHello& out) {
    NetReader reader;
    if (!readHeader(data, size, NetPacketType::Hello, reader))
        return false;
    out.playerName = reader.str(64);
    return reader.ok();
}

// --- ServerWelcome -----------------------------------------------------------

std::vector<std::byte> ServerWelcome::serialize() const {
    std::vector<std::byte> out;
    out.reserve(64 + serverName.size());
    writeHeader(out, NetPacketType::Welcome);
    detail::writeU32(out, entityId);
    detail::writeU32(out, serverTick);
    detail::writeU32(out, static_cast<std::uint32_t>(tickRate));
    detail::writeU16(out, static_cast<std::uint16_t>(maxPlayers));
    detail::writeF32(out, fixedDt);
    detail::writeF32(out, move.maxSpeed);
    detail::writeF32(out, move.friction);
    detail::writeF32(out, move.stopSpeed);
    detail::writeF32(out, move.accelerate);
    detail::writeF32(out, move.airAccelerate);
    detail::writeF32(out, move.gravity);
    detail::writeF32(out, move.jumpSpeed);
    detail::writeF32(out, move.groundY);
    detail::writeF32(out, move.sprintSpeed);
    detail::writeF32(out, move.staminaDrainPerSecond);
    detail::writeF32(out, move.staminaRegenPerSecond);
    writeString(out, serverName, 64);
    return out;
}

bool ServerWelcome::deserialize(const std::byte* data, std::size_t size, ServerWelcome& out) {
    NetReader reader;
    if (!readHeader(data, size, NetPacketType::Welcome, reader))
        return false;
    out.entityId = reader.u32();
    out.serverTick = reader.u32();
    out.tickRate = static_cast<int>(reader.u32());
    out.maxPlayers = reader.u16();
    out.fixedDt = reader.f32();
    out.move.maxSpeed = reader.f32();
    out.move.friction = reader.f32();
    out.move.stopSpeed = reader.f32();
    out.move.accelerate = reader.f32();
    out.move.airAccelerate = reader.f32();
    out.move.gravity = reader.f32();
    out.move.jumpSpeed = reader.f32();
    out.move.groundY = reader.f32();
    out.move.sprintSpeed = reader.f32();
    out.move.staminaDrainPerSecond = reader.f32();
    out.move.staminaRegenPerSecond = reader.f32();
    out.serverName = reader.str(64);
    return reader.ok();
}

// --- InputCommand ------------------------------------------------------------

std::vector<std::byte> InputCommand::serialize() const {
    std::vector<std::byte> out;
    out.reserve(23);
    writeHeader(out, NetPacketType::InputCommand);
    detail::writeU32(out, cmdNumber);
    detail::writeU32(out, lastAck);
    detail::writeF32(out, cmd.forward);
    detail::writeF32(out, cmd.strafe);
    out.push_back(static_cast<std::byte>(cmd.jump ? 1 : 0));
    detail::writeF32(out, cmd.yawDegrees);
    out.push_back(static_cast<std::byte>(cmd.sprint ? 1 : 0));
    return out;
}

bool InputCommand::deserialize(const std::byte* data, std::size_t size, InputCommand& out) {
    NetReader reader;
    if (!readHeader(data, size, NetPacketType::InputCommand, reader))
        return false;
    out.cmdNumber = reader.u32();
    out.lastAck = reader.u32();
    out.cmd.forward = reader.f32();
    out.cmd.strafe = reader.f32();
    out.cmd.jump = reader.u8() != 0;
    out.cmd.yawDegrees = reader.f32();
    out.cmd.sprint = reader.u8() != 0;
    out.cmd.cmdNumber = out.cmdNumber;
    return reader.ok();
}

// --- Snapshot ------------------------------------------------------------------

std::vector<std::byte> Snapshot::serialize() const {
    std::vector<std::byte> out;
    out.reserve(16 + entities.size() * 36 + removed.size() * 4);
    writeHeader(out, NetPacketType::Snapshot);
    detail::writeU32(out, tick);
    detail::writeU32(out, baseline);
    detail::writeU32(out, lastCmdNumber);
    out.push_back(static_cast<std::byte>(full ? 1 : 0));
    detail::writeU16(out, static_cast<std::uint16_t>(std::min<std::size_t>(entities.size(), 0xFFFF)));
    for (const EntitySnapshot& entity : entities) {
        detail::writeU32(out, entity.entity);
        detail::writeF32(out, entity.position.x);
        detail::writeF32(out, entity.position.y);
        detail::writeF32(out, entity.position.z);
        detail::writeF32(out, entity.velocity.x);
        detail::writeF32(out, entity.velocity.y);
        detail::writeF32(out, entity.velocity.z);
        detail::writeF32(out, entity.yaw);
        detail::writeI16(out, entity.health);
        std::uint8_t flags = 0;
        if (entity.hasHealth)
            flags |= 1u;
        if (entity.alive)
            flags |= 2u;
        out.push_back(static_cast<std::byte>(flags));
    }
    detail::writeU16(out, static_cast<std::uint16_t>(std::min<std::size_t>(removed.size(), 0xFFFF)));
    for (const std::uint32_t entity : removed)
        detail::writeU32(out, entity);
    return out;
}

bool Snapshot::deserialize(const std::byte* data, std::size_t size, Snapshot& out) {
    NetReader reader;
    if (!readHeader(data, size, NetPacketType::Snapshot, reader))
        return false;
    out.tick = reader.u32();
    out.baseline = reader.u32();
    out.lastCmdNumber = reader.u32();
    out.full = reader.u8() != 0;
    const std::uint16_t count = reader.u16();
    // Every entity record is 35 bytes; reject counts the packet
    // cannot possibly contain before reserving anything.
    if (reader.remaining() < static_cast<std::size_t>(count) * 35)
        return false;
    out.entities.clear();
    out.entities.reserve(count);
    for (std::uint16_t i = 0; i < count; ++i) {
        EntitySnapshot entity;
        entity.entity = reader.u32();
        entity.position.x = reader.f32();
        entity.position.y = reader.f32();
        entity.position.z = reader.f32();
        entity.velocity.x = reader.f32();
        entity.velocity.y = reader.f32();
        entity.velocity.z = reader.f32();
        entity.yaw = reader.f32();
        entity.health = reader.i16();
        const std::uint8_t flags = reader.u8();
        entity.hasHealth = (flags & 1u) != 0;
        entity.alive = (flags & 2u) != 0;
        out.entities.push_back(entity);
    }
    const std::uint16_t removedCount = reader.u16();
    if (reader.remaining() < static_cast<std::size_t>(removedCount) * 4)
        return false;
    out.removed.clear();
    out.removed.reserve(removedCount);
    for (std::uint16_t i = 0; i < removedCount; ++i)
        out.removed.push_back(reader.u32());
    return reader.ok();
}

// --- NetGameEvent ----------------------------------------------------------------

std::vector<std::byte> NetGameEvent::serialize() const {
    std::vector<std::byte> out;
    out.reserve(20 + text.size());
    writeHeader(out, NetPacketType::GameEvent);
    out.push_back(static_cast<std::byte>(kind));
    detail::writeU32(out, entity);
    detail::writeU32(out, other);
    detail::writeU32(out, static_cast<std::uint32_t>(value));
    detail::writeU32(out, serverTick);
    writeString(out, text, 256);
    return out;
}

bool NetGameEvent::deserialize(const std::byte* data, std::size_t size, NetGameEvent& out) {
    NetReader reader;
    if (!readHeader(data, size, NetPacketType::GameEvent, reader))
        return false;
    const std::uint8_t kindByte = reader.u8();
    if (kindByte < static_cast<std::uint8_t>(NetGameEventKind::Touch) ||
        kindByte > static_cast<std::uint8_t>(NetGameEventKind::Message))
        return false;
    out.kind = static_cast<NetGameEventKind>(kindByte);
    out.entity = reader.u32();
    out.other = reader.u32();
    out.value = static_cast<std::int32_t>(reader.u32());
    out.serverTick = reader.u32();
    out.text = reader.str(256);
    return reader.ok();
}

// --- NetStatus -------------------------------------------------------------------

std::vector<std::byte> NetStatus::serialize() const {
    std::vector<std::byte> out;
    out.reserve(24 + serverName.size());
    writeHeader(out, NetPacketType::StatusResponse);
    out.push_back(static_cast<std::byte>(alive ? 1 : 0));
    detail::writeU32(out, pingMs);
    detail::writeU16(out, static_cast<std::uint16_t>(playerCount));
    detail::writeU16(out, static_cast<std::uint16_t>(maxPlayers));
    detail::writeU32(out, serverTick);
    detail::writeU16(out, static_cast<std::uint16_t>(tickRate));
    writeString(out, serverName, 64);
    return out;
}

bool NetStatus::deserialize(const std::byte* data, std::size_t size, NetStatus& out) {
    NetReader reader;
    if (!readHeader(data, size, NetPacketType::StatusResponse, reader))
        return false;
    out.alive = reader.u8() != 0;
    out.pingMs = reader.u32();
    out.playerCount = reader.u16();
    out.maxPlayers = reader.u16();
    out.serverTick = reader.u32();
    out.tickRate = reader.u16();
    out.serverName = reader.str(64);
    return reader.ok();
}

// --- NetTextPacket -----------------------------------------------------------------

std::vector<std::byte> NetTextPacket::serialize() const {
    std::vector<std::byte> out;
    out.reserve(3 + text.size());
    writeHeader(out, type);
    writeString(out, text, 1024);
    return out;
}

bool NetTextPacket::deserialize(const std::byte* data, std::size_t size, NetPacketType type, NetTextPacket& out) {
    NetReader reader;
    if (!readHeader(data, size, type, reader))
        return false;
    out.type = type;
    out.text = reader.str(1024);
    return reader.ok();
}

} // namespace engine
