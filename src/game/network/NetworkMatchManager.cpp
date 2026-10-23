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

    bool ok = m_session->StartHost(port, 0x5EED0000u | static_cast<uint32_t>(port));
    if (!ok)
    {
        m_hostPort = 0;
        m_bridge.reset();
        return false;
    }
m_hostPort = port;

    // Bridge the world views to the session sinks.
    m_bridge->AttachToSession(m_session.get());
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
    return true;
}

void NetworkMatchManager::Leave()
{
    if (m_session)
        m_session->Leave();
    m_hostPort = 0;
    m_remotePlayers.Clear();
    m_pendingEvents.clear();
}

void NetworkMatchManager::SetLocalStateProvider(LocalStateProvider provider)
{
    m_localStateProvider = provider;
    if (m_bridge)
        m_bridge->SetTypedLocalProvider(provider);
}

void NetworkMatchManager::SetMissionStateProvider(MissionStateProvider provider)
{
    // Mission sync is still carried by the legacy MissionSyncManager path; this
    // hook is preserved for API compatibility (mission relay is not tied to the
    // engine Session yet).
    (void)provider;
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
        m_session->Update();
}