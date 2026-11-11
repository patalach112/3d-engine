#include "Net/NetTransport.h"

#include <enet/enet.h>

#include <atomic>
#include <mutex>
#include <utility>

namespace engine {
namespace {

// ENet is process-global; reference-count initialise/deinitialise
// across transports so servers and clients can coexist in one
// process (editor + tests do exactly that).
std::mutex g_enetMutex;
int g_enetUsers = 0;

bool acquireEnet() {
    const std::lock_guard<std::mutex> lock(g_enetMutex);
    if (g_enetUsers == 0 && enet_initialize() != 0)
        return false;
    ++g_enetUsers;
    return true;
}

void releaseEnet() {
    const std::lock_guard<std::mutex> lock(g_enetMutex);
    if (--g_enetUsers == 0)
        enet_deinitialize();
}

} // namespace

NetTransport::NetTransport() = default;

NetTransport::~NetTransport() {
    shutdown();
}

bool NetTransport::createServer(std::uint16_t port, std::size_t maxPeers) {
    shutdown();
    if (!acquireEnet())
        return false;
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;
    auto* host = enet_host_create(&address, maxPeers, 2, 0, 0);
    if (host == nullptr) {
        releaseEnet();
        return false;
    }
    m_host = host;
    m_isServer = true;
    return true;
}

bool NetTransport::createClient() {
    shutdown();
    if (!acquireEnet())
        return false;
    auto* host = enet_host_create(nullptr, 1, 2, 0, 0);
    if (host == nullptr) {
        releaseEnet();
        return false;
    }
    m_host = host;
    m_isServer = false;
    return true;
}

bool NetTransport::connect(const std::string& host, std::uint16_t port) {
    if (m_host == nullptr || m_isServer)
        return false;
    ENetAddress address;
    if (enet_address_set_host(&address, host.c_str()) != 0)
        return false;
    address.port = port;
    auto* self = static_cast<ENetHost*>(m_host);
    ENetPeer* peer = enet_host_connect(self, &address, 2, 0);
    if (peer == nullptr)
        return false;
    const std::uint32_t peerId = m_nextPeerId++;
    peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(peerId));
    m_peers.emplace_back(peer, peerId);
    return true;
}

void NetTransport::shutdown() {
    if (m_host == nullptr)
        return;
    auto* host = static_cast<ENetHost*>(m_host);
    for (auto& [peerPtr, peerId] : m_peers) {
        (void)peerId;
        enet_peer_disconnect_now(static_cast<ENetPeer*>(peerPtr), 0);
    }
    m_peers.clear();
    enet_host_destroy(host);
    m_host = nullptr;
    m_isServer = false;
    releaseEnet();
}

std::uint16_t NetTransport::localPort() const {
    if (m_host == nullptr)
        return 0;
    return static_cast<const ENetHost*>(m_host)->address.port;
}

void NetTransport::poll(std::vector<TransportEvent>& out, std::uint32_t timeoutMs) {
    if (m_host == nullptr)
        return;
    auto* host = static_cast<ENetHost*>(m_host);
    ENetEvent event;
    int result = enet_host_service(host, &event, timeoutMs);
    while (result > 0) {
        auto peerIdOf = [this](ENetPeer* peer) {
            return static_cast<std::uint32_t>(reinterpret_cast<std::uintptr_t>(peer->data));
        };
        switch (event.type) {
        case ENET_EVENT_TYPE_CONNECT: {
            std::uint32_t peerId = peerIdOf(event.peer);
            if (peerId == 0) { // inbound server-side peer: assign now
                peerId = m_nextPeerId++;
                event.peer->data = reinterpret_cast<void*>(static_cast<std::uintptr_t>(peerId));
                m_peers.emplace_back(event.peer, peerId);
            }
            out.push_back(TransportEvent{TransportEventType::Connect, peerId, 0, {}});
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE: {
            TransportEvent received;
            received.type = TransportEventType::Receive;
            received.peerId = peerIdOf(event.peer);
            received.channel = event.channelID;
            received.data.assign(reinterpret_cast<const std::byte*>(event.packet->data),
                                 reinterpret_cast<const std::byte*>(event.packet->data) + event.packet->dataLength);
            out.push_back(std::move(received));
            enet_packet_destroy(event.packet);
            break;
        }
        case ENET_EVENT_TYPE_DISCONNECT: {
            const std::uint32_t peerId = peerIdOf(event.peer);
            out.push_back(TransportEvent{TransportEventType::Disconnect, peerId, 0, {}});
            std::erase_if(m_peers, [peer = static_cast<void*>(event.peer)](const auto& entry) {
                return entry.first == peer;
            });
            event.peer->data = nullptr;
            break;
        }
        case ENET_EVENT_TYPE_NONE:
            break;
        }
        result = enet_host_service(host, &event, 0);
    }
}

bool NetTransport::send(std::uint32_t peerId, const std::byte* data, std::size_t size, std::uint8_t channel,
                        bool reliable) {
    if (m_host == nullptr)
        return false;
    for (auto& [peerPtr, id] : m_peers) {
        if (id != peerId)
            continue;
        ENetPacket* packet = enet_packet_create(data, size, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
        if (packet == nullptr)
            return false;
        if (enet_peer_send(static_cast<ENetPeer*>(peerPtr), channel, packet) != 0) {
            enet_packet_destroy(packet);
            return false;
        }
        return true;
    }
    return false;
}

void NetTransport::broadcast(const std::byte* data, std::size_t size, std::uint8_t channel, bool reliable,
                             std::uint32_t exceptPeerId) {
    if (m_host == nullptr)
        return;
    auto* host = static_cast<ENetHost*>(m_host);
    if (exceptPeerId == 0xFFFFFFFFu) {
        ENetPacket* packet = enet_packet_create(data, size, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
        if (packet != nullptr)
            enet_host_broadcast(host, channel, packet);
        return;
    }
    for (auto& [peerPtr, id] : m_peers) {
        if (id == exceptPeerId)
            continue;
        ENetPacket* packet = enet_packet_create(data, size, reliable ? ENET_PACKET_FLAG_RELIABLE : 0);
        if (packet != nullptr && enet_peer_send(static_cast<ENetPeer*>(peerPtr), channel, packet) != 0)
            enet_packet_destroy(packet);
    }
}

void NetTransport::disconnectPeer(std::uint32_t peerId, bool flush) {
    for (auto& [peerPtr, id] : m_peers) {
        if (id != peerId)
            continue;
        auto* peer = static_cast<ENetPeer*>(peerPtr);
        if (flush) {
            enet_peer_disconnect(peer, 0);
        } else {
            enet_peer_disconnect_later(peer, 0);
        }
        return;
    }
}

void NetTransport::flush() {
    if (m_host != nullptr)
        enet_host_flush(static_cast<ENetHost*>(m_host));
}

std::uint32_t NetTransport::peerCount() const {
    return static_cast<std::uint32_t>(m_peers.size());
}

std::uint32_t NetTransport::roundTripTime(std::uint32_t peerId) const {
    for (const auto& [peerPtr, id] : m_peers) {
        if (id == peerId)
            return static_cast<const ENetPeer*>(peerPtr)->roundTripTime;
    }
    return 0;
}

bool NetTransport::hasPeer(std::uint32_t peerId) const {
    for (const auto& [peerPtr, id] : m_peers) {
        (void)peerPtr;
        if (id == peerId)
            return true;
    }
    return false;
}

} // namespace engine
