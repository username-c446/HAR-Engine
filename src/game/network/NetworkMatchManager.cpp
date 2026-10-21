#include "NetworkMatchManager.h"

NetworkMatchManager* NetworkMatchManager::spInstance = nullptr;

NetworkMatchManager::NetworkMatchManager()
    : m_hostPort(0)
    , m_localStateProvider(nullptr)
    , m_hostMaxPlayers(0)
{
    m_hostPassword[0] = '\0';
}

NetworkMatchManager::~NetworkMatchManager()
{
    Leave();
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
    if (m_server == nullptr) {
        m_server.reset(new NetworkServer());
    }
    if (m_hostMaxPlayers > 0) m_server->SetMaxPlayers(m_hostMaxPlayers);
    if (m_hostPassword[0] != '\0') m_server->SetPassword(m_hostPassword);
    if (!m_server->Start(port)) {
        m_server.reset();
        return false;
    }
    m_hostPort = port;
    return true;
}

void NetworkMatchManager::SetHostConfig(const char* password, unsigned short maxPlayers)
{
    m_hostPassword[0] = '\0';
    if (password != nullptr && password[0] != '\0')
    {
        strncpy(m_hostPassword, password, sizeof(m_hostPassword) - 1);
    }
    m_hostMaxPlayers = maxPlayers;

    // If the server already exists (was created for a host), apply now.
    if (m_server != nullptr)
    {
        if (m_hostMaxPlayers > 0) m_server->SetMaxPlayers(m_hostMaxPlayers);
        if (m_hostPassword[0] != '\0') m_server->SetPassword(m_hostPassword);
    }
}

void NetworkMatchManager::SetClientPassword(const char* password)
{
    if (password != nullptr && password[0] != '\0' && m_client != nullptr)
    {
        m_client->SetPassword(password);
    }
}

bool NetworkMatchManager::JoinHost(const char* serverIp, unsigned short port)
{
    if (IsInMatch()) return false;
    if (m_client == nullptr) {
        m_client.reset(new NetworkClient());
    }
    if (!m_client->Connect(serverIp, port)) {
        m_client.reset();
        return false;
    }
    return true;
}

void NetworkMatchManager::Leave()
{
    if (m_client != nullptr) m_client->Disconnect();
    if (m_server != nullptr) m_server->Stop();
    m_client.reset();
    m_server.reset();
    m_hostPort = 0;
}

const RemotePlayerManager& NetworkMatchManager::GetRemotePlayers() const
{
    if (m_server != nullptr) return m_server->GetRemotePlayers();
    if (m_client != nullptr) return m_client->GetRemotePlayers();
    static RemotePlayerManager empty;
    return empty;
}

void NetworkMatchManager::DrainJoinLeaveEvents(std::vector<NetworkClient::JoinLeaveEvent>& out)
{
    if (m_client != nullptr)
    {
        m_client->DrainJoinLeaveEvents(out);
    }
}

void NetworkMatchManager::Update()
{
    if (m_server != nullptr) {
        m_server->Update();
    }
    if (m_client != nullptr) {
        m_client->Update();
        PlayerNetworkState localState;
        if (m_client->IsServerReady()
            && m_localStateProvider != nullptr
            && m_localStateProvider(localState)) {
            m_client->SendPlayerState(localState);
        }
    }
}