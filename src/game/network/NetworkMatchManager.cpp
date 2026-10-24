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
#include "RemotePlayerManager.h"
#include "RemotePlayerWorldView.h"
#include "RemoteVehicleWorldView.h"
#include <chrono>

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

NetworkMatchManager::NetworkMatchManager()
    : m_hostPort(0)
    , m_localStateProvider(nullptr)
    , m_missionStateProvider(nullptr)
    , m_hostMaxPlayers(0)
{
    m_hostPassword[0] = '\0';
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
        m_hostPort = 0;
        m_bridge.reset();
        return false;
    }
m_hostPort = port;

    // Bridge the world views to the session sinks.
    m_bridge->AttachToSession(m_session.get());
    AttachMissionChannel();
    ReattachWorldEventSink();
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

    bool ok = m_session->Join(serverIp, port);
    if (!ok)
    {
        m_bridge.reset();
        return false;
    }
    m_hostPort = port;
    m_bridge->AttachToSession(m_session.get());
    AttachMissionChannel();
    ReattachWorldEventSink();
    return true;
}

void NetworkMatchManager::Leave()
{
    if (m_session)
        m_session->Leave();
    m_hostPort = 0;
    m_remotePlayers.Clear();
    m_remoteMissionStates.clear();
    m_pendingEvents.clear();
    m_pendingWorldEvents.clear();
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

void NetworkMatchManager::ReattachWorldEventSink()
{
    if (!m_session) return;
    m_session->SetCustomEventSink(
        [this](net::PeerId, const uint8_t* data, uint16_t size)
        {
            if (!m_worldEventSink) return;
            auto d = net::WorldEventCodec::Decode(data, size);
            if (d.valid) m_worldEventSink(&d);
        });
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
        strncpy(m_hostPassword, password, sizeof(m_hostPassword) - 1);
    m_hostMaxPlayers = maxPlayers;
}

void NetworkMatchManager::SetClientPassword(const char* password)
{
    // The engine Session does not enforce a password gate in this pass; the
    // legacy auth path is being phased out. Kept for API compatibility.
    (void)password;
}

void NetworkMatchManager::DrainJoinLeaveEvents(std::vector<JoinLeaveEvent>& out)
{
    out.insert(out.end(), m_pendingEvents.begin(), m_pendingEvents.end());
    m_pendingEvents.clear();
}

void NetworkMatchManager::Update()
{
    if (m_session)
    {
        m_session->Update();
        // Keep the bridge's notion of "who am I" in sync with the session.
        // The local peer id is not resolved until the server sends WELCOME
        // after we connect/join (0 before, then the server-assigned slot, which
        // for the host is 0 and for clients is 1,2,3...). If we did not push it
        // into the bridge, GamePlayerBridge would keep its default m_selfId==0
        // on every client and wrongly filter the HOST's relayed state (whose
        // peer id IS 0), silently dropping the one remote player a client should
        // see. See docs/MULTIPLAYER_SYNC_MANIFEST.ru.md, "Индекс аватара vs
        // сетевой peer-id".
        if (m_bridge)
            m_bridge->SetLocalPeerId( static_cast<net::PeerId>(GetLocalClientId()) );
        // World events raised during the join handshake go out now.
        FlushPendingWorldEvents();
    }
}