#pragma once
#include "Core/Export.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace engine {

// ---------------------------------------------------------------------------
// NetTransport: RAII wrapper over an ENet host (UDP transport).
//
// One class plays both roles: createServer() binds a port and
// accepts peers; createClient() + connect() dials out. Events are
// pumped into a caller-owned vector (cleared by the caller, never
// reallocated by the transport after warm-up), and outbound sends
// take raw buffers — the engine side of the hot loop performs no
// heap allocation per packet; ENet's own packet allocation is the
// only one, inside the library.
//
// Channels: 0 = reliable, 1 = unreliable (see NetProtocol::channelOf).
// Peer ids are transport-local, monotonically assigned uint32s.
// ---------------------------------------------------------------------------

enum class TransportEventType : std::uint8_t { None, Connect, Receive, Disconnect };

struct TransportEvent {
    TransportEventType type = TransportEventType::None;
    std::uint32_t peerId = 0;
    std::uint8_t channel = 0;
    std::vector<std::byte> data; // Receive only
};

class ENGINE_NET_API NetTransport {
public:
    NetTransport();
    ~NetTransport();
    NetTransport(const NetTransport&) = delete;
    NetTransport& operator=(const NetTransport&) = delete;

    [[nodiscard]] bool createServer(std::uint16_t port, std::size_t maxPeers);
    [[nodiscard]] bool createClient();
    // Client role: starts an asynchronous connect; completion
    // arrives as a Connect event from poll().
    [[nodiscard]] bool connect(const std::string& host, std::uint16_t port);
    void shutdown();

    [[nodiscard]] bool isServer() const { return m_isServer; }
    [[nodiscard]] bool active() const { return m_host != nullptr; }
    // Bound local port (meaningful for servers; port 0 at create
    // time means the OS assigned one — read it back here).
    [[nodiscard]] std::uint16_t localPort() const;

    // Appends pending events to `out` (not cleared). Waits at most
    // timeoutMs inside ENet's service call; 0 = non-blocking.
    void poll(std::vector<TransportEvent>& out, std::uint32_t timeoutMs = 0);

    [[nodiscard]] bool send(std::uint32_t peerId, const std::byte* data, std::size_t size, std::uint8_t channel,
                            bool reliable);
    void broadcast(const std::byte* data, std::size_t size, std::uint8_t channel, bool reliable,
                   std::uint32_t exceptPeerId = 0xFFFFFFFFu);
    void disconnectPeer(std::uint32_t peerId, bool flush = false);
    // Forces all queued packets/commands out onto the wire now
    // (used before teardown so a polite Disconnect is delivered).
    void flush();
    [[nodiscard]] std::uint32_t peerCount() const;
    [[nodiscard]] std::uint32_t roundTripTime(std::uint32_t peerId) const; // ms, 0 when unknown
    [[nodiscard]] bool hasPeer(std::uint32_t peerId) const;

private:
    void* m_host = nullptr; // ENetHost*
    bool m_isServer = false;
    std::uint32_t m_nextPeerId = 1;
    // ENetPeer* -> peerId, kept as opaque pointers to avoid
    // exposing ENet headers to consumers.
    std::vector<std::pair<void*, std::uint32_t>> m_peers;
};

} // namespace engine
