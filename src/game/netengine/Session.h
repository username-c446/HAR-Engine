#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Packet.h"
#include "ClientEndpoint.h"
#include "ServerCore.h"
#include "WorldInterfaces.h"
#include "WorldState.h"
#include "Ledger.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <vector>

namespace net
{
    // net::Session
    // ------------
    // A participant in a match. It is always a CLIENT of a server; a hosting
    // session additionally runs the server in-process (an embedded ServerCore,
    // the very same server the dedicated server product runs) and joins it
    // through a loopback client like any other peer. So host and joiner share
    // one client code path, and a hosted match behaves exactly like one on a
    // dedicated server.
    //
    // It knows nothing about Avatar / Character / world objects: the world is
    // attached from outside through small interfaces and sinks.
    //
    // Connection: PID_HELLO (protocol + game version + optional password) ->
    // WELCOME (peer id, server clock, session options) or PID_REFUSE (reason).
    class Session
    {
    public:
        Session();
        ~Session();

        Session(const Session&) = delete;
        Session& operator=(const Session&) = delete;

        // ---- lifecycle -----------------------------------------------------

        // Hosts on `port`: starts the embedded server, then joins it through a
        // loopback client. False if a match is active or the port is taken.
        bool StartHost(uint16_t port, uint32_t worldSeed);

        // Joins a server (a hosting game or a dedicated server).
        bool Join(const char* serverIp, uint16_t port);

        // Leaves any active match (stops the embedded server). Idempotent.
        void Leave();

        // Pumps the server (when hosting) and the client. Once per frame.
        void Update();

        // ---- match state ---------------------------------------------------

        bool IsInMatch() const { return m_core.IsRunning() || m_client != nullptr; }
        bool IsHost()   const { return m_core.IsRunning(); }
        bool IsJoined() const { return m_client != nullptr && m_client->IsConnected(); }
        PeerId GetLocalPeerId() const { return m_selfId; }
        // True once the server has welcomed our client (our peer id is final).
        // Before that, reliable requests (world state, ledger, events) queue.
        bool IsWelcomed() const { return m_welcomed; }
        uint32_t GetWorldSeed() const { return m_worldSeed; }

        // The other peers in the match (server-authoritative roster).
        const std::set<PeerId>& GetPeerRoster() const { return m_peerRoster; }

        // Why the server refused us (REFUSE_NONE if it did not). A refused
        // session has left the match.
        RefuseReason GetRefusal() const { return m_refusal; }
        // True when an established connection dropped (server gone / kicked
        // without a reason). The caller should Leave().
        bool IsConnectionLost() const { return m_connectionLost; }

        // ---- time ------------------------------------------------------------

        // Server clock estimate (ms), seeded by WELCOME, refined by PING/PONG.
        bool     IsClockSynced() const { return m_client && m_client->IsClockSynced(); }
        uint32_t ServerTimeMs() const { return m_client ? m_client->ServerTimeMs() : 0; }
        uint32_t GetRttMs() const { return m_client ? m_client->GetRttMs() : 0; }

        // ---- admission / server configuration ----------------------------------

        // Optional authorization (off by default): with no password a hosted
        // session is open (co-op / session games). Applies to the next StartHost.
        void SetHostPassword(const char* password) { m_core.SetPassword(password); }
        bool HasHostPassword() const { return m_core.HasPassword(); }

        // Password presented in PID_HELLO when joining a protected server.
        void SetPassword(const char* password);

        // Game content version: presented when joining and, when hosting,
        // required from joiners (0 = not checked).
        void SetGameVersion(uint32_t v) { m_gameVersion = v; m_core.SetGameVersion(v); }

        // Slot cap of a hosted session (the host's own loopback takes one slot).
        static constexpr const size_t kDefaultMaxPeers = ServerCore::kDefaultMaxPeers;
        void SetMaxPeers(size_t n) { m_core.SetMaxPeers(n); }

        // Server-side plausibility gate for relayed player state (hosting).
        void SetStateValidator(IStateValidator* v) { m_core.SetStateValidator(v); }

        // The embedded server (hosting): configure counters, rate limits,
        // admission filter, options... Not running while joined elsewhere.
        ServerCore&       GetServerCore() { return m_core; }
        const ServerCore& GetServerCore() const { return m_core; }

        // Peers joining / leaving, as seen by THIS client (from the server's
        // player list; same on host and joiner). Our own id is never reported.
        using JoinLeaveFn = std::function<void(PeerId, bool /*joined*/)>;
        void SetJoinLeaveFn(JoinLeaveFn fn) { m_joinLeave = std::move(fn); }

        // Session options announced by the server (SessionOption bits).
        uint32_t GetSessionOptions() const { return m_sessionOptions; }
        // Hosting: changes the options for everybody.
        void SetSessionOptions(uint32_t options) { m_core.SetSessionOptions(options); }
        using SessionOptionsFn = std::function<void(uint32_t options)>;
        void SetSessionOptionsFn(SessionOptionsFn fn) { m_sessionOptionsFn = std::move(fn); }

        // ---- player state stream (PID_PLAYER_STATE) -----------------------------

        void SetLocalStateProvider(ILocalStateProvider* p) { m_localState = p; }
        void SetRemoteStateSink(IRemoteStateSink* s) { m_remoteSink = s; }
        // Send period of the local state (default 100 ms; 0 = every Update).
        void SetStateSendIntervalMs(uint32_t ms) { m_stateIntervalMs = ms; }
        // The server's validator refused our state: snap the local player to
        // this blob (same format as the state stream).
        using StateCorrectionFn = std::function<void(const uint8_t* data, uint16_t size)>;
        void SetStateCorrectionSink(StateCorrectionFn fn) { m_correctionSink = std::move(fn); }

        // ---- mission progress stream (PID_MISSION_STATE) -------------------------

        void SetMissionStateProvider(IMissionStateProvider* p) { m_missionState = p; }
        using MissionStateFn = std::function<void(PeerId from, const uint8_t* data, uint16_t size)>;
        void SetMissionStateSink(MissionStateFn fn) { m_missionSink = std::move(fn); }
        // Send period of the mission progress (default 300 ms; 0 = every Update).
        void SetMissionSendIntervalMs(uint32_t ms) { m_missionIntervalMs = ms; }

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
        const WorldStateTable&  GetWorldStateTable()  const { return m_core.GetWorldStateTable(); } // hosting

        // ---- server-authoritative counters (Ledger.h) ----------------------

        // Server-side counter rules of a hosted session (kept across matches).
        void ConfigureCounter(CounterId counter, const CounterConfig& cfg) { m_core.ConfigureCounter(counter, cfg); }
        // Outcome of each of our own requests (roll back !accepted deltas).
        using LedgerFn = std::function<void(const LedgerOutcome&)>;
        void SetLedgerSink(LedgerFn fn) { m_ledgerSink = std::move(fn); }
        // Requests a predicted change. Queued until WELCOME.
        bool RequestLedgerChange(CounterId counter, uint8_t reason, int32_t delta, uint64_t claim);
        const LedgerClient& GetLedger() const { return m_ledgerClient; }
        const LedgerServer& GetLedgerServer() const { return m_core.GetLedgerServer(); }

        // ---- owner-streamed dynamic entities (PID_ENTITY_STATE) ------------

        // Streams state for entities we own (their WS_CHANNEL_ENTITY record must
        // be ours; the server drops other records). Batched, timestamped.
        bool SendEntityStates(const std::vector<EntityStateRecord>& records);
        using EntityStateFn = std::function<void(PeerId owner, const EntityStateRecord& rec, uint32_t serverTimeMs)>;
        void SetEntityStateSink(EntityStateFn fn) { m_entitySink = std::move(fn); }

        // ---- events aimed at an entity / another player ------------------------

        // A hit / impulse / damage on an entity someone else owns. The server
        // delivers it to the owner only. Payload <= kMaxEventPayload.
        static constexpr const uint16_t kMaxEventPayload = 64;
        bool SendEntityEvent(uint64_t entity, uint8_t kind, const uint8_t* data, uint16_t size);
        using EntityEventFn = std::function<void(PeerId from, uint64_t entity, uint8_t kind,
                                                 const uint8_t* data, uint16_t size)>;
        void SetEntityEventSink(EntityEventFn fn) { m_entityEventSink = std::move(fn); }

        // A hit on another player (relayed only with SO_PVP).
        bool SendPlayerEvent(PeerId target, uint8_t kind, const uint8_t* data, uint16_t size);
        using PlayerEventFn = std::function<void(PeerId from, uint8_t kind, const uint8_t* data, uint16_t size)>;
        void SetPlayerEventSink(PlayerEventFn fn) { m_playerEventSink = std::move(fn); }

        // ---- open user-data channel (0x8000+) -------------------------------

        // Inbound user packets from other peers (both PID_USER and PID_USER_STREAM).
        using CustomEventFn = std::function<void(PeerId from, const uint8_t* data, uint16_t size)>;
        void SetCustomEventSink(CustomEventFn fn) { m_customEvent = std::move(fn); }

        // Ships an opaque payload to all other peers. `stream` = unreliable-
        // sequenced (only the newest matters), otherwise reliable-ordered.
        bool SendCustomEvent(const uint8_t* data, uint16_t size, bool stream = false);

    private:
        void OnClientPacket(const ClientEndpoint::RawPacket& rp);
        void ApplyPlayerList(const uint8_t* data, uint16_t size);
        void ResetClientState();
        void EndMatch(bool notify = true);

        void SendHelloIfDue();
        // Reliable client->server requests queued until WELCOME (bounded).
        void SendToServerWhenWelcomed(PacketId id, const uint8_t* data, uint16_t size);
        void FlushOutbox();
        // Stream packets: [u32 server time][payload].
        bool SendStream(PacketId id, const uint8_t* data, uint16_t size);
        void SendLocalStateIfDue();
        void SendMissionStateIfDue();

        ServerCore                      m_core;   // running only while hosting
        std::unique_ptr<ClientEndpoint> m_client;

        PeerId       m_selfId   = 0;
        bool         m_welcomed = false;
        bool         m_helloSent = false;
        bool         m_wasReady = false;
        bool         m_connectionLost = false;
        RefuseReason m_refusal = REFUSE_NONE;
        RefuseReason m_pendingRefusal = REFUSE_NONE; // seen in the packet handler
        uint32_t     m_worldSeed = 0;
        uint32_t     m_sessionOptions = SO_NONE;
        uint32_t     m_gameVersion = 0;
        char         m_password[HandshakeCodec::kMaxPassword + 1] = { 0 };
        std::set<PeerId> m_peerRoster;

        JoinLeaveFn      m_joinLeave;
        SessionOptionsFn m_sessionOptionsFn;

        // Player / mission streams.
        ILocalStateProvider*   m_localState = nullptr;
        IRemoteStateSink*      m_remoteSink = nullptr;
        StateCorrectionFn      m_correctionSink;
        uint32_t               m_stateIntervalMs = 100;
        uint32_t               m_lastStateSendMs = 0;
        bool                   m_stateSentOnce = false;
        IMissionStateProvider* m_missionState = nullptr;
        MissionStateFn         m_missionSink;
        uint32_t               m_missionIntervalMs = 300;
        uint32_t               m_lastMissionSendMs = 0;
        bool                   m_missionSentOnce = false;
        uint8_t                m_scratch[MAX_PACKET_PAYLOAD];

        // Replicated world state (client mirror).
        WorldStateMirror m_worldMirror;
        WorldStateFn     m_worldSink;
        WorldSyncedFn    m_worldSynced;

        // Counters.
        LedgerClient m_ledgerClient;
        LedgerFn     m_ledgerSink;

        // Entities / events / user data.
        EntityStateFn m_entitySink;
        EntityEventFn m_entityEventSink;
        PlayerEventFn m_playerEventSink;
        CustomEventFn m_customEvent;

        // Client->server reliable requests waiting for WELCOME.
        struct Outgoing { PacketId id; std::vector<uint8_t> bytes; };
        static constexpr const size_t kMaxOutbox = 256;
        std::vector<Outgoing> m_outbox;
    };
}
