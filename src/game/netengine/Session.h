#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Packet.h"
#include "PacketRegistry.h"
#include "ServerEndpoint.h"
#include "ClientEndpoint.h"
#include "WorldInterfaces.h"
#include "WorldState.h"
#include "Ledger.h"

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
    //
    // Server-side admission: until a peer is admitted (immediately on an open
    // host, after a matching PID_AUTH on a protected one) every packet from it
    // except PID_AUTH is dropped — an unauthenticated client can neither inject
    // state nor read the world.
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
        // True once the server has welcomed our client (our peer id is final).
        // Before that, reliable requests (world state, ledger) are queued.
        bool IsWelcomed() const { return m_welcomed; }
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
        // Authorization is OPTIONAL and off by default: with no password the
        // session is open (co-op / session games) and peers are admitted on
        // connect without any extra round trip. Setting a password turns the
        // PID_AUTH gate on for the next StartHost().
        void SetHostPassword(const char* password);
        bool HasHostPassword() const { return m_hostPassword[0] != '\0'; }

        // Client-side password presented to the host on request. Set before the
        // host requires it; the session stores it and answers PID_AUTH demands.
        void SetPassword(const char* password) { SetClientPassword(password); }

        // Slot cap applied by the next StartHost() (the host's own loopback
        // client takes one slot). 0 = default (kDefaultMaxPeers).
        static constexpr const size_t kDefaultMaxPeers = 32;
        void SetMaxPeers(size_t n) { m_maxPeers = n ? n : kDefaultMaxPeers; }

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

        // ---- dedicated mission-progress channel (PID_MISSION_STATE) -------

        // The session asks this provider for the local mission-progress blob and
        // ships it to peers on its own (slower) cadence. May be null (idle).
        void SetMissionStateProvider(IMissionStateProvider* p) { m_missionState = p; }
        // The session delivers remote mission-progress blobs to this sink.
        using MissionStateFn = std::function<void(PeerId from, const uint8_t* data, uint16_t size)>;
        void SetMissionStateSink(MissionStateFn fn) { m_missionSink = std::move(fn); }

        // Broadcast cadence for mission progress, in Update frames: mission is sent
        // every m_missionSendEvery frames. Default 18 (≈ every 3rd player-state
        // send at the default 6-frame state cadence, ~3.3 Hz at 60 fps).
        void SetMissionSendEvery(unsigned n) { m_missionSendEvery = (n > 0) ? n : 1; }

        // ---- replicated world state (WorldState.h) -------------------------

        // Every authoritative change reaches this sink: remote changes,
        // confirmations/rejections of our own predictions, snapshot entries and
        // erasures. The game applies entry.value idempotently.
        using WorldStateFn = std::function<void(const WorldStateEntry& e, WorldStateReason reason, PeerId origin)>;
        void SetWorldStateSink(WorldStateFn fn) { m_worldSink = std::move(fn); }
        // Fired once the late-join snapshot has been fully received.
        using WorldSyncedFn = std::function<void()>;
        void SetWorldSyncedFn(WorldSyncedFn fn) { m_worldSynced = std::move(fn); }

        // Requests a change (the caller has already applied it locally). Queued
        // until WELCOME. Returns false outside a match or for a malformed op.
        bool RequestWorldState(const WorldStateOp& op);
        const WorldStateMirror& GetWorldStateMirror() const { return m_worldMirror; }
        const WorldStateTable&  GetWorldStateTable()  const { return m_worldTable; } // server side

        // ---- server-authoritative counters (Ledger.h) ----------------------

        // Server-side counter rules (host or dedicated server). Kept across
        // matches; configure before or after StartHost.
        void ConfigureCounter(CounterId counter, const CounterConfig& cfg) { m_ledgerServer.Configure(counter, cfg); }
        // Outcome of each of our own requests (roll back !accepted deltas).
        using LedgerFn = std::function<void(const LedgerOutcome&)>;
        void SetLedgerSink(LedgerFn fn) { m_ledgerSink = std::move(fn); }
        // Requests a predicted change. Queued until WELCOME.
        bool RequestLedgerChange(CounterId counter, uint8_t reason, int32_t delta, uint64_t claim);
        const LedgerClient& GetLedger() const { return m_ledgerClient; }
        const LedgerServer& GetLedgerServer() const { return m_ledgerServer; }

        // ---- owner-streamed dynamic entities (PID_ENTITY_STATE) ------------

        // Streams state for entities we own (their WS_CHANNEL_ENTITY record must
        // be ours; the server drops other records). Batched into packets.
        bool SendEntityStates(const std::vector<EntityStateRecord>& records);
        using EntityStateFn = std::function<void(PeerId owner, const EntityStateRecord& rec)>;
        void SetEntityStateSink(EntityStateFn fn) { m_entitySink = std::move(fn); }

        // ---- open user-data channel (0x8000+) -------------------------------

        // Callback for inbound custom events. `from` is the originating peer;
        // on a host session the event is relayed from a remote client, on a
        // joiner it comes straight from the server. Self-echo is filtered.
        using CustomEventFn = std::function<void(PeerId from, const uint8_t* data, uint16_t size)>;
        void SetCustomEventSink(CustomEventFn fn) { m_customEvent = std::move(fn); }

        // Ships an opaque payload to all other peers in the match (relayed
        // server-side). The engine never interprets the bytes. Returns false if
        // not in a match or the payload exceeds MAX_PACKET_PAYLOAD.
        bool SendCustomEvent(const uint8_t* data, uint16_t size);

    private:
        // Server-side packet / peer handling (shared by every host session).
        void OnServerPacket(const ServerEndpoint::RawPacket& rp);
        void OnServerPeerEvent(const PeerEvent& e);
        bool IsAdmitted(PeerId peer) const { return m_admitted.count(peer) != 0; }
        void AdmitPeer(PeerId peer);
        void SendWorldSnapshot(PeerId peer);
        void HandleWorldStateRequest(PeerId from, const uint8_t* data, uint16_t size);
        void HandleLedgerRequest(PeerId from, const uint8_t* data, uint16_t size);
        void RelayEntityStates(PeerId from, const uint8_t* data, uint16_t size);
        void Relay(PacketId id, PeerId from, const uint8_t* data, uint16_t size);

        // Client-side packet handling (host loopback and joiner alike).
        void OnClientPacket(const ClientEndpoint::RawPacket& rp);

        // Reliable client->server requests queued until WELCOME (bounded).
        void SendToServerWhenWelcomed(PacketId id, const uint8_t* data, uint16_t size);
        void FlushOutbox();

        // Sends the local player's state (from m_localState) to all peers.
        void SendLocalStateIfDue();
        // Delivers an inbound state packet to m_remoteSink.
        void HandleRemoteState(PeerId from, const uint8_t* data, uint16_t size);
        // Sends the local mission-progress blob (from m_missionState) to all
        // peers, on the slower frame-based mission cadence.
        void SendMissionStateIfDue();
        // Delivers an inbound mission-state packet to m_missionSink.
        void HandleRemoteMissionState(PeerId from, const uint8_t* data, uint16_t size);
        // Presents the client-side password to the host once the ENet peer is
        // up (and only once). No-op when absent or already sent.
        void SendAuthIfDue();
        // Transport instances (composed). A host holds a server + loopback client.
        std::unique_ptr<ServerEndpoint> m_server;
        std::unique_ptr<ClientEndpoint> m_client;
        PacketRegistry                  m_registry;

        PeerId       m_selfId   = 0;
        bool         m_welcomed = false; // WELCOME received on the client side
        uint32_t     m_worldSeed = 0;
        uint16_t     m_port     = 0;
        size_t       m_maxPeers = kDefaultMaxPeers;
        JoinLeaveFn  m_joinLeave;
        // Kept apart on purpose: a host is both server and (loopback) client, and
        // the client view excludes itself, so one shared set would "un-admit"
        // the host's own loopback peer on every PLAYER_LIST.
        std::set<PeerId> m_admitted;    // server: peers allowed in
        std::set<PeerId> m_peerRoster;  // client: the other peers in the match

        // Authentication / validation state.
        char         m_hostPassword[65] = { 0 };   // host-side required password ("" = open)
        char         m_clientPassword[65] = { 0 }; // client-side presented password
        bool         m_authSent = false;           // client-side PID_AUTH already delivered
        IStateValidator* m_validator = nullptr;    // optional state plausibility hook

        // World attachment (may be null when world not wired, e.g. dedicated relay).
        ILocalStateProvider* m_localState = nullptr;
        IRemoteStateSink*    m_remoteSink = nullptr;
        CustomEventFn        m_customEvent;   // open user-data channel sink
        unsigned             m_stateSendEvery = 6;
        unsigned             m_stateTick     = 0;
        uint8_t              m_stateScratch[MAX_PACKET_PAYLOAD];

        // Dedicated mission-progress channel (PID_MISSION_STATE).
        IMissionStateProvider* m_missionState = nullptr;
        MissionStateFn         m_missionSink;  // inbound mission-progress sink
        unsigned               m_missionSendEvery = 18; // update frames between mission sends
        unsigned               m_missionTick = 0;
        uint8_t                m_missionScratch[MAX_PACKET_PAYLOAD];

        // Replicated world state.
        WorldStateTable  m_worldTable;   // server: authoritative
        WorldStateMirror m_worldMirror;  // client: confirmed mirror + predictions
        WorldStateFn     m_worldSink;
        WorldSyncedFn    m_worldSynced;

        // Counters / economy.
        LedgerServer m_ledgerServer;
        LedgerClient m_ledgerClient;
        LedgerFn     m_ledgerSink;

        // Owner-streamed entities.
        EntityStateFn m_entitySink;

        // Client->server reliable requests waiting for WELCOME.
        struct Outgoing { PacketId id; std::vector<uint8_t> bytes; };
        static constexpr const size_t kMaxOutbox = 256;
        std::vector<Outgoing> m_outbox;

        // Client-side password setter (no-op on a host).
        void SetClientPassword(const char* password);
    };
}
