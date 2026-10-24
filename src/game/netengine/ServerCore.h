#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Packet.h"
#include "ServerEndpoint.h"
#include "WorldInterfaces.h"
#include "WorldState.h"
#include "Ledger.h"
#include "NetTime.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <vector>

namespace net
{
    // Session-wide options the server announces to every client (WELCOME and
    // PID_SESSION_INFO). Bits 0..15 are engine options, 16..31 game-defined.
    enum SessionOption : uint32_t
    {
        SO_NONE = 0,
        SO_PVP  = 0x00000001, // player-vs-player hits (PID_PLAYER_EVENT) are relayed
    };

    // Per-peer limits the server enforces. A packet over its class budget is
    // dropped; a peer that keeps flooding (violationsToKick drops within
    // violationWindowMs) is refused with REFUSE_FLOOD. rate 0 = unlimited.
    struct RateLimitConfig
    {
        float    streamPerSec    = 90.0f;   // player / mission / entity / user-stream packets
        float    streamBurst     = 180.0f;
        float    requestPerSec   = 240.0f;  // world-state, ledger, entity/player events
        float    requestBurst    = 600.0f;  // a joiner flushes its queued requests at once
        float    userPerSec      = 60.0f;   // reliable PID_USER
        float    userBurst       = 120.0f;
        float    bytesPerSec     = 96.0f * 1024.0f;
        float    bytesBurst      = 192.0f * 1024.0f;
        uint32_t violationsToKick  = 300;
        uint32_t violationWindowMs = 10000;
    };

    // Handshake wire format (little-endian, bounds-checked):
    //   HELLO   : u32 magic | u16 protocol | u32 game version | u8 n | n password bytes
    //   WELCOME : u16 peer id | u32 server time | u32 session options
    //   REFUSE  : u8 reason | u16 server protocol
    namespace HandshakeCodec
    {
        static constexpr const uint8_t kMaxPassword = 64;

        inline uint16_t EncodeHello(uint8_t* out, uint16_t cap, uint32_t gameVersion, const char* password)
        {
            size_t n = password ? std::strlen(password) : 0;
            if (n > kMaxPassword) n = kMaxPassword;
            Writer w(out, cap);
            w.WriteU32(PROTOCOL_MAGIC);
            w.WriteU16(PROTOCOL_VERSION);
            w.WriteU32(gameVersion);
            w.WriteU8(static_cast<uint8_t>(n));
            if (n) w.WriteBytes(password, static_cast<uint16_t>(n));
            return w.Ok() ? w.Position() : 0;
        }

        // `password` receives a NUL-terminated copy (buffer of kMaxPassword+1).
        inline bool DecodeHello(const uint8_t* d, uint16_t size, uint32_t& magic, uint16_t& protocol,
                                uint32_t& gameVersion, char* password)
        {
            Reader r(d, size);
            uint8_t n = 0;
            password[0] = '\0';
            if (!r.ReadU32(magic) || !r.ReadU16(protocol) || !r.ReadU32(gameVersion) || !r.ReadU8(n)) return false;
            if (n > kMaxPassword || !r.ReadBytes(password, n)) return false;
            password[n] = '\0';
            return true;
        }

        inline uint16_t EncodeWelcome(uint8_t* out, uint16_t cap, PeerId peer, uint32_t serverTime, uint32_t options)
        {
            Writer w(out, cap);
            w.WriteU16(peer);
            w.WriteU32(serverTime);
            w.WriteU32(options);
            return w.Ok() ? w.Position() : 0;
        }

        inline bool DecodeWelcome(const uint8_t* d, uint16_t size, PeerId& peer, uint32_t& serverTime, uint32_t& options)
        {
            Reader r(d, size);
            return r.ReadU16(peer) && r.ReadU32(serverTime) && r.ReadU32(options);
        }
    }

    // net::ServerCore
    // ---------------
    // The authoritative server of a match, identical in a hosting game (embedded
    // in net::Session next to the host's own loopback client) and in the
    // dedicated server product: one implementation, so both kinds of server
    // behave the same.
    //
    // It owns the transport (ServerEndpoint) and every server-side decision:
    //   - admission: PID_HELLO handshake (magic, protocol and game version,
    //     optional password, slot cap, optional admission filter), handshake
    //     timeout, PID_REFUSE with a reason;
    //   - replicated world state (WorldStateTable) and its late-join snapshot;
    //   - counters / economy (LedgerServer);
    //   - relays: player / mission streams (timestamped, validated), owner-
    //     checked entity streams, entity and player events, user data;
    //   - per-peer rate limits.
    // Nothing is accepted from a peer before it is admitted.
    class ServerCore
    {
    public:
        ServerCore();
        ~ServerCore();

        ServerCore(const ServerCore&) = delete;
        ServerCore& operator=(const ServerCore&) = delete;

        // ---- lifecycle -----------------------------------------------------

        // Starts listening. False if already running or the port is taken.
        bool Start(uint16_t port, uint32_t worldSeed);
        void Stop();
        void Update();
        bool IsRunning() const { return m_endpoint != nullptr; }
        uint16_t GetPort() const { return m_port; }
        uint32_t GetWorldSeed() const { return m_worldSeed; }

        // ---- configuration (kept across Start/Stop) -------------------------

        // Optional authorization: "" / null = open session (co-op, session
        // games), otherwise PID_HELLO must carry the password.
        void SetPassword(const char* password);
        bool HasPassword() const { return m_password[0] != '\0'; }
        const char* GetPassword() const { return m_password; }

        // Slot cap: admitted peers, the host's loopback client included.
        static constexpr const size_t kDefaultMaxPeers = 32;
        void   SetMaxPeers(size_t n) { m_maxPeers = n ? n : kDefaultMaxPeers; }
        size_t GetMaxPeers() const { return m_maxPeers; }

        // Game content version clients must match (0 = do not check).
        void SetGameVersion(uint32_t v) { m_gameVersion = v; }
        // A connected peer must complete PID_HELLO within this window.
        void SetHandshakeTimeoutMs(uint32_t ms) { m_handshakeTimeoutMs = ms ? ms : 5000; }
        void SetRateLimits(const RateLimitConfig& cfg) { m_limits = cfg; }
        const RateLimitConfig& GetRateLimits() const { return m_limits; }

        // Counter rules (see Ledger.h). Balances reset per match, rules stay.
        void ConfigureCounter(CounterId counter, const CounterConfig& cfg) { m_ledger.Configure(counter, cfg); }

        // Plausibility gate for relayed player state (anti-teleport / speed).
        void SetStateValidator(IStateValidator* v) { m_validator = v; }

        // Final say on admission after the built-in checks (e.g. a server mod).
        using AdmissionFn = std::function<bool(PeerId)>;
        void SetAdmissionFilter(AdmissionFn fn) { m_admission = std::move(fn); }

        // Session options announced to every client; a change is broadcast.
        void     SetSessionOptions(uint32_t options);
        uint32_t GetSessionOptions() const { return m_options; }

        // ---- observers (server-side view; the host's loopback has its own) --

        using JoinLeaveFn = std::function<void(PeerId, bool /*joined*/)>;
        void SetJoinLeaveFn(JoinLeaveFn fn) { m_joinLeave = std::move(fn); }
        // Every accepted world-state change / erasure.
        using WorldChangeFn = std::function<void(const WorldStateEntry& e, bool erased, PeerId origin)>;
        void SetWorldChangeObserver(WorldChangeFn fn) { m_worldChange = std::move(fn); }
        // Every relayed user packet (PID_USER / PID_USER_STREAM).
        using UserPacketFn = std::function<void(PeerId from, const uint8_t* data, uint16_t size)>;
        void SetUserPacketObserver(UserPacketFn fn) { m_userObserver = std::move(fn); }

        // ---- state ----------------------------------------------------------

        const std::set<PeerId>& GetAdmittedPeers() const { return m_admitted; }
        bool IsAdmitted(PeerId peer) const { return m_admitted.count(peer) != 0; }
        const WorldStateTable& GetWorldStateTable() const { return m_world; }
        const LedgerServer&    GetLedgerServer() const { return m_ledger; }
        uint32_t NowMs() const { return m_endpoint ? m_endpoint->NowMs() : 0; }
        uint32_t GetPeerRttMs(PeerId peer) const { return m_endpoint ? m_endpoint->GetPeerRttMs(peer) : 0; }

        // Refuses / removes a peer with a reason it is told about.
        void Kick(PeerId peer, RefuseReason reason);

        struct Stats
        {
            uint32_t refused      = 0; // handshakes refused
            uint32_t rateDropped  = 0; // packets dropped by the rate limits
            uint32_t stateDropped = 0; // player states refused by the validator
            uint32_t corrections  = 0; // PID_STATE_CORRECTION sent
            uint32_t floodKicks   = 0;
        };
        const Stats& GetStats() const { return m_stats; }

        // Server-side framing of a relayed stream: [u16 from][u32 time][payload].
        static constexpr const uint16_t kStreamRelayHeader = 2 + 4;
        // Client->server stream prefix: [u32 sample server time][payload].
        static constexpr const uint16_t kStreamClientHeader = 4;
        // A released entity nobody claims within this window is erased.
        void SetClaimWindowMs(uint32_t ms) { m_claimWindowMs = ms ? ms : 3000; }

        // How far a client stream timestamp may lag the server clock before it
        // is clamped (and may never lead it).
        static constexpr const uint32_t kMaxStreamLagMs = 2000;

    private:
        enum LimitClass { LC_STREAM, LC_REQUEST, LC_USER, LC_COUNT };

        struct PeerInfo
        {
            uint32_t    connectedAt = 0;
            bool        admitted = false;
            TokenBucket buckets[LC_COUNT];
            TokenBucket bytes;
            uint32_t    violations = 0;
            uint32_t    violationWindowStart = 0;
        };

        void OnPacket(const ServerEndpoint::RawPacket& rp);
        void OnPeerEvent(const PeerEvent& e);
        void HandleHello(PeerId from, const uint8_t* data, uint16_t size);
        void Refuse(PeerId peer, RefuseReason reason);
        void Admit(PeerId peer);
        void BroadcastPlayerList();
        void SendWorldSnapshot(PeerId peer);
        bool AllowPacket(PeerId from, PacketId id, uint16_t size);
        void HandleState(PeerId from, const uint8_t* data, uint16_t size);
        void RelayStream(PacketId id, PeerId from, const uint8_t* data, uint16_t size);
        void HandleWorldStateRequest(PeerId from, const uint8_t* data, uint16_t size);
        void HandleLedgerRequest(PeerId from, const uint8_t* data, uint16_t size);
        void RelayEntityStates(PeerId from, const uint8_t* data, uint16_t size);
        void HandleEntityEvent(PeerId from, const uint8_t* data, uint16_t size);
        void HandlePlayerEvent(PeerId from, const uint8_t* data, uint16_t size);
        void Relay(PacketId id, PeerId from, const uint8_t* data, uint16_t size);
        void SendToAdmitted(PacketId id, const uint8_t* data, uint16_t size, PeerId omit = 0);
        void BroadcastErase(const WorldStateKey& key, PeerId origin, uint32_t seq);

        std::unique_ptr<ServerEndpoint> m_endpoint;
        uint16_t m_port = 0;
        uint32_t m_worldSeed = 0;

        // configuration
        char            m_password[65] = { 0 };
        size_t          m_maxPeers = kDefaultMaxPeers;
        uint32_t        m_gameVersion = 0;
        uint32_t        m_handshakeTimeoutMs = 5000;
        RateLimitConfig m_limits;
        uint32_t        m_options = SO_NONE;
        uint32_t        m_claimWindowMs = 3000;
        IStateValidator* m_validator = nullptr;
        AdmissionFn     m_admission;

        // observers
        JoinLeaveFn   m_joinLeave;
        WorldChangeFn m_worldChange;
        UserPacketFn  m_userObserver;

        // state
        std::map<PeerId, PeerInfo> m_peers;    // every connected peer
        std::set<PeerId>           m_admitted; // admitted subset
        WorldStateTable m_world;
        LedgerServer    m_ledger;
        Stats           m_stats;
    };
}
