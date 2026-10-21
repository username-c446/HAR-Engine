#pragma once

#include "NetworkServer.h"
#include "NetworkClient.h"
#include "PlayerNetworkState.h"

#include <memory>
#include <vector>

// Singleton orchestrating an online match on top of NetworkServer/NetworkClient.
// Provides host/join/leave lifecycle and, each Update, pushes the local player's
// state (supplied by the game through SetLocalStateProvider) and ingests remote
// players' states from the network.
//
// The local-state source is decoupled from the game singletons so the manager
// stays unit-testable: the playing context wires up a provider that reads the
// live Avatar.
class NetworkMatchManager
{
public:
    // Returns true and fills out with the current local player state, or false
    // if no state is available yet.
    typedef bool (*LocalStateProvider)(PlayerNetworkState& out);

    static NetworkMatchManager* CreateInstance();
    static NetworkMatchManager* GetInstance();
    static void DestroyInstance();

    bool StartHost(unsigned short port);
    bool JoinHost(const char* serverIp, unsigned short port);
    void Leave();

    void SetLocalStateProvider(LocalStateProvider provider) { m_localStateProvider = provider; }

    // Host configuration: password and slot cap applied to the server when it is
    // next (re)started via StartHost. Empty password / 0 slots = defaults.
    void SetHostConfig(const char* password, unsigned short maxPlayers);
    // Password for joining a protected host (applied to an existing client).
    void SetClientPassword(const char* password);

    bool IsHosting() const { return m_server != nullptr; }
    bool IsJoined() const { return m_client != nullptr && m_client->IsConnected(); }
    bool IsInMatch() const { return m_server != nullptr || (m_client && m_client->IsConnected()); }

    const RemotePlayerManager& GetRemotePlayers() const;
    unsigned short GetLocalClientId() const { return m_client ? m_client->GetClientId() : 0; }
    unsigned short GetHostPort() const { return m_hostPort; }

    // Drains the client's pending join/leave notifications. Empty unless we are
    // a joined client. Lets the UI print "Player connected/disconnected".
    void DrainJoinLeaveEvents(std::vector<NetworkClient::JoinLeaveEvent>& out);

    // Drives the network layer. Call once per game frame from the playing context.
    void Update();

private:
    NetworkMatchManager();
    ~NetworkMatchManager();

    NetworkMatchManager(const NetworkMatchManager&);
    NetworkMatchManager& operator=(const NetworkMatchManager&);

    static NetworkMatchManager* spInstance;

    std::unique_ptr<NetworkServer> m_server;
    std::unique_ptr<NetworkClient> m_client;
    unsigned short m_hostPort;
    LocalStateProvider m_localStateProvider;
    char m_hostPassword[65];
    unsigned short m_hostMaxPlayers;
};