#include "NetworkMatchManager.h"

// Bridge: the reusable engine network session (net::Session) + the game-side
// adapter (GamePlayerBridge), keeping the old NetworkMatchManager singleton
// public API so the rest of the game (gameplay, console, main menu, missions,
// vehicles) keeps working unchanged while we roll the engine layer out.
//
// Over time consumers can switch to net::Session directly and this wrapper is
// removed.

#include "../netengine/Session.h"
#include "../netengine/GamePlayerBridge.h"
#include "../netengine/WorldInterfaces.h"
#include "EconomyConfig.h"
#include "RemotePlayerManager.h"
#include "RemotePlayerWorldView.h"
#include "RemoteVehicleWorldView.h"
#include <chrono>
#include <cstring>

// Bridges the game's typed MissionStateProvider ((MissionSyncState&) -> bool)
// to the engine Session's opaque IMissionStateProvider (bytes). Serializes the
// 13-byte MissionSyncState into the buffer the session ships on PID_MISSION_STATE.
class NetworkMatchManager::GameMissionAdapter : public net::IMissionStateProvider
{
public:
    MissionStateProvider typed = nullptr;

    bool GetMissionState(uint8_t* out, uint16_t capacity, uint16_t& size) override
    {
        if (typed == nullptr) return false;
        if (capacity < static_cast<uint16_t>(MissionSyncState::SERIALIZED_SIZE)) return false;
        MissionSyncState st;
        st.Clear();
        if (!typed(st)) return false;
        st.Serialize(reinterpret_cast<char*>(out));
        size = static_cast<uint16_t>(MissionSyncState::SERIALIZED_SIZE);
        return true;
    }
};

NetworkMatchManager* NetworkMatchManager::spInstance = nullptr;

NetWorldRegistry& NetworkMatchManager::WorldRegistry()
{
    static NetWorldRegistry s_registry;
    return s_registry;
}

NetEntityManager& NetworkMatchManager::Entities()
{
    static NetEntityManager s_entities;
    return s_entities;
}

PlayerEconomy& NetworkMatchManager::Economy()
{
    static PlayerEconomy s_economy;
    return s_economy;
}

PlayerPresence& NetworkMatchManager::Presence()
{
    static PlayerPresence s_presence;
    return s_presence;
}

NetworkMatchManager::NetworkMatchManager()
    : m_hostPort(0)
    , m_localStateProvider(nullptr)
    , m_missionStateProvider(nullptr)
    , m_correctionHandler(nullptr)
    , m_playerHitHandler(nullptr)
    , m_vehicleDriverLookup(nullptr)
    , m_puppetPlayerLookup(nullptr)
    , m_hostMaxPlayers(0)
    , m_entitiesWelcomed(false)
    , m_lastRefusal(net::REFUSE_NONE)
    , m_lastLost(false)
{
    m_hostPassword[0] = '\0';
    m_clientPassword[0] = '\0';
}

NetworkMatchManager::~NetworkMatchManager()
{
    Leave();
    m_bridge.reset();
    m_session.reset();
}

NetworkMatchManager* NetworkMatchManager::CreateInstance()
{
    if (spInstance == nullptr) {
        spInstance = new NetworkMatchManager();
    }
    return spInstance;
}

NetworkMatchManager* NetworkMatchManager::GetInstance()
{
    return spInstance;
}

void NetworkMatchManager::DestroyInstance()
{
    delete spInstance;
    spInstance = nullptr;
}

bool NetworkMatchManager::StartHost(unsigned short port)
{
    if (IsInMatch()) return false;
    if (m_session == nullptr)
        m_session.reset(new net::Session());
    if (m_bridge == nullptr)
        m_bridge.reset(new net::GamePlayerBridge());

    // The game's authoritative remote set lives here; give it to the bridge so
    // incoming blobs update it.
    m_bridge->SetTypedLocalProvider(m_localStateProvider);
    m_bridge->SetRemotePlayers(&m_remotePlayers);

    // Host configuration: optional authorization, slot cap (+1 for the host's
    // own loopback client), and the game's server rules (economy, movement
    // validation, mods, PvP) — the same GameServer a dedicated server runs.
    m_session->SetHostPassword(m_hostPassword[0] != '\0' ? m_hostPassword : nullptr);
    m_session->SetMaxPeers(m_hostMaxPlayers ? static_cast<size_t>(m_hostMaxPlayers) + 1 : 0);
    m_gameServer.Attach(m_session->GetServerCore(), m_serverOptions);

    // Session-scoped world seed: randomized per match so every host differs, and
    // broadcast to clients in WELCOME so all peers seed their world RNG
    // identically. Steady clock is a good entropy source (differs between
    // matches; the upper bits make same-second matches distinct) and needs no
    // platform SDK so the network layer stays reusable/testable.
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    uint32_t worldSeed = (static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count()) ^ 0x5EED0000u) & 0x7FFFFFFFu;
    if (worldSeed == 0) worldSeed = 0x5EED0000u;
    bool ok = m_session->StartHost(port, worldSeed);
    if (!ok)
    {
        m_gameServer.Detach();
        m_hostPort = 0;
        m_bridge.reset();
        return false;
    }
    m_hostPort = port;
    AttachSession();
    return true;
}

bool NetworkMatchManager::JoinHost(const char* serverIp, unsigned short port)
{
    if (IsInMatch()) return false;
    if (m_session == nullptr)
        m_session.reset(new net::Session());
    if (m_bridge == nullptr)
        m_bridge.reset(new net::GamePlayerBridge());

    m_bridge->SetTypedLocalProvider(m_localStateProvider);
    m_bridge->SetRemotePlayers(&m_remotePlayers);
    m_session->SetPassword(m_clientPassword[0] != '\0' ? m_clientPassword : nullptr);

    bool ok = m_session->Join(serverIp, port);
    if (!ok)
    {
        m_bridge.reset();
        return false;
    }
    m_hostPort = port;
    AttachSession();
    return true;
}

void NetworkMatchManager::AttachSession()
{
    net::Session* s = m_session.get();

    // Bridge the world views to the session sinks (the session reports a
    // departure to the bridge itself), then our join/leave notifications.
    m_lastRefusal = net::REFUSE_NONE;
    m_lastLost = false;
    m_bridge->AttachToSession(s);
    s->SetJoinLeaveFn([this](net::PeerId peer, bool joined)
    {
        JoinLeaveEvent ev;
        ev.clientId = static_cast<unsigned short>(peer);
        ev.joined = joined;
        m_pendingEvents.push_back(ev);
    });
    AttachMissionChannel();

    // Replicated world: every authoritative decision goes to both services
    // (each ignores what is not its own: static ids vs. dynamic entities).
    s->SetWorldStateSink([](const net::WorldStateEntry& e, net::WorldStateReason reason, net::PeerId)
    {
        WorldRegistry().OnAuthoritative(e, reason);
        Entities().OnAuthoritative(e, reason);
        Presence().OnAuthoritative(e, reason);
    });
    s->SetEntityStateSink([](net::PeerId owner, const net::EntityStateRecord& rec, uint32_t serverTimeMs)
    {
        Entities().OnEntityState(owner, rec, serverTimeMs);
    });
    s->SetStateCorrectionSink([this](const uint8_t* data, uint16_t size)
    {
        if (m_correctionHandler == nullptr || size < PlayerNetworkState::SERIALIZED_SIZE) return;
        PlayerNetworkState st;
        st.Deserialize(reinterpret_cast<const char*>(data));
        m_correctionHandler(st);
    });
    s->SetLedgerSink([s](const net::LedgerOutcome& o)
    {
        Economy().OnOutcome(o, s->GetLedger().Pending());
    });

    s->SetPlayerEventSink([this](net::PeerId from, uint8_t kind, const uint8_t* data, uint16_t size)
    {
        net::NetEntityHit hit;
        if (m_playerHitHandler == nullptr || !hit.Decode(data, size)) return;
        m_playerHitHandler(from, kind, hit);
    });
    s->SetEntityEventSink([](net::PeerId from, uint64_t entity, uint8_t kind, const uint8_t* data, uint16_t size)
    {
        Entities().OnEntityEvent(from, entity, kind, data, size);
    });

    WorldRegistry().SetSender([s](const net::WorldStateOp& op) { return s->RequestWorldState(op); });
    Entities().SetSenders(
        [s](const net::WorldStateOp& op) { return s->RequestWorldState(op); },
        [s](const std::vector<net::EntityStateRecord>& recs) { return s->SendEntityStates(recs); },
        [s](uint64_t entity, uint8_t kind, const uint8_t* data, uint16_t size)
        {
            return s->SendEntityEvent(entity, kind, data, size);
        });
    Entities().SetClock([s]() { return s->ServerTimeMs(); });
    Presence().SetSender([s](const net::WorldStateOp& op) { return s->RequestWorldState(op); });
    Economy().SetSender([s](net::CounterId c, uint8_t reason, int32_t delta, uint64_t claim)
    {
        return s->RequestLedgerChange(c, reason, delta, claim);
    });

    // The opening balance is the first request in the (ordered) outbox, so the
    // server knows it before any change.
    Economy().OnMatchStart(economy::COUNTER_COINS);
    m_entitiesWelcomed = false;
}

void NetworkMatchManager::Leave()
{
    if (m_session)
        m_session->Leave();
    m_lastRefusal = net::REFUSE_NONE;
    m_lastLost = false;
    ResetServices();
}

void NetworkMatchManager::ResetServices()
{
    m_gameServer.Detach();
    m_hostPort = 0;
    m_remotePlayers.Clear();
    m_remoteMissionStates.clear();
    m_pendingEvents.clear();

    WorldRegistry().SetSender(NetWorldRegistry::SendFn());
    WorldRegistry().ClearState();
    Entities().SetSenders(NetEntityManager::SendOpFn(), NetEntityManager::SendStatesFn());
    Entities().SetClock(NetEntityManager::ClockFn());
    Presence().SetSender(PlayerPresence::SendFn());
    Presence().OnMatchEnd();
    Entities().OnMatchEnd();
    Economy().SetSender(PlayerEconomy::SendFn());
    Economy().OnMatchEnd();
    m_entitiesWelcomed = false;
}

void NetworkMatchManager::AttachMissionChannel()
{
    if (!m_session) return;
    // Set up the adapter for the typed provider if it wasn't created yet.
    if (!m_missionAdapter)
        m_missionAdapter.reset(new GameMissionAdapter());
    m_missionAdapter->typed = m_missionStateProvider;

    m_session->SetMissionStateProvider(m_missionAdapter.get());
    m_session->SetMissionStateSink(
        [this](net::PeerId from, const uint8_t* data, uint16_t size)
        {
            if (size < static_cast<uint16_t>(MissionSyncState::SERIALIZED_SIZE)) return;
            MissionSyncState st;
            st.Deserialize(reinterpret_cast<const char*>(data));
            HandleRemoteMissionState(static_cast<int>(from), st);
        });
}

void NetworkMatchManager::HandleRemoteMissionState(int peerId, const MissionSyncState& state)
{
    m_remoteMissionStates[peerId] = state;
}

void NetworkMatchManager::SetLocalStateProvider(LocalStateProvider provider)
{
    m_localStateProvider = provider;
    if (m_bridge)
        m_bridge->SetTypedLocalProvider(provider);
}

void NetworkMatchManager::SetMissionStateProvider(MissionStateProvider provider)
{
    m_missionStateProvider = provider;
    // Propagate to the session adapter immediately if it already exists (e.g.
    // set before StartHost/JoinHost, or re-set mid-match). AttachMissionChannel
    // also reads m_missionStateProvider when the session is created.
    if (m_missionAdapter)
        m_missionAdapter->typed = provider;
}

void NetworkMatchManager::SetHostConfig(const char* password, unsigned short maxPlayers)
{
    m_hostPassword[0] = '\0';
    if (password != nullptr && password[0] != '\0')
    {
        strncpy(m_hostPassword, password, sizeof(m_hostPassword) - 1);
        m_hostPassword[sizeof(m_hostPassword) - 1] = '\0';
    }
    m_hostMaxPlayers = maxPlayers;
}

void NetworkMatchManager::SetHostPvp(bool pvp)
{
    m_serverOptions.pvp = pvp;
    if (IsHosting()) m_gameServer.SetPvp(pvp);
}

void NetworkMatchManager::SetClientPassword(const char* password)
{
    m_clientPassword[0] = '\0';
    if (password != nullptr && password[0] != '\0')
    {
        strncpy(m_clientPassword, password, sizeof(m_clientPassword) - 1);
        m_clientPassword[sizeof(m_clientPassword) - 1] = '\0';
    }
    // Applies to an existing client too: it answers the host's PID_AUTH gate.
    if (m_session) m_session->SetPassword(m_clientPassword[0] != '\0' ? m_clientPassword : nullptr);
}

bool NetworkMatchManager::ReportRemoteVehicleHit(void* ghost, const net::NetEntityHit& hit)
{
    int peer = m_vehicleDriverLookup ? m_vehicleDriverLookup(ghost) : 0;
    if (peer <= 0) return false;
    if (IsPvpEnabled())
    {
        uint8_t buf[net::NetEntityHit::SIZE];
        uint16_t n = hit.Encode(buf, sizeof(buf));
        if (n) m_session->SendPlayerEvent(static_cast<net::PeerId>(peer), PE_VEHICLE_HIT, buf, n);
    }
    return true;
}

bool NetworkMatchManager::ReportRemotePlayerHit(void* puppet, const net::NetEntityHit& hit)
{
    int peer = m_puppetPlayerLookup ? m_puppetPlayerLookup(puppet) : 0;
    if (peer <= 0) return false;
    if (IsPvpEnabled())
    {
        uint8_t buf[net::NetEntityHit::SIZE];
        uint16_t n = hit.Encode(buf, sizeof(buf));
        if (n) m_session->SendPlayerEvent(static_cast<net::PeerId>(peer), PE_KNOCKDOWN, buf, n);
    }
    return true;
}

bool NetworkMatchManager::IsRemotePlayerNear(float x, float z, float radius) const
{
    struct Query { float x, z, r2; bool found; } q = { x, z, radius * radius, false };
    m_remotePlayers.ForEachPlayer(
        [](int, const PlayerNetworkState& st, void* user) -> bool
        {
            Query* q = static_cast<Query*>(user);
            float dx = st.x - q->x, dz = st.z - q->z;
            if (dx * dx + dz * dz <= q->r2) { q->found = true; return false; }
            return true;
        },
        &q);
    return q.found;
}

void NetworkMatchManager::DrainJoinLeaveEvents(std::vector<JoinLeaveEvent>& out)
{
    out.insert(out.end(), m_pendingEvents.begin(), m_pendingEvents.end());
    m_pendingEvents.clear();
}

void NetworkMatchManager::Update(float dtSeconds)
{
    if (!m_session) return;
    m_session->Update();

    // The match ended on its own: refused by the server or connection lost.
    if (m_session->GetRefusal() != net::REFUSE_NONE || m_session->IsConnectionLost())
    {
        net::RefuseReason refusal = m_session->GetRefusal();
        bool lost = m_session->IsConnectionLost();
        m_session->Leave();
        ResetServices();
        m_lastRefusal = refusal;
        m_lastLost = lost;
        return;
    }

    // Keep the bridge's notion of "who am I" in sync with the session. The
    // local peer id is resolved by WELCOME (0 before, then the server-assigned
    // slot >= 1). Without this, GamePlayerBridge would filter relayed state
    // against a stale id. See docs/MULTIPLAYER_SYNC_MANIFEST.ru.md.
    if (m_bridge)
        m_bridge->SetLocalPeerId(static_cast<net::PeerId>(GetLocalClientId()));

    // Owned entities can only be announced once our peer id is final.
    if (!m_entitiesWelcomed && m_session->IsWelcomed())
    {
        Entities().OnWelcomed(m_session->GetLocalPeerId());
        Presence().OnWelcomed(m_session->GetLocalPeerId());
        m_entitiesWelcomed = true;
    }

    WorldRegistry().Pump();
    Entities().Update(dtSeconds);
}
