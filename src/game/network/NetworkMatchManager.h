#pragma once

#include "PlayerNetworkState.h"
#include "MissionSyncState.h"
#include "RemotePlayerManager.h"   // placeholder; will be bridged from net::Session
#include "NetWorldRegistry.h"
#include "NetEntityManager.h"
#include "PlayerEconomy.h"
#include "../netengine/Session.h"
#include "../netengine/GamePlayerBridge.h"

#include <memory>
#include <vector>
#include <map>

// Singleton orchestrating an online match. This is a THIN BRIDGE: it keeps the
// same public API the rest of the game already calls (StartHost/JoinHost/Leave/
// IsInMatch/GetRemotePlayers), but its internals delegate to the reusable engine
// layer (net::Session + net::GamePlayerBridge).
//
// It also wires the game's replicated-world services to the session:
//   * NetWorldRegistry  — static world objects  <-> server world state
//   * NetEntityManager  — owner-simulated entities (traffic)
//   * PlayerEconomy     — local coin bank        <-> server ledger
// Those services are process singletons (objects register while a level loads,
// before any match exists) and are reached through the static accessors below.
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

    // Replicated-world services (always available, match or not).
    static NetWorldRegistry& WorldRegistry();
    static NetEntityManager& Entities();
    static PlayerEconomy&    Economy();

    bool StartHost(unsigned short port);
    bool JoinHost(const char* serverIp, unsigned short port);
    void Leave();

    void SetLocalStateProvider(LocalStateProvider provider);
    void SetMissionStateProvider(MissionStateProvider provider);

    // Host configuration applied by the next StartHost. Authorization is
    // optional: an empty password hosts an open session (co-op / session
    // games); a password turns the PID_AUTH gate on. 0 slots = engine default.
    void SetHostConfig(const char* password, unsigned short maxPlayers);
    // Password presented to a protected host (before or after JoinHost).
    void SetClientPassword(const char* password);
    // Whether the hosted economy accepts cheat credits (debug sessions).
    void SetAllowEconomyCheats(bool allow) { m_allowEconomyCheats = allow; }

    bool IsHosting() const { return m_session && m_session->IsHost(); }
    bool IsJoined() const { return m_session && m_session->IsJoined(); }
    bool IsInMatch() const { return m_session && m_session->IsInMatch(); }
    bool IsWelcomed() const { return m_session && m_session->IsWelcomed(); }
    // True once the late-join world snapshot has been applied.
    bool IsWorldSynced() const { return m_session && m_session->GetWorldStateMirror().IsSynced(); }

    // The authoritative remote-player registry the game reads each frame to
    // draw/interpolate remote players. Maintained by the bridge.
    const RemotePlayerManager& GetRemotePlayers() const { return m_remotePlayers; }
    unsigned short GetLocalClientId() const { return m_session ? m_session->GetLocalPeerId() : 0; }
    unsigned short GetHostPort() const { return m_hostPort; }

    // Remote players' mission progress, delivered via the dedicated
    // PID_MISSION_STATE channel and keyed by peer id. Empty outside a match or
    // before a peer has reported progress.
    const std::map<int, MissionSyncState>& GetRemoteMissionStates() const
    {
        return m_remoteMissionStates;
    }
    bool GetRemoteMissionState(int peerId, MissionSyncState& out) const
    {
        std::map<int, MissionSyncState>::const_iterator it = m_remoteMissionStates.find(peerId);
        if (it == m_remoteMissionStates.end()) return false;
        out = it->second;
        return true;
    }

    // The match's server-authoritative world seed (0 outside a match). The game
    // uses it to seed its world RNG identically on every peer so the static
    // world composition (traffic/peds/parking/props) matches. See
    // Game::GetRandomSeed().
    unsigned int GetWorldSeed() const { return m_session ? m_session->GetWorldSeed() : 0; }

    // Engine session (custom events for mods, diagnostics). Null outside a match.
    net::Session* GetSession() const { return m_session.get(); }

    // Drains pending join/leave notifications (host and joiner) for the UI.
    void DrainJoinLeaveEvents(std::vector<JoinLeaveEvent>& out);

    // Drives the network layer, the world services and the bridge. Call once
    // per game frame (also while paused, so the connection stays alive).
    void Update(float dtSeconds = 1.0f / 60.0f);

private:
    NetworkMatchManager();
    ~NetworkMatchManager();

    NetworkMatchManager(const NetworkMatchManager&);
    NetworkMatchManager& operator=(const NetworkMatchManager&);

    // Session created: attach the world views, mission channel, replicated
    // world services and join/leave notifications.
    void AttachSession();

    // Wire the dedicated mission-progress channel (PID_MISSION_STATE):
    // attach the adapter as the session's IMissionStateProvider and register the
    // sink that fills m_remoteMissionStates. Called after the session exists.
    void AttachMissionChannel();

    // Adapter class for bridging the typed MissionStateProvider to net::Session's
    // opaque IMissionStateProvider. Defined in the .cpp; forward-declared here.
    class GameMissionAdapter;
    // Delivers a remote mission-progress blob (decoded) into m_remoteMissionStates.
    void HandleRemoteMissionState(int peerId, const MissionSyncState& state);

    static NetworkMatchManager* spInstance;

    // Bridge to the reusable engine network layer.
    std::unique_ptr<net::Session> m_session;
    std::unique_ptr<net::GamePlayerBridge> m_bridge;
    std::unique_ptr<GameMissionAdapter> m_missionAdapter;

    unsigned short m_hostPort;
    LocalStateProvider m_localStateProvider;
    MissionStateProvider m_missionStateProvider;
    char m_hostPassword[65];
    char m_clientPassword[65];
    unsigned short m_hostMaxPlayers;
    bool m_allowEconomyCheats;
    bool m_entitiesWelcomed;

    // Game-side authoritative remote set, fed by the bridge.
    RemotePlayerManager m_remotePlayers;
    std::vector<JoinLeaveEvent> m_pendingEvents;

    // Remote players' mission progress, keyed by peer id (PID_MISSION_STATE).
    std::map<int, MissionSyncState> m_remoteMissionStates;
};
