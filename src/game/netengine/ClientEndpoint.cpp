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

        m_host = enet_host_create(nullptr, 1, 1, 0, 0);
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

        m_peer = enet_host_connect(m_host, &addr, 1, 0);
        if (!m_peer)
        {
            enet_host_destroy(m_host);
            m_host = nullptr;
            return false;
        }

        m_connected = true;
        m_peerReady = false;
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

        // Automatic heartbeat: every N Updates, originate a PING probe. This
        // keeps the server's liveness window fresh (a responding client is
        // never dropped) and measures RTT on each PONG. Guests that set a tiny
        // interval (or 0) effectively heartbeat every frame.
        if (m_peerReady && m_peer != nullptr && ++m_pingTick >= m_pingEvery)
        {
            m_pingTick = 0;
            SendPing();
        }

        ENetEvent ev;
        while (enet_host_service(m_host, &ev, 0) > 0)
        {
            switch (ev.type)
            {
                case ENET_EVENT_TYPE_CONNECT:
                    m_peerReady = true;
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
                            if (pid == PID_PING)
                            {
                                // Server heartbeat probe: answer so the server
                                // sees our liveness and we keep the channel warm.
                                SendTo(PID_PONG, nullptr, 0);
                            }
                            else if (pid == PID_PONG)
                            {
                                // Round-trip complete: record the measured RTT.
                                if (m_pingSentMs != 0)
                                {
                                    enet_uint32 nowMs = enet_time_get();
                                    m_pingMs   = static_cast<unsigned>(nowMs - m_pingSentMs);
                                    m_hasPing  = true;
                                    m_pingSentMs = 0;
                                }
                                // Also route to the packet sink so the session
                                // layer can observe the PONG if it cares.
                                if (m_packetSink)
                                {
                                    RawPacket rp;
                                    rp.id   = pid;
                                    rp.data = ev.packet->data + kHeaderBytes;
                                    rp.size = size;
                                    m_packetSink(rp);
                                }
                            }
                            else if (m_packetSink)
                            {
                                RawPacket rp;
                                rp.id   = pid;
                                rp.data = ev.packet->data + kHeaderBytes;
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

        ENetPacket* pkt = enet_packet_create(nullptr, kHeaderBytes + payloadSize,
                                             ENET_PACKET_FLAG_RELIABLE);
        if (!pkt) return false;
        uint8_t* d = pkt->data;
        d[0] = static_cast<uint8_t>(id & 0xFF);
        d[1] = static_cast<uint8_t>((id >> 8) & 0xFF);
        d[2] = static_cast<uint8_t>(payloadSize & 0xFF);
        d[3] = static_cast<uint8_t>((payloadSize >> 8) & 0xFF);
        if (payloadSize > 0) std::memcpy(d + kHeaderBytes, payload, payloadSize);
        enet_peer_send(m_peer, 0, pkt);
        return true;
    }

    void ClientEndpoint::SendPing()
    {
        if (!m_host || !m_peer) return;
        m_pingSentMs = enet_time_get();
        SendTo(PID_PING, nullptr, 0);
        enet_host_flush(m_host);
    }
}