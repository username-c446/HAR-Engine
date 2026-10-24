#include "Session.h"

#include <cstring>

namespace net
{
    namespace
    {
        inline uint16_t ReadU16Le(const uint8_t* d)
        {
            return static_cast<uint16_t>(d[0]) | static_cast<uint16_t>(d[1]) << 8;
        }
        inline uint32_t ReadU32Le(const uint8_t* d)
        {
            return static_cast<uint32_t>(d[0]) | static_cast<uint32_t>(d[1]) << 8
                 | static_cast<uint32_t>(d[2]) << 16 | static_cast<uint32_t>(d[3]) << 24;
        }
        // Relayed stream framing (ServerCore): [u16 from][u32 time][payload].
        constexpr const uint16_t kRelayHeader = ServerCore::kStreamRelayHeader;
    }

    Session::Session() = default;
    // Silent teardown: the sinks the game attached may already be gone, so a
    // destroyed session reports nothing (Leave() does, explicitly).
    Session::~Session() { EndMatch(false); }

    // =====================================================================
    // lifecycle
    // =====================================================================

    void Session::ResetClientState()
    {
        m_selfId = 0;
        m_welcomed = false;
        m_helloSent = false;
        m_wasReady = false;
        m_connectionLost = false;
        m_refusal = REFUSE_NONE;
        m_pendingRefusal = REFUSE_NONE;
        m_worldSeed = 0;
        m_sessionOptions = SO_NONE;
        m_peerRoster.clear();
        m_worldMirror.Clear();
        m_ledgerClient.Clear();
        m_outbox.clear();
        m_stateSentOnce = false;
        m_missionSentOnce = false;
    }

    bool Session::StartHost(uint16_t port, uint32_t worldSeed)
    {
        if (IsInMatch()) return false;
        if (!m_core.Start(port, worldSeed)) return false;

        // The host is a client of its own server: same handshake (the host's
        // password, if any, is presented like any joiner's).
        auto client = std::make_unique<ClientEndpoint>();
        if (!client->Connect("127.0.0.1", port))
        {
            m_core.Stop();
            return false;
        }
        ResetClientState();
        m_client = std::move(client);
        m_worldSeed = worldSeed;
        m_client->SetPacketSink([this](const ClientEndpoint::RawPacket& rp) { OnClientPacket(rp); });
        return true;
    }

    bool Session::Join(const char* serverIp, uint16_t port)
    {
        if (IsInMatch()) return false;
        auto client = std::make_unique<ClientEndpoint>();
        if (!client->Connect(serverIp, port)) return false;
        ResetClientState();
        m_client = std::move(client);
        m_client->SetPacketSink([this](const ClientEndpoint::RawPacket& rp) { OnClientPacket(rp); });
        return true;
    }

    void Session::EndMatch(bool notify)
    {
        // Tell the game every remote player is gone (a uniform cleanup path for
        // Leave, refusal and connection loss).
        std::set<PeerId> gone;
        gone.swap(m_peerRoster);
        for (PeerId p : gone)
        {
            if (!notify) break;
            if (m_remoteSink) m_remoteSink->OnPeerLeft(p);
            if (m_joinLeave) m_joinLeave(p, false);
        }
        if (m_client) m_client->Disconnect();
        m_client.reset();
        m_core.Stop();
    }

    void Session::Leave()
    {
        EndMatch();
        ResetClientState();
    }

    void Session::Update()
    {
        if (m_core.IsRunning()) m_core.Update();
        if (!m_client) return;
        m_client->Update();

        // A refusal arrives inside the transport's service loop; the endpoint
        // is torn down only now that it has returned.
        if (m_pendingRefusal != REFUSE_NONE)
        {
            RefuseReason reason = m_pendingRefusal;
            EndMatch();
            ResetClientState();
            m_refusal = reason;
            return;
        }

        if (m_client->IsPeerReady()) m_wasReady = true;
        else if (m_wasReady && !m_connectionLost)
        {
            // An established connection went away without a PID_REFUSE.
            m_connectionLost = true;
            EndMatch();
            return;
        }

        SendHelloIfDue();
        FlushOutbox();
        SendLocalStateIfDue();
        SendMissionStateIfDue();
    }

    void Session::SetPassword(const char* password)
    {
        std::memset(m_password, 0, sizeof(m_password));
        if (password == nullptr) return;
        size_t n = std::strlen(password);
        if (n > HandshakeCodec::kMaxPassword) n = HandshakeCodec::kMaxPassword;
        std::memcpy(m_password, password, n);
    }

    void Session::SendHelloIfDue()
    {
        if (m_helloSent || !m_client || !m_client->IsPeerReady()) return;
        // A hosting session presents its own server's password, so the host
        // always passes its own gate; a joiner presents what it was given.
        uint8_t buf[4 + 2 + 4 + 1 + HandshakeCodec::kMaxPassword];
        uint16_t n = HandshakeCodec::EncodeHello(buf, sizeof(buf), m_gameVersion,
                                                 IsHost() ? m_core.GetPassword() : m_password);
        if (n && m_client->SendTo(PID_HELLO, buf, n)) m_helloSent = true;
    }

    // =====================================================================
    // inbound
    // =====================================================================

    void Session::ApplyPlayerList(const uint8_t* data, uint16_t size)
    {
        if (size < 2) return;
        uint16_t count = ReadU16Le(data);
        if (size < 2 + count * 2) return;
        std::set<PeerId> now;
        for (uint16_t k = 0; k < count; ++k)
        {
            PeerId id = ReadU16Le(data + 2 + k * 2);
            if (id != m_selfId) now.insert(id);
        }
        // Diff against the previous roster: one join/leave per change.
        std::set<PeerId> before;
        before.swap(m_peerRoster);
        m_peerRoster = now;
        for (PeerId p : before)
            if (!now.count(p))
            {
                if (m_remoteSink) m_remoteSink->OnPeerLeft(p);
                if (m_joinLeave) m_joinLeave(p, false);
            }
        for (PeerId p : now)
            if (!before.count(p) && m_joinLeave) m_joinLeave(p, true);
    }

    void Session::OnClientPacket(const ClientEndpoint::RawPacket& rp)
    {
        switch (rp.id)
        {
        case PID_WELCOME:
        {
            PeerId self = 0;
            uint32_t serverTime = 0, options = 0;
            if (!HandshakeCodec::DecodeWelcome(rp.data, rp.size, self, serverTime, options)) break;
            m_selfId = self;
            m_welcomed = true;
            m_client->SeedServerClock(serverTime);
            m_sessionOptions = options;
            if (m_sessionOptionsFn) m_sessionOptionsFn(options);
            break;
        }

        case PID_REFUSE:
            if (rp.size >= 1)
            {
                RefuseReason reason = static_cast<RefuseReason>(rp.data[0]);
                m_pendingRefusal = reason == REFUSE_NONE ? REFUSE_DENIED : reason;
            }
            break;

        case PID_WORLD_SEED:
            if (rp.size >= 4) m_worldSeed = ReadU32Le(rp.data);
            break;

        case PID_SESSION_INFO:
            if (rp.size >= 4)
            {
                m_sessionOptions = ReadU32Le(rp.data);
                if (m_sessionOptionsFn) m_sessionOptionsFn(m_sessionOptions);
            }
            break;

        case PID_PLAYER_LIST:
            ApplyPlayerList(rp.data, rp.size);
            break;

        case PID_STATE_CORRECTION:
            if (m_correctionSink) m_correctionSink(rp.data, rp.size);
            break;

        case PID_USER:
        case PID_USER_STREAM:
            if (rp.size >= 2)
            {
                PeerId from = ReadU16Le(rp.data);
                if (from != m_selfId && m_customEvent)
                    m_customEvent(from, rp.data + 2, static_cast<uint16_t>(rp.size - 2));
            }
            break;

        case PID_PLAYER_STATE:
            if (rp.size > kRelayHeader && m_remoteSink)
            {
                PeerId from = ReadU16Le(rp.data);
                if (from != m_selfId)
                    m_remoteSink->ApplyRemoteState(from, rp.data + kRelayHeader,
                                                   static_cast<uint16_t>(rp.size - kRelayHeader),
                                                   ReadU32Le(rp.data + 2));
            }
            break;

        case PID_MISSION_STATE:
            if (rp.size > kRelayHeader && m_missionSink)
            {
                PeerId from = ReadU16Le(rp.data);
                if (from != m_selfId)
                    m_missionSink(from, rp.data + kRelayHeader, static_cast<uint16_t>(rp.size - kRelayHeader));
            }
            break;

        case PID_ENTITY_STATE:
            if (rp.size >= kRelayHeader && m_entitySink)
            {
                PeerId owner = ReadU16Le(rp.data);
                if (owner == m_selfId) break;
                uint32_t t = ReadU32Le(rp.data + 2);
                std::vector<EntityStateRecord> recs;
                if (EntityStreamCodec::Decode(rp.data + kRelayHeader, static_cast<uint16_t>(rp.size - kRelayHeader), recs))
                    for (const EntityStateRecord& rec : recs) m_entitySink(owner, rec, t);
            }
            break;

        case PID_ENTITY_EVENT:
            // [u16 from][u64 entity][u8 kind][payload]
            if (rp.size >= 2 + 8 + 1 && m_entityEventSink)
            {
                PeerId from = ReadU16Le(rp.data);
                uint64_t entity = static_cast<uint64_t>(ReadU32Le(rp.data + 2))
                                | static_cast<uint64_t>(ReadU32Le(rp.data + 6)) << 32;
                m_entityEventSink(from, entity, rp.data[10], rp.data + 11, static_cast<uint16_t>(rp.size - 11));
            }
            break;

        case PID_PLAYER_EVENT:
            // [u16 from][u16 target][u8 kind][payload]
            if (rp.size >= 2 + 2 + 1 && m_playerEventSink)
            {
                PeerId from = ReadU16Le(rp.data);
                m_playerEventSink(from, rp.data[4], rp.data + 5, static_cast<uint16_t>(rp.size - 5));
            }
            break;

        case PID_WS_UPDATE:
        {
            PeerId origin = 0;
            uint32_t seq = 0;
            WorldStateEntry e;
            if (!WorldStateCodec::DecodeUpdate(rp.data, rp.size, origin, seq, e)) break;
            WorldStateReason reason = m_worldMirror.OnUpdate(e, origin, seq, m_selfId);
            if (m_worldSink) m_worldSink(e, reason, origin);
            break;
        }

        case PID_WS_REJECT:
        {
            uint32_t seq = 0;
            WorldStateKey key;
            bool exists = false;
            WorldStateEntry e;
            if (!WorldStateCodec::DecodeReject(rp.data, rp.size, seq, key, exists, e)) break;
            m_worldMirror.OnReject(key, exists, e, seq);
            if (!exists) { e = WorldStateEntry(); e.key = key; }
            if (m_worldSink) m_worldSink(e, exists ? WSR_REJECT : WSR_ERASED, m_selfId);
            break;
        }

        case PID_WS_ERASE:
        {
            PeerId origin = 0;
            uint32_t seq = 0;
            WorldStateKey key;
            if (!WorldStateCodec::DecodeErase(rp.data, rp.size, origin, seq, key)) break;
            m_worldMirror.OnErase(key);
            WorldStateEntry e;
            e.key = key;
            if (m_worldSink) m_worldSink(e, WSR_ERASED, origin);
            break;
        }

        case PID_WS_SNAPSHOT:
        {
            std::vector<WorldStateEntry> entries;
            if (!WorldStateCodec::DecodeSnapshot(rp.data, rp.size, entries)) break;
            for (const WorldStateEntry& e : entries)
            {
                m_worldMirror.OnSnapshotEntry(e);
                if (m_worldSink) m_worldSink(e, WSR_SNAPSHOT, e.owner);
            }
            break;
        }

        case PID_WS_SNAPSHOT_END:
            m_worldMirror.OnSnapshotEnd();
            if (m_worldSynced) m_worldSynced();
            break;

        case PID_LEDGER_RESULT:
        {
            uint32_t seq = 0;
            CounterId counter = 0;
            bool accepted = false;
            int64_t balance = 0;
            if (!LedgerCodec::DecodeResult(rp.data, rp.size, seq, counter, accepted, balance)) break;
            LedgerOutcome o = m_ledgerClient.OnResult(seq, counter, accepted, balance);
            if (m_ledgerSink) m_ledgerSink(o);
            break;
        }

        default:
            break;
        }
    }

    // =====================================================================
    // outbound
    // =====================================================================

    bool Session::RequestWorldState(const WorldStateOp& op)
    {
        if (!m_client) return false;
        if (op.kind >= WSO_COUNT || op.policy >= WSP_COUNT || op.value.len > WS_MAX_VALUE) return false;
        uint32_t seq = m_worldMirror.Predict(op.key);
        uint8_t buf[MAX_PACKET_PAYLOAD];
        uint16_t n = WorldStateCodec::EncodeRequest(buf, sizeof(buf), seq, op);
        if (n == 0) return false;
        SendToServerWhenWelcomed(PID_WS_REQUEST, buf, n);
        return true;
    }

    bool Session::RequestLedgerChange(CounterId counter, uint8_t reason, int32_t delta, uint64_t claim)
    {
        if (!m_client) return false;
        uint32_t seq = m_ledgerClient.Predict(counter, reason, delta);
        uint8_t buf[LedgerCodec::REQUEST_SIZE];
        uint16_t n = LedgerCodec::EncodeRequest(buf, sizeof(buf), seq, counter, reason, delta, claim);
        if (n == 0) return false;
        SendToServerWhenWelcomed(PID_LEDGER_REQUEST, buf, n);
        return true;
    }

    bool Session::SendStream(PacketId id, const uint8_t* data, uint16_t size)
    {
        if (!m_client || !m_welcomed) return false;
        uint8_t buf[MAX_PACKET_PAYLOAD];
        Writer w(buf, sizeof(buf));
        w.WriteU32(m_client->ServerTimeMs());
        w.WriteBytes(data, size);
        return w.Ok() && m_client->SendTo(id, buf, w.Position());
    }

    bool Session::SendEntityStates(const std::vector<EntityStateRecord>& records)
    {
        if (!m_client || !m_welcomed) return false;
        size_t first = 0;
        bool ok = true;
        while (first < records.size())
        {
            uint8_t buf[MAX_PACKET_PAYLOAD];
            uint16_t n = EntityStreamCodec::Encode(buf, MAX_PACKET_PAYLOAD - ServerCore::kStreamRelayHeader,
                                                   records, first);
            if (n == 0) break;
            ok = SendStream(PID_ENTITY_STATE, buf, n) && ok;
        }
        return ok;
    }

    bool Session::SendEntityEvent(uint64_t entity, uint8_t kind, const uint8_t* data, uint16_t size)
    {
        if (!m_client || size > kMaxEventPayload) return false;
        uint8_t buf[8 + 1 + kMaxEventPayload];
        Writer w(buf, sizeof(buf));
        w.WriteU32(static_cast<uint32_t>(entity & 0xFFFFFFFFu));
        w.WriteU32(static_cast<uint32_t>(entity >> 32));
        w.WriteU8(kind);
        if (size) w.WriteBytes(data, size);
        if (!w.Ok()) return false;
        SendToServerWhenWelcomed(PID_ENTITY_EVENT, buf, w.Position());
        return true;
    }

    bool Session::SendPlayerEvent(PeerId target, uint8_t kind, const uint8_t* data, uint16_t size)
    {
        if (!m_client || size > kMaxEventPayload) return false;
        uint8_t buf[2 + 1 + kMaxEventPayload];
        Writer w(buf, sizeof(buf));
        w.WriteU16(target);
        w.WriteU8(kind);
        if (size) w.WriteBytes(data, size);
        if (!w.Ok()) return false;
        SendToServerWhenWelcomed(PID_PLAYER_EVENT, buf, w.Position());
        return true;
    }

    void Session::SendToServerWhenWelcomed(PacketId id, const uint8_t* data, uint16_t size)
    {
        if (m_welcomed && m_outbox.empty() && m_client->SendTo(id, data, size)) return;
        // Not welcomed yet (or older requests still waiting): keep order.
        if (m_outbox.size() >= kMaxOutbox) m_outbox.erase(m_outbox.begin());
        Outgoing o;
        o.id = id;
        o.bytes.assign(data, data + size);
        m_outbox.push_back(o);
    }

    void Session::FlushOutbox()
    {
        if (!m_client || !m_welcomed) return;
        size_t sent = 0;
        while (sent < m_outbox.size())
        {
            const Outgoing& o = m_outbox[sent];
            if (!m_client->SendTo(o.id, o.bytes.data(), static_cast<uint16_t>(o.bytes.size()))) break;
            ++sent;
        }
        m_outbox.erase(m_outbox.begin(), m_outbox.begin() + sent);
    }

    void Session::SendLocalStateIfDue()
    {
        if (!m_localState || !m_welcomed) return;
        uint32_t now = m_client->ServerTimeMs();
        if (m_stateSentOnce && TimeDiff(now, m_lastStateSendMs) < static_cast<int32_t>(m_stateIntervalMs)) return;

        uint16_t size = 0;
        if (!m_localState->GetLocalState(m_scratch, MAX_PACKET_PAYLOAD - ServerCore::kStreamRelayHeader, size)
            || size == 0) return;
        if (SendStream(PID_PLAYER_STATE, m_scratch, size))
        {
            m_lastStateSendMs = now;
            m_stateSentOnce = true;
        }
    }

    void Session::SendMissionStateIfDue()
    {
        if (!m_missionState || !m_welcomed) return;
        uint32_t now = m_client->ServerTimeMs();
        if (m_missionSentOnce && TimeDiff(now, m_lastMissionSendMs) < static_cast<int32_t>(m_missionIntervalMs)) return;

        uint16_t size = 0;
        if (!m_missionState->GetMissionState(m_scratch, MAX_PACKET_PAYLOAD - ServerCore::kStreamRelayHeader, size)
            || size == 0) return;
        if (SendStream(PID_MISSION_STATE, m_scratch, size))
        {
            m_lastMissionSendMs = now;
            m_missionSentOnce = true;
        }
    }

    bool Session::SendCustomEvent(const uint8_t* data, uint16_t size, bool stream)
    {
        if (!m_client || size > MAX_PACKET_PAYLOAD - 2) return false;
        PacketId id = stream ? PID_USER_STREAM : PID_USER;
        if (stream) return m_welcomed && m_client->SendTo(id, data, size);
        SendToServerWhenWelcomed(id, data, size);
        return true;
    }
}
