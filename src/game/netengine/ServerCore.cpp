#include "ServerCore.h"

#include <cstring>

namespace net
{
    namespace
    {
        // Transport capacity above the slot cap, so a client over the cap still
        // connects and is told REFUSE_FULL instead of timing out.
        constexpr const uint16_t kRefuseHeadroom = 4;

        inline void WriteU16Le(uint8_t* d, uint16_t v)
        {
            d[0] = static_cast<uint8_t>(v & 0xFF);
            d[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        }
    }

    ServerCore::ServerCore() = default;
    ServerCore::~ServerCore() { Stop(); }

    // =====================================================================
    // lifecycle / configuration
    // =====================================================================

    bool ServerCore::Start(uint16_t port, uint32_t worldSeed)
    {
        if (m_endpoint) return false;
        auto ep = std::make_unique<ServerEndpoint>();
        size_t capacity = m_maxPeers + kRefuseHeadroom;
        if (capacity > 4095) capacity = 4095; // ENet's peer limit
        if (!ep->Start(port, static_cast<uint16_t>(capacity))) return false;

        m_endpoint = std::move(ep);
        m_port = port;
        m_worldSeed = worldSeed;
        m_peers.clear();
        m_admitted.clear();
        m_world.Clear();
        m_ledger.Clear();
        m_stats = Stats();
        m_endpoint->SetPacketSink([this](const ServerEndpoint::RawPacket& rp) { OnPacket(rp); });
        m_endpoint->SetPeerEventFn([this](const PeerEvent& e) { OnPeerEvent(e); });
        return true;
    }

    void ServerCore::Stop()
    {
        if (m_endpoint)
        {
            m_endpoint->SetPacketSink(nullptr);
            m_endpoint->SetPeerEventFn(nullptr);
            m_endpoint->Stop();
            m_endpoint.reset();
        }
        m_port = 0;
        m_peers.clear();
        m_admitted.clear();
        m_world.Clear();
        m_ledger.Clear(); // balances only; counter rules survive
    }

    void ServerCore::Update()
    {
        if (!m_endpoint) return;
        m_endpoint->Update();

        // Handshake timeout: a peer that connected but never presented a valid
        // HELLO is not allowed to hold a slot.
        uint32_t now = NowMs();
        std::vector<PeerId> stale;
        for (const auto& kv : m_peers)
            if (!kv.second.admitted && TimeDiff(now, kv.second.connectedAt) > static_cast<int32_t>(m_handshakeTimeoutMs))
                stale.push_back(kv.first);
        for (PeerId p : stale) Refuse(p, REFUSE_HANDSHAKE);

        // Released entities nobody took over.
        std::vector<WorldStateKey> expired;
        m_world.ExpireClaimable(now, m_claimWindowMs, expired);
        for (const WorldStateKey& key : expired) BroadcastErase(key, 0, 0);
    }

    void ServerCore::SetPassword(const char* password)
    {
        std::memset(m_password, 0, sizeof(m_password));
        if (password == nullptr) return;
        size_t n = std::strlen(password);
        if (n > HandshakeCodec::kMaxPassword) n = HandshakeCodec::kMaxPassword;
        std::memcpy(m_password, password, n);
    }

    void ServerCore::SetSessionOptions(uint32_t options)
    {
        if (options == m_options) return;
        m_options = options;
        if (!m_endpoint) return;
        uint8_t buf[4];
        Writer w(buf, sizeof(buf));
        w.WriteU32(m_options);
        SendToAdmitted(PID_SESSION_INFO, buf, sizeof(buf));
    }

    // =====================================================================
    // admission
    // =====================================================================

    void ServerCore::OnPeerEvent(const PeerEvent& e)
    {
        if (e.connected)
        {
            PeerInfo info;
            info.connectedAt = NowMs();
            info.buckets[LC_STREAM].Configure(m_limits.streamPerSec, m_limits.streamBurst);
            info.buckets[LC_REQUEST].Configure(m_limits.requestPerSec, m_limits.requestBurst);
            info.buckets[LC_USER].Configure(m_limits.userPerSec, m_limits.userBurst);
            info.bytes.Configure(m_limits.bytesPerSec, m_limits.bytesBurst);
            info.violationWindowStart = info.connectedAt;
            m_peers[e.peer] = info;
            return; // admitted only after a valid PID_HELLO
        }

        m_peers.erase(e.peer);
        bool wasAdmitted = m_admitted.erase(e.peer) != 0;
        if (!wasAdmitted) return;
        for (auto& kv : m_peers) kv.second.forwardedAt.erase(e.peer);
        if (m_interest) m_interest->OnPeerLeft(e.peer);

        // Its owner-bound world state goes with it (dynamic entities), and its
        // session balances are dropped.
        std::vector<WorldStateKey> erased;
        std::vector<WorldStateEntry> released;
        m_world.OnPeerLeft(e.peer, erased, &released, NowMs());
        for (const WorldStateKey& key : erased) BroadcastErase(key, 0, 0);
        for (const WorldStateEntry& r : released)
        {
            uint8_t buf[MAX_PACKET_PAYLOAD];
            uint16_t n = WorldStateCodec::EncodeUpdate(buf, sizeof(buf), 0, 0, r);
            if (n) SendToAdmitted(PID_WS_UPDATE, buf, n);
            if (m_worldChange) m_worldChange(r, false, 0);
        }
        m_ledger.OnPeerLeft(e.peer);
        if (m_validator) m_validator->OnPeerLeft(e.peer);

        BroadcastPlayerList();
        if (m_joinLeave) m_joinLeave(e.peer, false);
    }

    void ServerCore::HandleHello(PeerId from, const uint8_t* data, uint16_t size)
    {
        if (IsAdmitted(from)) return; // a repeated HELLO is ignored

        uint32_t magic = 0, gameVersion = 0;
        uint16_t protocol = 0;
        char password[HandshakeCodec::kMaxPassword + 1];
        if (!HandshakeCodec::DecodeHello(data, size, magic, protocol, gameVersion, password)
            || magic != PROTOCOL_MAGIC || protocol != PROTOCOL_VERSION)
        {
            Refuse(from, REFUSE_PROTOCOL);
            return;
        }
        if (m_gameVersion != 0 && gameVersion != m_gameVersion)
        {
            Refuse(from, REFUSE_GAME_VERSION);
            return;
        }
        if (m_password[0] != '\0' && std::strcmp(password, m_password) != 0)
        {
            Refuse(from, REFUSE_PASSWORD);
            return;
        }
        if (m_admitted.size() >= m_maxPeers)
        {
            Refuse(from, REFUSE_FULL);
            return;
        }
        if (m_admission && !m_admission(from))
        {
            Refuse(from, REFUSE_DENIED);
            return;
        }
        Admit(from);
    }

    void ServerCore::Refuse(PeerId peer, RefuseReason reason)
    {
        if (!m_endpoint) return;
        uint8_t buf[3];
        Writer w(buf, sizeof(buf));
        w.WriteU8(reason);
        w.WriteU16(PROTOCOL_VERSION);
        m_endpoint->SendTo(peer, PID_REFUSE, buf, sizeof(buf));
        ++m_stats.refused;
        // KickPeer reports the departure synchronously (OnPeerEvent cleans up).
        m_endpoint->KickPeer(peer);
    }

    void ServerCore::Kick(PeerId peer, RefuseReason reason)
    {
        if (m_peers.count(peer)) Refuse(peer, reason);
    }

    void ServerCore::Admit(PeerId peer)
    {
        auto it = m_peers.find(peer);
        if (it == m_peers.end() || it->second.admitted) return;
        it->second.admitted = true;

        uint8_t welcome[10];
        uint16_t n = HandshakeCodec::EncodeWelcome(welcome, sizeof(welcome), peer, NowMs(), m_options);
        m_endpoint->SendTo(peer, PID_WELCOME, welcome, n);

        uint8_t seed[4];
        Writer w(seed, sizeof(seed));
        w.WriteU32(m_worldSeed);
        m_endpoint->SendTo(peer, PID_WORLD_SEED, seed, sizeof(seed));

        m_admitted.insert(peer);
        BroadcastPlayerList();
        if (m_joinLeave) m_joinLeave(peer, true);

        // The reliable channel is ordered, so the joiner sees the whole world
        // before any live change that follows.
        SendWorldSnapshot(peer);
    }

    void ServerCore::BroadcastPlayerList()
    {
        std::vector<uint8_t> list(2 + m_admitted.size() * 2, 0);
        WriteU16Le(list.data(), static_cast<uint16_t>(m_admitted.size()));
        uint16_t idx = 0;
        for (PeerId id : m_admitted)
        {
            WriteU16Le(list.data() + 2 + idx * 2, id);
            ++idx;
        }
        SendToAdmitted(PID_PLAYER_LIST, list.data(), static_cast<uint16_t>(list.size()));
    }

    void ServerCore::SendWorldSnapshot(PeerId peer)
    {
        std::vector<WorldStateEntry> entries;
        entries.reserve(m_world.Size());
        m_world.ForEach([&](const WorldStateEntry& e) { entries.push_back(e); });

        std::vector<std::vector<uint8_t>> chunks;
        WorldStateCodec::EncodeSnapshot(entries, MAX_PACKET_PAYLOAD, chunks);
        for (const auto& c : chunks)
            m_endpoint->SendTo(peer, PID_WS_SNAPSHOT, c.data(), static_cast<uint16_t>(c.size()));

        uint8_t end[4];
        Writer w(end, sizeof(end));
        w.WriteU32(static_cast<uint32_t>(entries.size()));
        m_endpoint->SendTo(peer, PID_WS_SNAPSHOT_END, end, sizeof(end));
    }

    // =====================================================================
    // packets
    // =====================================================================

    bool ServerCore::AllowPacket(PeerId from, PacketId id, uint16_t size)
    {
        auto it = m_peers.find(from);
        if (it == m_peers.end()) return false;
        PeerInfo& info = it->second;

        LimitClass cls = LC_REQUEST;
        if (ChannelOf(id) == CH_STREAM) cls = LC_STREAM;
        else if (id == PID_USER) cls = LC_USER;

        uint32_t now = NowMs();
        if (info.buckets[cls].Take(now) && info.bytes.Take(now, static_cast<float>(size) + 4.0f))
            return true;

        ++m_stats.rateDropped;
        if (TimeDiff(now, info.violationWindowStart) > static_cast<int32_t>(m_limits.violationWindowMs))
        {
            info.violationWindowStart = now;
            info.violations = 0;
        }
        if (++info.violations >= m_limits.violationsToKick && m_limits.violationsToKick != 0)
        {
            ++m_stats.floodKicks;
            Refuse(from, REFUSE_FLOOD);
        }
        return false;
    }

    void ServerCore::OnPacket(const ServerEndpoint::RawPacket& rp)
    {
        if (rp.id == PID_HELLO)
        {
            HandleHello(rp.from, rp.data, rp.size);
            return;
        }

        // Everything else requires an admitted peer, within its budget.
        if (!IsAdmitted(rp.from)) return;
        if (!AllowPacket(rp.from, rp.id, rp.size)) return;

        switch (rp.id)
        {
        case PID_USER:
        case PID_USER_STREAM:
            // Open user-data relay: the server does not interpret the bytes.
            if (m_userObserver) m_userObserver(rp.from, rp.data, rp.size);
            Relay(rp.id, rp.from, rp.data, rp.size);
            break;

        case PID_PLAYER_STATE:
            HandleState(rp.from, rp.data, rp.size);
            break;

        case PID_MISSION_STATE:
            RelayStream(PID_MISSION_STATE, rp.from, rp.data, rp.size);
            break;

        case PID_ENTITY_STATE:
            RelayEntityStates(rp.from, rp.data, rp.size);
            break;

        case PID_ENTITY_EVENT:
            HandleEntityEvent(rp.from, rp.data, rp.size);
            break;

        case PID_PLAYER_EVENT:
            HandlePlayerEvent(rp.from, rp.data, rp.size);
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

    void ServerCore::SendToAdmitted(PacketId id, const uint8_t* data, uint16_t size, PeerId omit)
    {
        // Never Broadcast on the endpoint: peers still in the handshake must
        // not see the world. One shared packet for all recipients.
        m_to.clear();
        for (PeerId p : m_admitted)
            if (p != omit) m_to.push_back(p);
        if (!m_to.empty()) m_endpoint->SendToMany(m_to.data(), m_to.size(), id, data, size);
    }

    void ServerCore::Relay(PacketId id, PeerId from, const uint8_t* data, uint16_t size)
    {
        // [sender u16][payload], to every admitted peer except the sender.
        uint8_t framed[2 + MAX_PACKET_PAYLOAD];
        if (size + 2 > MAX_PACKET_PAYLOAD) return;
        WriteU16Le(framed, from);
        std::memcpy(framed + 2, data, size);
        SendToAdmitted(id, framed, static_cast<uint16_t>(size + 2), from);
    }

    uint32_t ServerCore::ClampStreamTime(uint32_t t) const
    {
        // The time is the sender's estimate of OUR clock; it may lag (bounded)
        // but never lead it.
        uint32_t now = NowMs();
        if (TimeDiff(t, now) > 0) return now;
        if (TimeDiff(now, t) > static_cast<int32_t>(kMaxStreamLagMs)) return now - kMaxStreamLagMs;
        return t;
    }

    uint16_t ServerCore::FrameStream(PeerId from, uint32_t t, const uint8_t* payload, uint16_t size, uint8_t* out) const
    {
        // Relayed stream: [u16 from][u32 time][payload].
        if (static_cast<uint32_t>(size) + kStreamRelayHeader > MAX_PACKET_PAYLOAD) return 0;
        Writer w(out, MAX_PACKET_PAYLOAD);
        w.WriteU16(from);
        w.WriteU32(t);
        if (size > 0) w.WriteBytes(payload, size);
        return w.Ok() ? w.Position() : 0;
    }

    void ServerCore::RelayStream(PacketId id, PeerId from, const uint8_t* data, uint16_t size)
    {
        // Client stream: [u32 sample time][payload], to every other peer.
        if (size < kStreamClientHeader) return;
        Reader r(data, size);
        uint32_t t = 0;
        r.ReadU32(t);
        uint8_t framed[MAX_PACKET_PAYLOAD];
        uint16_t n = FrameStream(from, ClampStreamTime(t), data + kStreamClientHeader,
                                 static_cast<uint16_t>(size - kStreamClientHeader), framed);
        if (n) SendToAdmitted(id, framed, n, from);
    }

    void ServerCore::RelayPlayerState(PeerId from, const uint8_t* data, uint16_t size)
    {
        // Like RelayStream, but each recipient gets the sample only if the
        // interest policy finds it relevant, at most once per the interval it
        // asks for (a distant player at a lower rate).
        if (size < kStreamClientHeader) return;
        Reader r(data, size);
        uint32_t t = 0;
        r.ReadU32(t);
        t = ClampStreamTime(t);
        uint8_t framed[MAX_PACKET_PAYLOAD];
        uint16_t n = FrameStream(from, t, data + kStreamClientHeader,
                                 static_cast<uint16_t>(size - kStreamClientHeader), framed);
        if (!n) return;

        m_to.clear();
        for (PeerId p : m_admitted)
        {
            if (p == from) continue;
            if (m_interest)
            {
                uint32_t interval = m_interest->PlayerStateIntervalMs(p, from);
                if (interval == IInterestPolicy::kNotRelevant)
                {
                    ++m_stats.samplesFiltered;
                    continue;
                }
                auto info = m_peers.find(p);
                if (interval > 0 && info != m_peers.end())
                {
                    // A tenth of the interval of slack absorbs the sender's
                    // cadence jitter (a 200 ms tier stays 5 Hz at 20 Hz input).
                    auto last = info->second.forwardedAt.find(from);
                    if (last != info->second.forwardedAt.end())
                    {
                        int32_t since = TimeDiff(t, last->second);
                        if (since >= 0 && since < static_cast<int32_t>(interval - interval / 10))
                        {
                            ++m_stats.samplesFiltered;
                            continue;
                        }
                    }
                    info->second.forwardedAt[from] = t;
                }
            }
            m_to.push_back(p);
        }
        if (!m_to.empty()) m_endpoint->SendToMany(m_to.data(), m_to.size(), PID_PLAYER_STATE, framed, n);
    }

    void ServerCore::HandleState(PeerId from, const uint8_t* data, uint16_t size)
    {
        if (size <= kStreamClientHeader) return;
        if (m_validator)
        {
            std::vector<uint8_t> correction;
            StateVerdict v = m_validator->ValidatePeerState(from, data + kStreamClientHeader,
                                                            static_cast<uint16_t>(size - kStreamClientHeader),
                                                            NowMs(), correction);
            if (v != SV_ACCEPT)
            {
                ++m_stats.stateDropped;
                if (v == SV_CORRECT && !correction.empty() && correction.size() <= MAX_PACKET_PAYLOAD)
                {
                    ++m_stats.corrections;
                    m_endpoint->SendTo(from, PID_STATE_CORRECTION, correction.data(),
                                       static_cast<uint16_t>(correction.size()));
                }
                return; // the last accepted state stays authoritative
            }
        }
        if (m_interest)
            m_interest->OnPlayerState(from, data + kStreamClientHeader,
                                      static_cast<uint16_t>(size - kStreamClientHeader), NowMs());
        RelayPlayerState(from, data, size);
    }

    void ServerCore::BroadcastErase(const WorldStateKey& key, PeerId origin, uint32_t seq)
    {
        uint8_t buf[MAX_PACKET_PAYLOAD];
        uint16_t n = WorldStateCodec::EncodeErase(buf, sizeof(buf), origin, seq, key);
        if (n) SendToAdmitted(PID_WS_ERASE, buf, n);
        if (m_worldChange)
        {
            WorldStateEntry e;
            e.key = key;
            m_worldChange(e, true, origin);
        }
    }

    void ServerCore::HandleWorldStateRequest(PeerId from, const uint8_t* data, uint16_t size)
    {
        uint32_t seq = 0;
        WorldStateOp op;
        if (!WorldStateCodec::DecodeRequest(data, size, seq, op)) return;

        WorldStateEntry entry;
        bool exists = false;
        WorldStateTable::Result r = m_world.Apply(from, op, entry, exists, NowMs());

        uint8_t buf[MAX_PACKET_PAYLOAD];
        uint16_t n = 0;
        switch (r)
        {
        case WorldStateTable::ACCEPTED:
            if (op.kind == WSO_ERASE)
            {
                BroadcastErase(op.key, from, seq);
            }
            else
            {
                n = WorldStateCodec::EncodeUpdate(buf, sizeof(buf), from, seq, entry);
                if (n) SendToAdmitted(PID_WS_UPDATE, buf, n);
                if (m_worldChange) m_worldChange(entry, false, from);
            }
            break;

        case WorldStateTable::UNCHANGED:
            // Valid but no change: confirm to the requester only.
            if (exists)
            {
                n = WorldStateCodec::EncodeUpdate(buf, sizeof(buf), from, seq, entry);
                if (n) m_endpoint->SendTo(from, PID_WS_UPDATE, buf, n);
            }
            else
            {
                n = WorldStateCodec::EncodeErase(buf, sizeof(buf), from, seq, op.key);
                if (n) m_endpoint->SendTo(from, PID_WS_ERASE, buf, n);
            }
            break;

        case WorldStateTable::REJECTED:
            n = WorldStateCodec::EncodeReject(buf, sizeof(buf), seq, op.key, exists, entry);
            if (n) m_endpoint->SendTo(from, PID_WS_REJECT, buf, n);
            break;
        }
    }

    void ServerCore::HandleLedgerRequest(PeerId from, const uint8_t* data, uint16_t size)
    {
        uint32_t seq = 0;
        CounterId counter = 0;
        uint8_t reason = 0;
        int32_t delta = 0;
        uint64_t claim = 0;
        if (!LedgerCodec::DecodeRequest(data, size, seq, counter, reason, delta, claim)) return;

        int64_t balance = 0;
        bool accepted = m_ledger.Apply(from, counter, reason, delta, claim, balance) == LedgerServer::ACCEPTED;
        uint8_t buf[LedgerCodec::RESULT_SIZE];
        uint16_t n = LedgerCodec::EncodeResult(buf, sizeof(buf), seq, counter, accepted, balance);
        if (n) m_endpoint->SendTo(from, PID_LEDGER_RESULT, buf, n);
    }

    void ServerCore::RelayEntityStates(PeerId from, const uint8_t* data, uint16_t size)
    {
        // [u32 sample time][batch]
        if (size < kStreamClientHeader) return;
        std::vector<EntityStateRecord> in;
        if (!EntityStreamCodec::Decode(data + kStreamClientHeader,
                                       static_cast<uint16_t>(size - kStreamClientHeader), in)) return;

        // Keep only records for entities this peer owns.
        std::vector<EntityStateRecord> owned;
        owned.reserve(in.size());
        for (const EntityStateRecord& rec : in)
        {
            WorldStateKey key;
            key.object = rec.entity;
            key.channel = WS_CHANNEL_ENTITY;
            WorldStateEntry e;
            if (m_world.Find(key, e) && e.policy == WSP_OWNER && e.owner == from)
                owned.push_back(rec);
        }

        if (owned.empty()) return;

        Reader r(data, size);
        uint32_t t = 0;
        r.ReadU32(t);
        t = ClampStreamTime(t);

        if (m_interest == nullptr)
        {
            m_to.clear();
            for (PeerId p : m_admitted)
                if (p != from) m_to.push_back(p);
            if (!m_to.empty()) SendEntityBatches(from, t, owned, m_to.data(), m_to.size());
            return;
        }

        // Each recipient gets the records relevant to it (entities near it).
        std::vector<EntityStateRecord> relevant;
        relevant.reserve(owned.size());
        for (const PeerId& p : m_admitted)
        {
            if (p == from) continue;
            relevant.clear();
            for (const EntityStateRecord& rec : owned)
            {
                if (m_interest->EntityRelevant(p, from, rec)) relevant.push_back(rec);
                else ++m_stats.entityRecordsFiltered;
            }
            if (!relevant.empty()) SendEntityBatches(from, t, relevant, &p, 1);
        }
    }

    void ServerCore::SendEntityBatches(PeerId from, uint32_t t, const std::vector<EntityStateRecord>& records,
                                       const PeerId* to, size_t count)
    {
        // [u16 owner][u32 time][batch], as many packets as the records need.
        size_t first = 0;
        while (first < records.size())
        {
            uint8_t framed[MAX_PACKET_PAYLOAD];
            Writer w(framed, sizeof(framed));
            w.WriteU16(from);
            w.WriteU32(t);
            uint16_t n = EntityStreamCodec::Encode(framed + kStreamRelayHeader,
                                                   MAX_PACKET_PAYLOAD - kStreamRelayHeader,
                                                   records, first);
            if (n == 0) break;
            m_endpoint->SendToMany(to, count, PID_ENTITY_STATE, framed, static_cast<uint16_t>(n + kStreamRelayHeader));
        }
    }

    void ServerCore::HandleEntityEvent(PeerId from, const uint8_t* data, uint16_t size)
    {
        // {u64 entity | u8 kind | payload}: delivered to the entity's owner only
        // (it simulates the entity, so it applies the hit and streams the
        // result). An entity without an owner (orphaned) cannot be hit.
        Reader r(data, size);
        uint64_t entity = 0;
        uint32_t lo = 0, hi = 0;
        if (!r.ReadU32(lo) || !r.ReadU32(hi)) return;
        entity = static_cast<uint64_t>(lo) | static_cast<uint64_t>(hi) << 32;
        WorldStateKey key;
        key.object = entity;
        key.channel = WS_CHANNEL_ENTITY;
        WorldStateEntry e;
        if (!m_world.Find(key, e) || e.policy != WSP_OWNER || e.owner == 0 || e.owner == from) return;
        if (!IsAdmitted(e.owner)) return;

        uint8_t framed[2 + MAX_PACKET_PAYLOAD];
        if (size + 2 > MAX_PACKET_PAYLOAD) return;
        WriteU16Le(framed, from);
        std::memcpy(framed + 2, data, size);
        m_endpoint->SendTo(e.owner, PID_ENTITY_EVENT, framed, static_cast<uint16_t>(size + 2));
    }

    void ServerCore::HandlePlayerEvent(PeerId from, const uint8_t* data, uint16_t size)
    {
        // {u16 target | u8 kind | payload}: a hit on another player. Only with
        // the PvP session option; delivered to the victim, who applies it to
        // its own avatar (and its state stream shows the result to everyone).
        if ((m_options & SO_PVP) == 0) return;
        Reader r(data, size);
        uint16_t target = 0;
        if (!r.ReadU16(target)) return;
        if (target == from || !IsAdmitted(target)) return;

        uint8_t framed[2 + MAX_PACKET_PAYLOAD];
        if (size + 2 > MAX_PACKET_PAYLOAD) return;
        WriteU16Le(framed, from);
        std::memcpy(framed + 2, data, size);
        m_endpoint->SendTo(target, PID_PLAYER_EVENT, framed, static_cast<uint16_t>(size + 2));
    }
}
