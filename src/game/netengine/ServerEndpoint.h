#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ENetInit.h"
#include "Packet.h"
#include "PacketRegistry.h"

#include <enet/enet.h>

#include <cstdint>
#include <map>
#include <functional>
#include <memory>

namespace net
{
    // Transport-side connection event, forwarded to the session layer. An
    // endpoint only knows about peers and packet delivery — it has no idea what
    // a peer "means" in the game world.
    struct PeerEvent
    {
        PeerId   peer;     // assigned id (server) or opaque (client)
        bool     connected; // true=joined, false=left
    };

    using PeerEventFn = std::function<void(const PeerEvent&)>;

    // net::ServerEndpoint
    // -------------------
    // Pure ENet host on a server that:
    //   - owns a single ENetHost (many peers),
    //   - assigns a PeerId to every accepted peer,
    //   - routes inbound packets to the bound PacketRegistry,
    //   - reports connect/disconnect via an optional PeerEventFn,
    //   - guarantees liveness (timeout sweep).
    //
    // It deliberately knows nothing about world/players/missions. Keeping the
    // transport dumb like this is what makes the layer reusable as an engine
    // API: higher layers (Session, game, mods) attach their own handlers.
    class ServerEndpoint
    {
    public:
        ServerEndpoint();
        ~ServerEndpoint();

        ServerEndpoint(const ServerEndpoint&) = delete;
        ServerEndpoint& operator=(const ServerEndpoint&) = delete;

        // Binds and starts listening. Returns false if the port is busy.
        bool    Start(uint16_t port, uint16_t maxPeers);
        void    Stop();
        void    Update();

        bool    IsRunning() const { return m_host != nullptr; }
        uint16_t GetPort() const { return m_port; }

        // Payload delivered to handlers (already unmasked from framing).
        struct RawPacket
        {
            PacketId id;
            PeerId   from;
            const uint8_t* data;   // points into the net::Packet backing buffer
            uint16_t size;         // payload byte count
        };
        using RawPacketFn = std::function<void(const RawPacket&)>;

        // Registers a sink that receives every successfully framed inbound
        // packet. The session layer uses it to feed its PacketRegistry.
        void SetPacketSink(RawPacketFn fn) { m_packetSink = std::move(fn); }

        void SetPeerEventFn(PeerEventFn fn) { m_peerEvent = std::move(fn); }

        // Liveness: a peer that has not delivered any packet (or been seen by
        // ENet events) within this many milliseconds is dropped by a sweep in
        // Update(). The timeout is transport-level: an idle peer is considered
        // gone regardless of what it means in the game. Pass 0 to restore the
        // default window. ENet itself also has a liveness timeout, but we keep
        // an explicit app-visible sweep so the session layer can always observe
        // the drop via m_peerEvent(connected=false).
        void SetClientTimeoutMs(unsigned int ms) { m_clientTimeoutMs = ms ? ms : kDefaultClientTimeoutMs; }

        // True while a peer with the given id is connected and has not timed out.
        bool HasPeer(PeerId id) const { return m_peersById.find(id) != m_peersById.end(); }

        // Forcibly disconnects a peer by id (e.g. a rejected authenticator).
        // Emits PeerEvent{id,false} and clears its liveness/session state.
        void KickPeer(PeerId id);

        // On-wire framing bounds.
        static constexpr int kChannel = 0;
        static constexpr enet_uint32 kFlushAfterService = 1; // service+flush per Update

        // Transport-level heartbeat cadence. The endpoint sends a PING probe to
        // every peer every `everay` Update() calls; a responding peer is heard
        // on the wire and so stays alive. Callers wire the actual PONG handling
        // through the packet sink; the endpoint only originates the probe.
        void SetPingInterval(unsigned everyNUpdates);

        // How many milliseconds the server keeps a peer with no inbound activity
        // before considering it dead. Defaults to kDefaultClientTimeoutMs.
        static constexpr unsigned kDefaultClientTimeoutMs = 30000;

        // Sends a framed packet (id + payloadSize + payload) to one peer.
        // Returns false on failure (unknown peer, oversize, host gone).
        bool SendTo(PeerId peer, PacketId id, const uint8_t* payload, uint16_t payloadSize);

        // Sends to every peer except `omit` (0 = none omitted).
        void Broadcast(PacketId id, const uint8_t* payload, uint16_t payloadSize, PeerId omit);

    private:
        void OnService(ENetHost* host);

        net::ENetInit        m_enetInit;
        ENetHost*            m_host        = nullptr;
        uint16_t             m_port        = 0;
        uint16_t             m_nextPeerId  = 0;
        bool                 m_running     = false;
        RawPacketFn          m_packetSink;
        PeerEventFn          m_peerEvent;
        std::map<ENetPeer*, PeerId> m_peerIds;
        std::map<PeerId, ENetPeer*> m_peersById;

        // Per-peer liveness bookkeeping (transport-level timeout sweep).
        struct PeerLiveness
        {
            enet_uint32 lastSeenMs = 0;   // last inbound packet OR connect time
        };
        std::map<PeerId, PeerLiveness> m_liveness;
        unsigned int                   m_clientTimeoutMs = kDefaultClientTimeoutMs;

        // Heartbeat state: send a PING probe to every peer every Nth Update.
        unsigned                      m_pingEvery = 6;   // ~10 Hz at 60fps
        unsigned                      m_pingTick  = 0;

        // Staging buffer for framed inbound (id+size+payload) so sinks can hold
        // a stable pointer across the dispatch call.
        uint8_t              m_decodeBuf[MAX_PACKET_PAYLOAD + 4];
    };
}