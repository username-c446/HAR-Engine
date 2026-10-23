#include "ServerEndpoint.h"

#include <cstring>

namespace net
{
    namespace
    {
        // Framing constants must agree with Packet.cpp.
        constexpr const uint16_t kHeaderBytes = sizeof(PacketId) + sizeof(uint16_t); // id + size
    }

    ServerEndpoint::ServerEndpoint() = default;
    ServerEndpoint::~ServerEndpoint() { Stop(); }

    bool ServerEndpoint::Start(uint16_t port, uint16_t maxPeers)
    {
        if (m_host) return false;
        if (maxPeers == 0) maxPeers = 1;

        ENetAddress addr;
        addr.host = ENET_HOST_ANY;
        addr.port = port;

        m_host = enet_host_create(&addr, maxPeers, 1, 0, 0);
        if (!m_host) return false;

        m_port = port;
        m_nextPeerId = 1; // 0 reserved as "not a peer"
        m_peerIds.clear();
        m_peersById.clear();
        m_liveness.clear();
        m_running = true;
        return true;
    }

    void ServerEndpoint::Stop()
    {
        if (m_host)
        {
            enet_host_destroy(m_host);
            m_host = nullptr;
        }
        m_running = false;
        m_port = 0;
        m_peerIds.clear();
        m_peersById.clear();
        m_liveness.clear();
    }

    void ServerEndpoint::SetPingInterval(unsigned everyNUpdates)
    {
        m_pingEvery = everyNUpdates ? everyNUpdates : 1;
    }

    void ServerEndpoint::Update()
    {
        if (!m_host) return;

        ENetEvent ev;
        while (enet_host_service(m_host, &ev, 0) > 0)
        {
            switch (ev.type)
            {
                case ENET_EVENT_TYPE_CONNECT:
                {
                    PeerId id = m_nextPeerId++;
                    m_peerIds[ev.peer] = id;
                    m_peersById[id] = ev.peer;
                    m_liveness[id].lastSeenMs = enet_time_get();
                    if (m_peerEvent) m_peerEvent({ id, true });
                    break;
                }
                case ENET_EVENT_TYPE_RECEIVE:
                {
                    // Any inbound packet proves the peer is alive.
                    auto lit = m_peerIds.find(ev.peer);
                    if (lit != m_peerIds.end())
                        m_liveness[lit->second].lastSeenMs = enet_time_get();

                    // Frame: [id u16][size u16][payload size bytes]
                    if (ev.packet->dataLength >= kHeaderBytes)
                    {
                        uint16_t pid, size;
                        pid  = static_cast<uint16_t>(ev.packet->data[0])
                             | static_cast<uint16_t>(ev.packet->data[1]) << 8;
                        size = static_cast<uint16_t>(ev.packet->data[2])
                             | static_cast<uint16_t>(ev.packet->data[3]) << 8;
                        if (static_cast<uint32_t>(kHeaderBytes) + size <= ev.packet->dataLength)
                        {
                            auto it = m_peerIds.find(ev.peer);
                            PeerId from = (it != m_peerIds.end()) ? it->second : 0;
                            if (pid == PID_PING)
                            {
                                // Heartbeat probe: answer with an empty PONG so
                                // the client can measure RTT and stay alive.
                                SendTo(from, PID_PONG, nullptr, 0);
                            }
                            else if (m_packetSink)
                            {
                                RawPacket rp;
                                rp.id   = pid;
                                rp.from = from;
                                rp.data = ev.packet->data + kHeaderBytes;
                                rp.size = size;
                                m_packetSink(rp);
                            }
                        }
                        // else: malformed frame, drop silently (protocol tolerant).
                    }
                    enet_packet_destroy(ev.packet);
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT:
                {
                    auto it = m_peerIds.find(ev.peer);
                    if (it != m_peerIds.end())
                    {
                        PeerId id = it->second;
                        if (m_peerEvent) m_peerEvent({ id, false });
                        m_peersById.erase(id);
                        m_liveness.erase(id);
                        m_peerIds.erase(it);
                    }
                    break;
                }
                default:
                    break;
            }
        }

        // Liveness sweep: drop a peer that has not been heard from within the
        // window. Mirrors the old server's timeout behaviour so silent clients
        // do not linger forever; the session layer observes each drop via
        // m_peerEvent(connected=false). A peer's liveness is refreshed only by
        // ITS OWN inbound packets (see RECEIVE above) — the server never
        // originates probes, so a truly silent client is eventually dropped.
        if (!m_liveness.empty())
        {
            enet_uint32 nowMs = enet_time_get();
            for (auto it = m_liveness.begin(); it != m_liveness.end();)
            {
                if (nowMs - it->second.lastSeenMs > m_clientTimeoutMs)
                {
                    PeerId dead = it->first;
                    ENetPeer* deadPeer = nullptr;
                    auto pit = m_peersById.find(dead);
                    if (pit != m_peersById.end())
                    {
                        deadPeer = pit->second;
                        m_peersById.erase(pit);
                    }
                    it = m_liveness.erase(it);
                    m_peerIds.erase(deadPeer);
                    if (m_peerEvent) m_peerEvent({ dead, false });
                    if (deadPeer) enet_peer_disconnect(deadPeer, 0);
                }
                else
                {
                    ++it;
                }
            }
        }

        if (kFlushAfterService) enet_host_flush(m_host);
    }

    bool ServerEndpoint::SendTo(PeerId peer, PacketId id, const uint8_t* payload, uint16_t payloadSize)
    {
        if (!m_host || payloadSize > MAX_PACKET_PAYLOAD) return false;
        auto it = m_peersById.find(peer);
        if (it == m_peersById.end()) return false;

        ENetPacket* pkt = enet_packet_create(nullptr, kHeaderBytes + payloadSize,
                                             ENET_PACKET_FLAG_RELIABLE);
        if (!pkt) return false;
        uint8_t* d = pkt->data;
        d[0] = static_cast<uint8_t>(id & 0xFF);
        d[1] = static_cast<uint8_t>((id >> 8) & 0xFF);
        d[2] = static_cast<uint8_t>(payloadSize & 0xFF);
        d[3] = static_cast<uint8_t>((payloadSize >> 8) & 0xFF);
        std::memcpy(d + kHeaderBytes, payload, payloadSize);
        enet_peer_send(it->second, kChannel, pkt);
        return true;
    }

    void ServerEndpoint::KickPeer(PeerId id)
    {
        auto pit = m_peersById.find(id);
        if (pit == m_peersById.end()) return; // already gone

        ENetPeer* peer = pit->second;
        // Reflect the removal to the session layer before disconnecting so
        // downstream state (roster, world shadow) drops it immediately.
        if (m_peerEvent) m_peerEvent({ id, false });
        m_peersById.erase(pit);
        m_liveness.erase(id);
        m_peerIds.erase(peer);
        enet_peer_disconnect(peer, 1);
        // The disconnect event will land in the next Update() as a DISCONNECT;
        // since we already removed the mapping, that handler is a safe no-op.
    }

    void ServerEndpoint::Broadcast(PacketId id, const uint8_t* payload, uint16_t payloadSize, PeerId omit)
    {
        if (!m_host) return;
        for (auto& kv : m_peersById)
        {
            if (kv.first == omit) continue;
            SendTo(kv.first, id, payload, payloadSize);
        }
    }
}