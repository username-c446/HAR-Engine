#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ENetInit.h"
#include "Packet.h"
#include "PacketRegistry.h"

#include <enet/enet.h>

#include <cstdint>
#include <functional>
#include <string>

namespace net
{
    // net::ClientEndpoint
    // -------------------
    // Pure ENet transport as a single peer connecting to a ServerEndpoint.
    // Mirrors the server transport: it owns one ENetHost+peer, decodes the
    // same [id][size][payload] framing, and delivers to a packet sink. It knows
    // nothing about sessions/world — those live in net::Session on top.
    class ClientEndpoint
    {
    public:
        ClientEndpoint();
        ~ClientEndpoint();

        ClientEndpoint(const ClientEndpoint&) = delete;
        ClientEndpoint& operator=(const ClientEndpoint&) = delete;

        // Connects and starts pumping. Returns true if the ENet connect attempt
        // was initiated (the session layer observes the handshake via events).
        bool    Connect(const char* serverIp, uint16_t serverPort);
        void    Disconnect();
        void    Update();

        bool    IsConnected() const { return m_connected; }
        bool    IsServerReachable() const { return m_peer != nullptr; }

        struct RawPacket
        {
            PacketId id;
            const uint8_t* data; // into staging buffer
            uint16_t size;
        };
        using RawPacketFn = std::function<void(const RawPacket&)>;
        // Set before Connect; receives every inbound packet from the server.
        void SetPacketSink(RawPacketFn fn) { m_packetSink = std::move(fn); }

        // Sends a framed packet to the server. Returns false on failure.
        bool SendTo(PacketId id, const uint8_t* payload, uint16_t payloadSize);

        // True once the server has accepted the connection (ENET connect seen).
        bool IsPeerReady() const { return m_peerReady; }
        // Clears ready state when (re)connecting.
        void ResetPeerReady() { m_peerReady = false; }

        // ---- ping / RTT measurement -----------------------------------------

        // Sends a PING probe to the server now (used both for heartbeat and
        // RTT). The server answers with PONG; the endpoint records the
        // round-trip time measured from the sent timestamp to the PONG.
        void SendPing();

        // Automatic heartbeat: sends a PING every Nth Update() (each auto/PING
        // keeps the server's liveness window fresh and measures RTT). Default
        // every 60th Update (~1 Hz at 60fps). Pass 0 to ping on every Update,
        // or use SendPing() directly for manual control.
        void SetPingIntervalMs(unsigned intervalUpdateCount)
        { m_pingEvery = intervalUpdateCount ? intervalUpdateCount : 1; }

        // True once at least one PING->PONG round-trip has completed.
        bool HasMeasuredPing() const { return m_hasPing; }
        // Last measured round-trip time in milliseconds (0 if never measured).
        unsigned GetPingMs() const { return m_pingMs; }

    private:
        net::ENetInit        m_enetInit;
        ENetHost*            m_host  = nullptr;
        ENetPeer*            m_peer  = nullptr;
        std::string          m_serverIp;
        uint16_t             m_serverPort = 0;
        bool                 m_connected = false;
        bool                 m_peerReady = false;
        RawPacketFn          m_packetSink;
        uint8_t              m_decodeBuf[MAX_PACKET_PAYLOAD + 4];

        // Ping/RTT state: last PING sent-at timestamp (ms) and measured RTT.
        enet_uint32          m_pingSentMs   = 0;
        unsigned             m_pingMs       = 0;
        bool                 m_hasPing      = false;
        unsigned             m_pingEvery    = 60;  // auto heartbeat cadence (Update calls)
        unsigned             m_pingTick     = 0;
    };
}