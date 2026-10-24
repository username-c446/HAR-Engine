#pragma once

#include "PlayerNetworkState.h"
#include "MissionSyncState.h"
#include "RemotePlayerManager.h"   // placeholder; will be bridged from net::Session
#include "../netengine/Session.h"
#include "../netengine/GamePlayerBridge.h"

#include <memory>
#include <vector>

// Singleton orchestrating an online match. This is a THIN BRIDGE: it keeps the
// same public API the rest of the game already calls (StartHost/JoinHost/Leave/
// IsInMatch/GetRemotePlayers), but its internals delegate to the reusable engine
// layer (net::Session + net::GamePlayerBridge) instead of the old
// NetworkServer/NetworkClient stack.
//
// It stays as a process singleton so consumers (gameplay, console, main menu,
// missions, vehicles) do not need to change while the bridge is being rolled
// out. After the wiring is proven, this wrapper can be removed and consumers
// switched to net::Session directly.
class NetworkMatchManager
{
public:
    // Join/leave notification consumed via DrainJoinLeaveEvents.
    struct JoinLeaveEvent
    {
        unsigned short clientId;
        bool joined;
    };

    // Returns true and fills out with the current local player state, or false
    // if no state is available yet. (This is the game-side typed provider that
    // net::GamePlayerBridge adapts into an opaque blob for net::Session.)
    typedef bool (*LocalStateProvider)(PlayerNetworkState& out);

    // Returns true and fills out with the local player's mission progress.
    typedef bool (*MissionStateProvider)(MissionSyncState& out);

    static NetworkMatchManager* CreateInstance();
    static NetworkMatchManager* GetInstance();
    static void DestroyInstance();

    bool StartHost(unsigned short port);
    bool JoinHost(const char* serverIp, unsigned short port);
    void Leave();

    void SetLocalStateProvider(LocalStateProvider provider);
    void SetMissionStateProvider(MissionStateProvider provider);

    // Host configuration: password and slot cap applied to the server when it is
    // next (re)started via StartHost. Empty password / 0 slots = defaults.
    void SetHostConfig(const char* password, unsigned short maxPlayers);
    // Password for joining a protected host (applied to an existing client).
    void SetClientPassword(const char* password);

    bool IsHosting() const { return m_session && m_session->IsHost(); }
    bool IsJoined() const { return m_session && m_session->IsJoined(); }
    bool IsInMatch() const { return m_session && m_session->IsInMatch(); }

    // The authoritative remote-player registry the game reads each frame to
    // draw/interpolate remote players. Maintained by the bridge.
    const RemotePlayerManager& GetRemotePlayers() const { return m_remotePlayers; }
    unsigned short GetLocalClientId() const { return m_session ? m_session->GetLocalPeerId() : 0; }
    unsigned short GetHostPort() const { return m_hostPort; }

    // The match's server-authoritative world seed (0 outside a match). The game
    // uses it to seed its world RNG identically on every peer so the static
    // world composition (traffic/peds/parking/props) matches. See
    // Game::GetRandomSeed().
    unsigned int GetWorldSeed() const { return m_session ? m_session->GetWorldSeed() : 0; }

    // Drains the client's pending join/leave notifications. Empty unless we are
    // a joined client. Lets the UI print "Player connected/disconnected".
    void DrainJoinLeaveEvents(std::vector<JoinLeaveEvent>& out);

    // Drives the network layer + world bridge. Call once per game frame from
    // the playing context. (World-view interpolation is done by the caller
    // from GetRemotePlayers(), kept as before for the bridge rollout.)
    void Update();

private:
    NetworkMatchManager();
    ~NetworkMatchManager();

    NetworkMatchManager(const NetworkMatchManager&);
    NetworkMatchManager& operator=(const NetworkMatchManager&);

    static NetworkMatchManager* spInstance;

    // Bridge to the reusable engine network layer.
    std::unique_ptr<net::Session> m_session;
    std::unique_ptr<net::GamePlayerBridge> m_bridge;

    unsigned short m_hostPort;
    LocalStateProvider m_localStateProvider;
    MissionStateProvider m_missionStateProvider;
    char m_hostPassword[65];
    unsigned short m_hostMaxPlayers;

    // Game-side authoritative remote set, fed by the bridge.
    RemotePlayerManager m_remotePlayers;
    mutable std::vector<JoinLeaveEvent> m_pendingEvents;
};