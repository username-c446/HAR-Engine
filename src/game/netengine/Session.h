#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Packet.h"
#include "PacketRegistry.h"
#include "ServerEndpoint.h"
#include "ClientEndpoint.h"
#include "WorldInterfaces.h"

#include <cstdint>
#include <memory>
#include <vector>
#include <set>
#include <limits>

namespace net
{
    // net::Session
    // ------------
    // The match orchestrator. It owns the transport endpoints and a packet
    // registry, and exposes the match lifecycle (host/join/leave). Crucially it
    // does NOT know about Avatar / Character / world objects — the world is
    // attached from outside through small interfaces. That is what makes the
    // network layer reusable as a standalone engine subsystem.
    //
    // A session is either a Host (owns a ServerEndpoint + its own loopback
    // client) or a Joiner (one ClientEndpoint). It cannot switch modes without
    // Leave() first.
    class Session
    {
    public:
        Session();
        ~Session();

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        // Unity access for the host process. A single session object lives for
        // the lifetime of the game (mirrors the old NetworkMatchManager
        // singleton so consumers can ask `Session::Get()->IsInMatch()` without
        // owning a pointer). Not part of the reusable engine API itself; the
        // engine works with a constructed Session instance.
        static Session* Create();
        static Session* Get();
        static void     Destroy();

        // ---- lifecycle -----------------------------------------------------

        // Bring up a host on `port`. The host also creates a loopback client so
        // the local player participates like any other peer. Returns false if a
        // match is already active or the port is taken.
        bool StartHost(uint16_t port, uint32_t worldSeed);

        // Join an existing host. Returns false if a match is already active.
        bool Join(const char* serverIp, uint16_t port);

        // Leave any active match (host or join). Idempotent.
        void Leave();

        // Pump both endpoints and deliver inbound packets to their handlers.
        // Call once per game frame.
        void Update();

        // ---- match state ---------------------------------------------------

        bool IsInMatch() const { return m_server != nullptr || m_client != nullptr; }
        bool IsHost()   const { return m_server != nullptr; }
        bool IsJoined() const { return m_client != nullptr && m_client->IsConnected(); }
        PeerId GetLocalPeerId() const { return m_selfId; }
        uint32_t GetWorldSeed() const { return m_worldSeed; }

        // Blood metro config: the set of peers currently in the match, NOT
        // including ourselves (the server-authoritative roster). Host and
        // joiner both keep it.
        const std::set<PeerId>& GetPeerRoster() const { return m_peerRoster; }

        // ---- authentication / protection ------------------------------------

        // Password-protected host: clients must present the matching password
        // (PID_AUTH) before the host welcomes them. A host with a password does
        // NOT send WELCOME/WORLD_SEED until the client authenticates. Joiner
        // side must call SetPassword() with the host's password before/after
        // Join(), and presents it automatically once the host asks for it.
        // An unset password leaves the host open (no auth required).
        void SetHostPassword(const char* password);
        bool HasHostPassword() const { return m_hostPassword[0] != '\0'; }

        // Client-side password presented to the host on request. Set before the
        // host requires it; the session stores it and answers PID_AUTH demands.
        void SetPassword(const char* password) { SetClientPassword(password); }

        // Server-authoritative validation hook for relayed state. If set, every
        // inbound player state from a remote peer is passed through the
        // validator BEFORE being broadcast; a rejected state is dropped (the
        // last accepted state stays authoritative). Usable to implement
        // anti-teleport / plausibility checks without coupling the session to
        // the game's world types.
        void SetStateValidator(IStateValidator* v) { m_validator = v; }

        // Session-level lifecycle events surfaced to the game, so the caller
        // :=ws without reaching into ENet or the registry.
        using JoinLeaveFn = std::function<void(PeerId, bool /*joined*/)>;
        void SetJoinLeaveFn(JoinLeaveFn fn) { m_joinLeave = std::move(fn); }

        // ---- world attachment (keeps the session world-agnostic) ----------

        // The session asks this provider for the local player's current state
        // every broadcast tick and ships it to peers. May be null (idle).
        void SetLocalStateProvider(ILocalStateProvider* p) { m_localState = p; }
        // The session delivers every remote player's state blob to this sink.
        void SetRemoteStateSink(IRemoteStateSink* s) { m_remoteSink = s; }

        // Broadcast cadence for local state. Default every 6th Update (~10 Hz).
        void SetStateSendEvery(unsigned n) { m_stateSendEvery = (n > 0) ? n : 1; }

    private:
        // Sends the local player's state (from m_localState) to all peers.
        void SendLocalStateIfDue();
        // Delivers an inbound state packet to m_remoteSink.
        void HandleRemoteState(PeerId from, const uint8_t* data, uint16_t size);
        // Presents the client-side password to the host once the ENet peer is
        // up (and only once). No-op when absent or already sent.
        void SendAuthIfDue();
        // Transport instances (composed). A host holds a server + loopback client.
        std::unique_ptr<ServerEndpoint> m_server;
        std::unique_ptr<ClientEndpoint> m_client;
        PacketRegistry                  m_registry;

        PeerId       m_selfId   = 0;
        uint32_t     m_worldSeed = 0;
        uint16_t     m_port     = 0;
        JoinLeaveFn  m_joinLeave;
        std::set<PeerId> m_peerRoster;  // server-authoritative roster (self excluded)

        // Authentication / validation state.
        char         m_hostPassword[65] = { 0 };   // host-side required password ("" = open)
        char         m_clientPassword[65] = { 0 }; // client-side presented password
        bool         m_authSent = false;           // client-side PID_AUTH already delivered
        std::set<PeerId> m_authenticated;          // host-side: peers that passed auth
        IStateValidator* m_validator = nullptr;    // optional state plausibility hook

        // World attachment (may be null when world not wired, e.g. dedicated relay).
        ILocalStateProvider* m_localState = nullptr;
        IRemoteStateSink*    m_remoteSink = nullptr;
        unsigned             m_stateSendEvery = 6;
        unsigned             m_stateTick     = 0;
        uint8_t              m_stateScratch[MAX_PACKET_PAYLOAD];

        // Client-side password setter (no-op on a host).
        void SetClientPassword(const char* password);
    };
}