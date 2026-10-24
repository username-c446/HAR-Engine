#include "Session.h"

namespace net
{
    // One process-level Session instance for the game (see Session.h).
    static Session* s_session = nullptr;

    Session* Session::Create()
    {
        if (s_session == nullptr)
            s_session = new Session();
        return s_session;
    }
    Session* Session::Get()
    {
        return s_session;
    }
    void Session::Destroy()
    {
        delete s_session;
        s_session = nullptr;
    }

    Session::Session() = default;
    Session::~Session() { Leave(); }

    namespace
    {
        // Pack/unpack helpers use the reserved-range ids defined in Packet.h.
        constexpr const PacketId kIdWelcome    = PID_WELCOME;
        constexpr const PacketId kIdSeed       = PID_WORLD_SEED;
        constexpr const PacketId kIdPlayerList = PID_PLAYER_LIST;
        constexpr const PacketId kIdState      = PID_PLAYER_STATE;
        constexpr const PacketId kIdMission    = PID_MISSION_STATE;
        constexpr const PacketId kIdEntity     = PID_ENTITY_STATE;
        constexpr const PacketId kIdAuth       = PID_AUTH;
        constexpr const PacketId kIdUser       = PID_USER;

        inline void WriteU16Le(uint8_t* d, uint16_t v)
        {
            d[0] = static_cast<uint8_t>(v & 0xFF);
            d[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        }
        inline uint16_t ReadU16Le(const uint8_t* d)
        {
            return static_cast<uint16_t>(d[0]) | static_cast<uint16_t>(d[1]) << 8;
        }
    }

    // =====================================================================
    // lifecycle
    // =====================================================================

    bool Session::StartHost(uint16_t port, uint32_t worldSeed)
    {
        if (IsInMatch()) return false;

        auto server = std::make_unique<ServerEndpoint>();
        if (!server->Start(port, static_cast<uint16_t>(m_maxPeers))) return false;

        auto client = std::make_unique<ClientEndpoint>();
        if (!client->Connect("127.0.0.1", port))
        {
            server->Stop();
            return false;
        }

        m_server    = std::move(server);
        m_client    = std::move(client);
        m_worldSeed = worldSeed;
        m_port      = port;
        m_selfId    = 0; // resolved on WELCOME
        m_welcomed  = false;
        m_peerRoster.clear();
        m_admitted.clear();
        m_worldTable.Clear();
        m_worldMirror.Clear();
        m_ledgerServer.Clear();
        m_ledgerClient.Clear();
        m_outbox.clear();

        // A password-protected host's own loopback client must authenticate to
        // its own server like any other peer. Seed the client-side password
        // with the host password so the host can actually participate; the
        // actual PID_AUTH is delivered by SendAuthIfDue() once the loopback
        // connection is up (sending it here, before the handshake, could drop
        // the payload and strand the host).
        m_authSent = false;
        if (m_hostPassword[0] != '\0')
            SetClientPassword(m_hostPassword);

        m_server->SetPacketSink([this](const ServerEndpoint::RawPacket& rp) { OnServerPacket(rp); });
        m_server->SetPeerEventFn([this](const PeerEvent& e) { OnServerPeerEvent(e); });
        m_client->SetPacketSink([this](const ClientEndpoint::RawPacket& rp) { OnClientPacket(rp); });
        return true;
    }

    bool Session::Join(const char* serverIp, uint16_t port)
    {
        if (IsInMatch()) return false;

        auto client = std::make_unique<ClientEndpoint>();
        if (!client->Connect(serverIp, port)) return false;

        m_client = std::move(client);
        m_port = port;
        m_worldSeed = 0;
        m_selfId = 0;
        m_welcomed = false;
        m_peerRoster.clear();
        m_worldMirror.Clear();
        m_ledgerClient.Clear();
        m_outbox.clear();
        m_authSent = false;

        // The client-side password (SetPassword) is presented the moment the
        // ENet connection is up. We do NOT send it here: SendTo before the
        // handshake completes can drop the payload, leaving a protected host
        // unable to admit us. Delivery is deferred to Update() via
        // SendAuthIfDue(), which fires once IsPeerReady() is true.
        m_client->SetPacketSink([this](const ClientEndpoint::RawPacket& rp) { OnClientPacket(rp); });
        return true;
    }

    void Session::Leave()
    {
        if (m_client) m_client->Disconnect();
        if (m_server) m_server->Stop();
        m_client.reset();
        m_server.reset();
        m_selfId = 0;
        m_welcomed = false;
        m_worldSeed = 0;
        m_port = 0;
        m_peerRoster.clear();
        m_admitted.clear();
        m_joinLeave = nullptr;
        m_missionTick = 0;
        m_worldTable.Clear();
        m_worldMirror.Clear();
        m_ledgerServer.Clear(); // balances only; counter rules survive
        m_ledgerClient.Clear();
        m_outbox.clear();
    }

    void Session::Update()
    {
        if (m_server) m_server->Update();
        if (m_client) m_client->Update();

        SendAuthIfDue();
        FlushOutbox();
        SendLocalStateIfDue();
        SendMissionStateIfDue();
    }

    // =====================================================================
    // server side
    // =====================================================================

    void Session::OnServerPeerEvent(const PeerEvent& e)
    {
        if (!e.connected)
        {
            bool wasAdmitted = m_admitted.erase(e.peer) != 0;

            // Its owner-bound world state goes with it (dynamic entities), and
            // its session balances are dropped.
            std::vector<WorldStateKey> erased;
            m_worldTable.OnPeerLeft(e.peer, erased);
            for (const WorldStateKey& key : erased)
            {
                uint8_t buf[MAX_PACKET_PAYLOAD];
                uint16_t n = WorldStateCodec::EncodeErase(buf, sizeof(buf), 0, 0, key);
                if (n) m_server->Broadcast(PID_WS_ERASE, buf, n, 0);
            }
            m_ledgerServer.OnPeerLeft(e.peer);

            if (wasAdmitted && m_joinLeave) m_joinLeave(e.peer, false);
            return;
        }
        // Open host: admit immediately. Protected host: wait for PID_AUTH.
        if (m_hostPassword[0] == '\0')
            AdmitPeer(e.peer);
    }

    void Session::AdmitPeer(PeerId peer)
    {
        if (IsAdmitted(peer)) return;

        uint8_t welcome[2];
        WriteU16Le(welcome, peer);
        m_server->SendTo(peer, kIdWelcome, welcome, sizeof(welcome));

        uint8_t seed[4];
        seed[0] = static_cast<uint8_t>(m_worldSeed & 0xFF);
        seed[1] = static_cast<uint8_t>((m_worldSeed >> 8) & 0xFF);
        seed[2] = static_cast<uint8_t>((m_worldSeed >> 16) & 0xFF);
        seed[3] = static_cast<uint8_t>((m_worldSeed >> 24) & 0xFF);
        m_server->SendTo(peer, kIdSeed, seed, sizeof(seed));

        m_admitted.insert(peer);
        if (m_joinLeave) m_joinLeave(peer, true);

        std::vector<uint8_t> list(2 + m_admitted.size() * 2, 0);
        WriteU16Le(list.data(), static_cast<uint16_t>(m_admitted.size()));
        uint16_t idx = 0;
        for (PeerId id : m_admitted)
        {
            WriteU16Le(list.data() + 2 + idx * 2, id);
            ++idx;
        }
        m_server->Broadcast(kIdPlayerList, list.data(), static_cast<uint16_t>(list.size()), 0);

        // The reliable channel is ordered, so the joiner sees the whole world
        // before any live change that follows.
        SendWorldSnapshot(peer);
    }

    void Session::SendWorldSnapshot(PeerId peer)
    {
        std::vector<WorldStateEntry> entries;
        entries.reserve(m_worldTable.Size());
        m_worldTable.ForEach([&](const WorldStateEntry& e) { entries.push_back(e); });

        std::vector<std::vector<uint8_t>> chunks;
        WorldStateCodec::EncodeSnapshot(entries, MAX_PACKET_PAYLOAD, chunks);
        for (const auto& c : chunks)
            m_server->SendTo(peer, PID_WS_SNAPSHOT, c.data(), static_cast<uint16_t>(c.size()));

        uint8_t end[4];
        Writer w(end, sizeof(end));
        w.WriteU32(static_cast<uint32_t>(entries.size()));
        m_server->SendTo(peer, PID_WS_SNAPSHOT_END, end, sizeof(end));
    }

    void Session::OnServerPacket(const ServerEndpoint::RawPacket& rp)
    {
        if (rp.id == kIdAuth)
        {
            // Client presents a password. A host without a password accepts
            // everyone implicitly; a protected host only admits a peer whose
            // presented password matches. A bad/missing password drops the
            // peer (disconnect), so an unauthenticated client is never admitted.
            char candidate[65];
            memset(candidate, 0, sizeof(candidate));
            size_t copyLen = rp.size < sizeof(candidate) - 1 ? rp.size : sizeof(candidate) - 1;
            if (copyLen) memcpy(candidate, rp.data, copyLen);
            candidate[copyLen] = 0;

            if (m_hostPassword[0] == '\0'
                || (copyLen <= strlen(m_hostPassword) && strcmp(candidate, m_hostPassword) == 0))
            {
                AdmitPeer(rp.from);
            }
            else
            {
                m_server->KickPeer(rp.from);
            }
            return;
        }

        // Everything else requires an admitted peer.
        if (!IsAdmitted(rp.from)) return;

        switch (rp.id)
        {
        case kIdUser:
            // Open user-data relay: the server does not interpret the bytes.
            Relay(kIdUser, rp.from, rp.data, rp.size);
            break;

        case kIdState:
            // Server-authoritative gate: a validator may reject the state
            // (e.g. teleport detection). Unvalidated/simple relays pass all.
            if (m_validator && !m_validator->ValidatePeerState(rp.from, rp.data, rp.size))
                return; // drop this state; keep last accepted authoritative
            // The host sees other players through its loopback client, which
            // receives the relayed copy, so the sink is not fed directly.
            Relay(kIdState, rp.from, rp.data, rp.size);
            break;

        case kIdMission:
            Relay(kIdMission, rp.from, rp.data, rp.size);
            break;

        case kIdEntity:
            RelayEntityStates(rp.from, rp.data, rp.size);
            break;

        case PID_WS_REQUEST:
            HandleWorldStateRequest(rp.from, rp.data, rp.size);
            break;

        case PID_LEDGER_REQUEST:
            HandleLedgerRequest(rp.from, rp.data, rp.size);
            break;

        default:
            break;
        }
    }

    void Session::Relay(PacketId id, PeerId from, const uint8_t* data, uint16_t size)
    {
        // [sender u16][payload], to every peer except the sender.
        uint8_t framed[2 + MAX_PACKET_PAYLOAD];
        if (size + 2 > sizeof(framed)) return;
        WriteU16Le(framed, from);
        std::memcpy(framed + 2, data, size);
        m_server->Broadcast(id, framed, static_cast<uint16_t>(size + 2), from);
    }

    void Session::HandleWorldStateRequest(PeerId from, const uint8_t* data, uint16_t size)
    {
        uint32_t seq = 0;
        WorldStateOp op;
        if (!WorldStateCodec::DecodeRequest(data, size, seq, op)) return;

        WorldStateEntry entry;
        bool exists = false;
        WorldStateTable::Result r = m_worldTable.Apply(from, op, entry, exists);

        uint8_t buf[MAX_PACKET_PAYLOAD];
        uint16_t n = 0;
        switch (r)
        {
        case WorldStateTable::ACCEPTED:
            if (op.kind == WSO_ERASE)
            {
                n = WorldStateCodec::EncodeErase(buf, sizeof(buf), from, seq, op.key);
                if (n) m_server->Broadcast(PID_WS_ERASE, buf, n, 0);
            }
            else
            {
                n = WorldStateCodec::EncodeUpdate(buf, sizeof(buf), from, seq, entry);
                if (n) m_server->Broadcast(PID_WS_UPDATE, buf, n, 0);
            }
            break;

        case WorldStateTable::UNCHANGED:
            // Valid but no change: confirm to the requester only.
            if (exists)
            {
                n = WorldStateCodec::EncodeUpdate(buf, sizeof(buf), from, seq, entry);
                if (n) m_server->SendTo(from, PID_WS_UPDATE, buf, n);
            }
            else
            {
                n = WorldStateCodec::EncodeErase(buf, sizeof(buf), from, seq, op.key);
                if (n) m_server->SendTo(from, PID_WS_ERASE, buf, n);
            }
            break;

        case WorldStateTable::REJECTED:
            n = WorldStateCodec::EncodeReject(buf, sizeof(buf), seq, op.key, exists, entry);
            if (n) m_server->SendTo(from, PID_WS_REJECT, buf, n);
            break;
        }
    }

    void Session::HandleLedgerRequest(PeerId from, const uint8_t* data, uint16_t size)
    {
        uint32_t seq = 0;
        CounterId counter = 0;
        uint8_t reason = 0;
        int32_t delta = 0;
        uint64_t claim = 0;
        if (!LedgerCodec::DecodeRequest(data, size, seq, counter, reason, delta, claim)) return;

        int64_t balance = 0;
        bool accepted = m_ledgerServer.Apply(from, counter, reason, delta, claim, balance)
                        == LedgerServer::ACCEPTED;
        uint8_t buf[LedgerCodec::RESULT_SIZE];
        uint16_t n = LedgerCodec::EncodeResult(buf, sizeof(buf), seq, counter, accepted, balance);
        if (n) m_server->SendTo(from, PID_LEDGER_RESULT, buf, n);
    }

    void Session::RelayEntityStates(PeerId from, const uint8_t* data, uint16_t size)
    {
        std::vector<EntityStateRecord> in;
        if (!EntityStreamCodec::Decode(data, size, in)) return;

        // Keep only records for entities this peer owns.
        std::vector<EntityStateRecord> owned;
        owned.reserve(in.size());
        for (const EntityStateRecord& rec : in)
        {
            WorldStateKey key;
            key.object = rec.entity;
            key.channel = WS_CHANNEL_ENTITY;
            WorldStateEntry e;
            if (m_worldTable.Find(key, e) && e.policy == WSP_OWNER && e.owner == from)
                owned.push_back(rec);
        }

        size_t first = 0;
        while (first < owned.size())
        {
            uint8_t batch[MAX_PACKET_PAYLOAD - 2];
            uint16_t n = EntityStreamCodec::Encode(batch, sizeof(batch), owned, first);
            if (n == 0) break;
            Relay(kIdEntity, from, batch, n);
        }
    }

    // =====================================================================
    // client side (host loopback and joiner)
    // =====================================================================

    void Session::OnClientPacket(const ClientEndpoint::RawPacket& rp)
    {
        switch (rp.id)
        {
        case kIdWelcome:
            if (rp.size >= 2)
            {
                m_selfId = ReadU16Le(rp.data);
                m_welcomed = true;
            }
            break;

        case kIdSeed:
            if (rp.size >= 4)
                m_worldSeed = static_cast<uint32_t>(rp.data[0])
                            | static_cast<uint32_t>(rp.data[1]) << 8
                            | static_cast<uint32_t>(rp.data[2]) << 16
                            | static_cast<uint32_t>(rp.data[3]) << 24;
            break;

        case kIdPlayerList:
            if (rp.size >= 2)
            {
                uint16_t count = ReadU16Le(rp.data);
                if (rp.size >= 2 + count * 2)
                {
                    m_peerRoster.clear();
                    for (uint16_t k = 0; k < count; ++k)
                    {
                        PeerId id = ReadU16Le(rp.data + 2 + k * 2);
                        if (id != m_selfId) m_peerRoster.insert(id);
                    }
                }
            }
            break;

        case kIdUser:
            if (rp.size >= 2)
            {
                PeerId from = ReadU16Le(rp.data);
                if (from != m_selfId && m_customEvent)
                    m_customEvent(from, rp.data + 2, static_cast<uint16_t>(rp.size - 2));
            }
            break;

        case kIdState:
            if (rp.size >= 2 + 1)
            {
                PeerId from = ReadU16Le(rp.data);
                if (from != m_selfId)
                    HandleRemoteState(from, rp.data + 2, static_cast<uint16_t>(rp.size - 2));
            }
            break;

        case kIdMission:
            if (rp.size >= 2 + 1)
            {
                PeerId from = ReadU16Le(rp.data);
                if (from != m_selfId)
                    HandleRemoteMissionState(from, rp.data + 2, static_cast<uint16_t>(rp.size - 2));
            }
            break;

        case kIdEntity:
            if (rp.size >= 2 && m_entitySink)
            {
                PeerId owner = ReadU16Le(rp.data);
                if (owner == m_selfId) break;
                std::vector<EntityStateRecord> recs;
                if (EntityStreamCodec::Decode(rp.data + 2, static_cast<uint16_t>(rp.size - 2), recs))
                    for (const EntityStateRecord& rec : recs) m_entitySink(owner, rec);
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

    bool Session::RequestWorldState(const WorldStateOp& op)
    {
        if (!m_client) return false;
        if (op.kind > WSO_ERASE || op.policy >= WSP_COUNT || op.value.len > WS_MAX_VALUE) return false;
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

    bool Session::SendEntityStates(const std::vector<EntityStateRecord>& records)
    {
        if (!m_client || !m_welcomed) return false;
        size_t first = 0;
        bool ok = true;
        while (first < records.size())
        {
            uint8_t buf[MAX_PACKET_PAYLOAD - 2];
            uint16_t n = EntityStreamCodec::Encode(buf, sizeof(buf), records, first);
            if (n == 0) break;
            ok = m_client->SendTo(kIdEntity, buf, n) && ok;
        }
        return ok;
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

    // =====================================================================
    // streams
    // =====================================================================

    void Session::SendAuthIfDue()
    {
        if (m_authSent) return;
        if (!m_client || !m_client->IsPeerReady()) return; // connection not up yet
        if (m_clientPassword[0] == '\0') { m_authSent = true; return; } // nothing to present

        uint8_t payload[65];
        size_t n = strlen(m_clientPassword);
        if (n > sizeof(payload)) n = sizeof(payload);
        memcpy(payload, m_clientPassword, n);
        m_client->SendTo(kIdAuth, payload, static_cast<uint16_t>(n));
        m_authSent = true;
    }

    void Session::SendLocalStateIfDue()
    {
        if (!m_localState) return;
        if (!IsJoined() || !m_welcomed) return; // unadmitted peers are dropped server-side

        if (++m_stateTick < m_stateSendEvery) return;
        m_stateTick = 0;

        uint16_t size = 0;
        if (!m_localState->GetLocalState(m_stateScratch, sizeof(m_stateScratch), size))
            return;
        if (size == 0) return;

        // Every session (host included, through its loopback client) ships its
        // state to the server, which relays it to the other peers.
        m_client->SendTo(kIdState, m_stateScratch, size);
    }

    void Session::SendMissionStateIfDue()
    {
        if (!m_missionState) return;
        if (!IsJoined() || !m_welcomed) return;

        // Slower, frame-based cadence: default ~every 18 Update frames (≈ every
        // 3rd state send at the default 6-frame player-state cadence) so mission
        // progress does not pile onto every transform snapshot.
        if (++m_missionTick < m_missionSendEvery) return;
        m_missionTick = 0;

        uint16_t size = 0;
        if (!m_missionState->GetMissionState(m_missionScratch, sizeof(m_missionScratch), size))
            return;
        if (size == 0) return;

        m_client->SendTo(kIdMission, m_missionScratch, size);
    }

    bool Session::SendCustomEvent(const uint8_t* data, uint16_t size)
    {
        if (size > MAX_PACKET_PAYLOAD) return false;
        // Every session (host included, via loopback) sends to the server,
        // which relays to everyone else once we are admitted.
        return m_client != nullptr && m_client->SendTo(kIdUser, data, size);
    }

    void Session::HandleRemoteState(PeerId from, const uint8_t* data, uint16_t size)
    {
        if (m_remoteSink) m_remoteSink->ApplyRemoteState(from, data, size);
    }

    void Session::HandleRemoteMissionState(PeerId from, const uint8_t* data, uint16_t size)
    {
        if (m_missionSink) m_missionSink(from, data, size);
    }

    void Session::SetHostPassword(const char* password)
    {
        memset(m_hostPassword, 0, sizeof(m_hostPassword));
        if (password != nullptr)
        {
            size_t n = strlen(password);
            if (n > sizeof(m_hostPassword) - 1) n = sizeof(m_hostPassword) - 1;
            memcpy(m_hostPassword, password, n);
            m_hostPassword[n] = '\0';
        }
        // A freshly configured password only applies to future matches; an
        // active host keeps its current admission state (the roster is already
        // built). Callers set this before StartHost().
    }

    void Session::SetClientPassword(const char* password)
    {
        memset(m_clientPassword, 0, sizeof(m_clientPassword));
        if (password != nullptr)
        {
            size_t n = strlen(password);
            if (n > sizeof(m_clientPassword) - 1) n = sizeof(m_clientPassword) - 1;
            memcpy(m_clientPassword, password, n);
            m_clientPassword[n] = '\0';
        }
    }
}
