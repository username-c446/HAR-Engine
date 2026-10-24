#include "ClientEndpoint.h"

#include <cstring>

namespace net
{
    namespace
    {
        // Framing constants must agree with Packet.cpp / ServerEndpoint.cpp.
        constexpr const uint16_t kHeaderBytes = sizeof(PacketId) + sizeof(uint16_t);
    }

    ClientEndpoint::ClientEndpoint() = default;
    ClientEndpoint::~ClientEndpoint() { Disconnect(); }

    bool ClientEndpoint::Connect(const char* serverIp, uint16_t serverPort)
    {
        if (m_connected || m_host) return false;

        m_host = enet_host_create(nullptr, 1, CH_COUNT, 0, 0);
        if (!m_host) return false;

        m_serverIp = serverIp ? serverIp : "";
        m_serverPort = serverPort;

        ENetAddress addr;
        if (enet_address_set_host(&addr, serverIp) != 0)
        {
            enet_host_destroy(m_host);
            m_host = nullptr;
            return false;
        }
        addr.port = serverPort;

        m_peer = enet_host_connect(m_host, &addr, CH_COUNT, 0);
        if (!m_peer)
        {
            enet_host_destroy(m_host);
            m_host = nullptr;
            return false;
        }

        m_connected = true;
        m_peerReady = false;
        m_hasPing = false;
        m_lastPingMs = 0;
        m_clock.Reset();
        return true;
    }

    void ClientEndpoint::Disconnect()
    {
        if (m_host)
        {
            // disconnect_now ships the disconnect command immediately. A queued
            // enet_peer_disconnect would die with the host below, leaving the
            // server to notice the departure only by timeout.
            if (m_peer) enet_peer_disconnect_now(m_peer, 0);
            enet_host_destroy(m_host);
            m_host = nullptr;
            m_peer = nullptr;
        }
        m_connected = false;
        m_peerReady = false;
    }

    void ClientEndpoint::Update()
    {
        if (!m_host) return;

        // Heartbeat: keeps the server's liveness window fresh and samples the
        // RTT / server clock.
        if (m_peerReady && m_peer != nullptr)
        {
            enet_uint32 now = enet_time_get();
            if (m_lastPingMs == 0 || now - m_lastPingMs >= m_pingIntervalMs)
                SendPing();
        }

        ENetEvent ev;
        while (enet_host_service(m_host, &ev, 0) > 0)
        {
            switch (ev.type)
            {
                case ENET_EVENT_TYPE_CONNECT:
                    m_peerReady = true;
                    ConfigureLink(ev.peer);
                    break;
                case ENET_EVENT_TYPE_RECEIVE:
                {
                    if (ev.packet->dataLength >= kHeaderBytes)
                    {
                        uint16_t pid = static_cast<uint16_t>(ev.packet->data[0])
                                     | static_cast<uint16_t>(ev.packet->data[1]) << 8;
                        uint16_t size = static_cast<uint16_t>(ev.packet->data[2])
                                      | static_cast<uint16_t>(ev.packet->data[3]) << 8;
                        if (static_cast<uint32_t>(kHeaderBytes) + size <= ev.packet->dataLength)
                        {
                            const uint8_t* body = ev.packet->data + kHeaderBytes;
                            if (pid == PID_PONG)
                            {
                                // {u32 our send time echoed, u32 server time}
                                Reader r(body, size);
                                uint32_t sent = 0, serverTime = 0;
                                if (r.ReadU32(sent) && r.ReadU32(serverTime))
                                {
                                    m_clock.OnPong(sent, serverTime, enet_time_get());
                                    m_hasPing = true;
                                }
                            }
                            else if (m_packetSink)
                            {
                                RawPacket rp;
                                rp.id   = pid;
                                rp.data = body;
                                rp.size = size;
                                m_packetSink(rp);
                            }
                        }
                    }
                    enet_packet_destroy(ev.packet);
                    break;
                }
                case ENET_EVENT_TYPE_DISCONNECT:
                    m_peerReady = false;
                    break;
                default:
                    break;
            }
        }

        enet_host_flush(m_host);
    }

    bool ClientEndpoint::SendTo(PacketId id, const uint8_t* payload, uint16_t payloadSize)
    {
        if (!m_host || !m_peer || payloadSize > MAX_PACKET_PAYLOAD) return false;

        // Delivery follows the packet id (Packet.h ChannelOf): reliable-ordered
        // for commands and world state, unreliable-sequenced for streams.
        Channel ch = ChannelOf(id);
        ENetPacket* pkt = enet_packet_create(nullptr, kHeaderBytes + payloadSize,
                                             ch == CH_RELIABLE ? ENET_PACKET_FLAG_RELIABLE : 0);
        if (!pkt) return false;
        uint8_t* d = pkt->data;
        d[0] = static_cast<uint8_t>(id & 0xFF);
        d[1] = static_cast<uint8_t>((id >> 8) & 0xFF);
        d[2] = static_cast<uint8_t>(payloadSize & 0xFF);
        d[3] = static_cast<uint8_t>((payloadSize >> 8) & 0xFF);
        if (payloadSize > 0) std::memcpy(d + kHeaderBytes, payload, payloadSize);
        if (enet_peer_send(m_peer, ch, pkt) != 0)
        {
            enet_packet_destroy(pkt);
            return false;
        }
        return true;
    }

    void ClientEndpoint::SendPing()
    {
        if (!m_host || !m_peer) return;
        m_lastPingMs = enet_time_get();
        if (m_lastPingMs == 0) m_lastPingMs = 1; // 0 means "never pinged"
        uint8_t buf[4];
        Writer w(buf, sizeof(buf));
        w.WriteU32(m_lastPingMs);
        SendTo(PID_PING, buf, sizeof(buf));
        enet_host_flush(m_host);
    }
}
