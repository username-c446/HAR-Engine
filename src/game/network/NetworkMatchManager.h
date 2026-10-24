#pragma once

#include "PlayerNetworkState.h"
#include "MissionSyncState.h"
#include "RemotePlayerManager.h"   // placeholder; will be bridged from net::Session
#include "NetWorldRegistry.h"
#include "NetEntityManager.h"
#include "PlayerEconomy.h"
#include "GameServer.h"
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

    // The server refused our last state (movement validator): move the local
    // player back to this authoritative state.
    typedef void (*StateCorrectionHandler)(const PlayerNetworkState& authoritative);

    // Player-vs-player hits (PID_PLAYER_EVENT, relayed only with SO_PVP). The
    // payload is a NetEntityHit: for PE_VEHICLE_HIT the impulse on the
    // victim's car, for PE_KNOCKDOWN the knock velocity of the victim.
    enum PlayerEventKind
    {
        PE_VEHICLE_HIT = 1,
        PE_KNOCKDOWN   = 2,
    };
    typedef void (*PlayerHitHandler)(unsigned short fromPeer, unsigned char kind, const net::NetEntityHit& hit);
    // Game-side lookups: which remote player a ghost machine / puppet belongs
    // to (0 = none).
    typedef int (*RemoteHandleLookup)(void* handle);

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
    void SetStateCorrectionHandler(StateCorrectionHandler handler) { m_correctionHandler = handler; }
    void SetPlayerHitHandler(PlayerHitHandler handler) { m_playerHitHandler = handler; }
    void SetRemoteLookups(RemoteHandleLookup vehicleDriver, RemoteHandleLookup puppetPlayer)
    {
        m_vehicleDriverLookup = vehicleDriver;
        m_puppetPlayerLookup = puppetPlayer;
    }

    // Our car hit a remote player's ghost machine: with PvP the hit goes to
    // that player. True if `ghost` is a remote player's machine (whatever PvP).
    bool ReportRemoteVehicleHit(void* ghost, const net::NetEntityHit& hit);
    // Our attack hit a remote player's puppet: with PvP the knock goes to that
    // player. True if `puppet` is a remote player (the caller must then not
    // apply the hit to the puppet locally — its owner's stream shows it).
    bool ReportRemotePlayerHit(void* puppet, const net::NetEntityHit& hit);

    // Host configuration applied by the next StartHost. Authorization is
    // optional: an empty password hosts an open session (co-op / session
    // games); a password turns the PID_AUTH gate on. 0 slots = engine default.
    void SetHostConfig(const char* password, unsigned short maxPlayers);
    // Password presented to a protected host (before or after JoinHost).
    void SetClientPassword(const char* password);
    // Whether the hosted economy accepts cheat credits (debug sessions).
    void SetAllowEconomyCheats(bool allow) { m_serverOptions.allowEconomyCheats = allow; }
    // Server rules of a hosted match: PvP, server Lua mods (the same GameServer
    // the dedicated server runs).
    void SetHostPvp(bool pvp);
    void SetHostMods(const std::vector<std::string>& scripts) { m_serverOptions.modScripts = scripts; }
    // PvP as announced by the server of the current match.
    bool IsPvpEnabled() const { return m_session && (m_session->GetSessionOptions() & net::SO_PVP) != 0; }

    // Why the last match ended on its own (refused by the server / lost
    // connection); REFUSE_NONE + !WasConnectionLost() after a normal Leave.
    net::RefuseReason GetLastRefusal() const { return m_lastRefusal; }
    bool WasConnectionLost() const { return m_lastLost; }

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

    // True if a remote player is within `radius` of (x,z) on the ground plane
    // (entity hand-over decisions).
    bool IsRemotePlayerNear(float x, float z, float radius) const;

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
    // Shared teardown of the game-side services.
    void ResetServices();

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
    StateCorrectionHandler m_correctionHandler;
    PlayerHitHandler m_playerHitHandler;
    RemoteHandleLookup m_vehicleDriverLookup;
    RemoteHandleLookup m_puppetPlayerLookup;
    char m_hostPassword[65];
    char m_clientPassword[65];
    unsigned short m_hostMaxPlayers;
    bool m_entitiesWelcomed;
    net::RefuseReason m_lastRefusal;
    bool m_lastLost;

    // Server rules when hosting (shared with the dedicated server).
    GameServer::Options m_serverOptions;
    GameServer m_gameServer;

    // Game-side authoritative remote set, fed by the bridge.
    RemotePlayerManager m_remotePlayers;
    std::vector<JoinLeaveEvent> m_pendingEvents;

    // Remote players' mission progress, keyed by peer id (PID_MISSION_STATE).
    std::map<int, MissionSyncState> m_remoteMissionStates;
};
