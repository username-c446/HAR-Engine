#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ENetInit.h"
#include "Packet.h"
#include "PacketRegistry.h"
#include "NetTime.h"

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

        // ---- ping / RTT / server clock --------------------------------------

        // Sends a PING {client time} now. The server answers PONG {echo, server
        // time}; each answer feeds the RTT and the server-clock estimate.
        void SendPing();

        // Automatic heartbeat period (default 500 ms). The heartbeat also keeps
        // the server's liveness window fresh. 0 = ping on every Update().
        void SetPingIntervalMs(unsigned intervalMs) { m_pingIntervalMs = intervalMs; }

        // True once at least one PING->PONG round trip has completed.
        bool     HasMeasuredPing() const { return m_hasPing; }
        // Smoothed round-trip time in milliseconds (0 before the first PONG).
        unsigned GetRttMs() const { return m_clock.RttMs(); }

        // Server clock estimate (ms, the server's own epoch). Valid once
        // IsClockSynced(); seeded by the session from WELCOME, refined by PONGs.
        bool     IsClockSynced() const { return m_clock.IsSynced(); }
        uint32_t ServerTimeMs() const { return m_clock.ServerTime(enet_time_get()); }
        void     SeedServerClock(uint32_t serverTimeMs) { m_clock.SeedFromWelcome(serverTimeMs, enet_time_get()); }

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

        // Heartbeat and clock estimate.
        ClockSync            m_clock;
        bool                 m_hasPing        = false;
        unsigned             m_pingIntervalMs = 500;
        enet_uint32          m_lastPingMs     = 0;
    };
}