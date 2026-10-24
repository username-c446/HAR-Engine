#pragma once

#include "PlayerNetworkState.h"
#include "MissionSyncState.h"
#include "RemotePlayerManager.h"   // placeholder; will be bridged from net::Session
#include "WorldEvent.h"
#include "../netengine/Session.h"
#include "../netengine/GamePlayerBridge.h"

#include <memory>
#include <vector>
#include <map>

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

    // ---- world event relay (over the open PID_USER channel) ----------------

    // Broadcasts a shared-world event to all other peers (relayed server-side).
    // The engine treats the payload as opaque bytes; WorldEventCodec adds the
    // type tag + origin. Returns false if not in a match or the payload is too
    // large. This is the mechanism for replicating destructible world state
    // (broken props, destroyed vehicles, state transitions, coins).
    //
    // Events raised before the server WELCOMEs us (a coin picked up on the
    // spawn point while the join handshake is still in flight) are queued and
    // flushed from Update() once our peer id is final, so they are neither lost
    // nor stamped with a provisional origin. The queue is bounded
    // (kMaxPendingWorldEvents, oldest dropped first).
    bool SendWorldEvent(net::WorldEventType type, const uint8_t* payload, uint16_t payloadSize)
    {
        if (!m_session || !m_session->IsInMatch()) return false;
        if (payloadSize > net::MAX_PACKET_PAYLOAD - net::WORLD_EVENT_HEADER_SIZE) return false;
        if (!m_session->IsWelcomed() || !m_pendingWorldEvents.empty())
        {
            // Keep ordering: once anything is queued, later events queue too.
            if (m_pendingWorldEvents.size() >= kMaxPendingWorldEvents)
                m_pendingWorldEvents.erase(m_pendingWorldEvents.begin());
            PendingWorldEvent pe;
            pe.type = type;
            pe.bytes.assign(payload, payload + payloadSize);
            m_pendingWorldEvents.push_back(pe);
            FlushPendingWorldEvents();
            return true;
        }
        return SendWorldEventNow(type, payload, payloadSize);
    }

    // Number of world events waiting for WELCOME (diagnostics/tests).
    size_t GetPendingWorldEventCount() const { return m_pendingWorldEvents.size(); }

    // Registers the callback for inbound world events. Receives the decoded
    // event (type + origin + payload). Self-echo is already filtered by the
    // session's transport. If the session does not exist yet (e.g. called before
    // StartHost/JoinHost), the sink is bound once the session is created.
    void SetWorldEventSink(net::WorldEventCodec::InboundFn fn)
    {
        m_worldEventSink = std::move(fn);
        if (m_session) ReattachWorldEventSink();
    }

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

    // Re-binds the (decoded) world-event sink to the session's custom-event
    // channel. Called after the session is created in StartHost/JoinHost so the
    // sink works even if SetWorldEventSink() was called before the session
    // existed.
    void ReattachWorldEventSink();

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
    unsigned short m_hostMaxPlayers;

    // Game-side authoritative remote set, fed by the bridge.
    RemotePlayerManager m_remotePlayers;
    mutable std::vector<JoinLeaveEvent> m_pendingEvents;

    // Remote players' mission progress, keyed by peer id (PID_MISSION_STATE).
    std::map<int, MissionSyncState> m_remoteMissionStates;

    // Decoded world-event sink (see SetWorldEventSink).
    net::WorldEventCodec::InboundFn m_worldEventSink;

    // World events raised before WELCOME (see SendWorldEvent).
    struct PendingWorldEvent
    {
        net::WorldEventType  type;
        std::vector<uint8_t> bytes;
    };
    static constexpr const size_t kMaxPendingWorldEvents = 64;
    std::vector<PendingWorldEvent> m_pendingWorldEvents;

    bool SendWorldEventNow(net::WorldEventType type, const uint8_t* payload, uint16_t payloadSize)
    {
        uint8_t buf[net::MAX_PACKET_PAYLOAD];
        uint16_t n = net::WorldEventCodec::Encode(buf, sizeof(buf), type,
                                                  static_cast<uint16_t>(GetLocalClientId()),
                                                  payload, payloadSize);
        if (n == 0) return false;
        return m_session->SendCustomEvent(buf, n);
    }

    // Ships queued events in order once welcomed; stops at the first send
    // failure so nothing is reordered or lost.
    void FlushPendingWorldEvents()
    {
        if (!m_session || !m_session->IsWelcomed()) return;
        size_t sent = 0;
        while (sent < m_pendingWorldEvents.size())
        {
            const PendingWorldEvent& pe = m_pendingWorldEvents[sent];
            const uint8_t* data = pe.bytes.empty() ? nullptr : &pe.bytes[0];
            if (!SendWorldEventNow(pe.type, data, static_cast<uint16_t>(pe.bytes.size()))) break;
            ++sent;
        }
        m_pendingWorldEvents.erase(m_pendingWorldEvents.begin(), m_pendingWorldEvents.begin() + sent);
    }
};