#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "RemotePlayerManager.h"
#include "PlayerNetworkState.h"
#include "MissionSyncState.h"
#include "NetworkPacket.h"
#include "ENetInit.h"
#include <enet/enet.h>
#include <map>
#include <vector>

class NetworkClient
{
public:
    NetworkClient();
    ~NetworkClient();

    bool Connect(const char* serverIp, unsigned short serverPort);
    void Disconnect();
    void Update();

    bool IsConnected() const { return m_connected; }
    bool IsServerReady() const { return m_serverReady; }
    unsigned short GetClientId() const { return m_clientId; }

    void SendPlayerState(const PlayerNetworkState& state);
    void SendMissionState(const MissionSyncState& state);
    void SendHello();
    void SendAuth(const char* password);
    void SetPassword(const char* password);

    const RemotePlayerManager& GetRemotePlayers() const { return m_remotePlayers; }
    int GetHelloRetriesLeft() const { return m_helloRetries; }
    void SetPingIntervalMs(enet_uint32 ms) { m_pingIntervalMs = ms ? ms : net::PING_INTERVAL_MS; }

    // Connection status for UI/HUD.
    // Round-trip latency in ms, or 0 if not yet measured / not connected.
    enet_uint32 GetPingMs() const { return m_pingMs; }
    // True once we have received at least one heartbeat round-trip.
    bool HasMeasuredPing() const { return m_pingMeasured; }

    // Accessor for tests/UI: the server-approved list of present entity ids.
    const std::vector<unsigned short>& GetAuthoritativePlayers() const { return m_authoritativePlayers; }

    // Join/leave notifications observed since the last call. Each event is a
    // client id plus a flag (true = joined, false = left). Consuming the events
    // in-order lets a UI print "Player X connected" / "disconnected".
    struct JoinLeaveEvent { unsigned short clientId; bool joined; };
    // Appends all pending events to out and clears the pending buffer.
    void DrainJoinLeaveEvents(std::vector<JoinLeaveEvent>& out);

    // Remote players' mission progress as relayed by the server (for the
    // "playing together" case). Indexed by network client id.
    const std::map<unsigned short, MissionSyncState>& GetRemoteMissionStates() const { return m_remoteMissionStates; }

private:
    void HandlePacket(const char* data, size_t size);

    net::ENetInit m_enetInit;
    ENetHost* m_host;
    ENetPeer* m_server;
    char m_serverIp[64];
    unsigned short m_serverPort;
    bool m_connected;
    bool m_serverReady;
    unsigned short m_clientId;
    int m_helloRetries;
    uint32_t m_sequence;
    enet_uint32 m_lastPingMs;
    enet_uint32 m_pingIntervalMs;
    enet_uint32 m_pingSentMs;    // timestamp of the in-flight ping, for RTT
    enet_uint32 m_pingMs;        // last measured round-trip latency
    bool m_pingMeasured;
    char m_password[65];
    bool m_hasPassword;
    RemotePlayerManager m_remotePlayers;
    std::vector<unsigned short> m_authoritativePlayers; // server-approved entity list
    std::vector<unsigned short> m_prevAuthoritativePlayers;
    std::vector<JoinLeaveEvent> m_pendingEvents; // join/leave notifications
    std::map<unsigned short, MissionSyncState> m_remoteMissionStates; // relayed mission progress
};