#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "RemotePlayerManager.h"
#include "ENetInit.h"
#include "NetworkPacket.h"
#include "ServerModManager.h"
#include "MissionSyncState.h"
#include <enet/enet.h>
#include <map>

class NetworkServer
{
public:
    NetworkServer();
    ~NetworkServer();

    bool Start(unsigned short port);
    void Stop();
    void Update();

    bool IsRunning() const { return m_host != nullptr; }
    unsigned short GetPort() const { return m_port; }
    const RemotePlayerManager& GetRemotePlayers() const { return m_players; }

    // Server configuration. Slots cap concurrent players; a non-empty password
    // requires clients to present PACKET_AUTH with a matching string before the
    // server will welcome them. Call before Start().
    void SetMaxPlayers(unsigned short slots) { m_maxPlayers = slots > 0 ? slots : 1; }
    void SetPassword(const char* password);
    bool HasPassword() const { return m_password[0] != '\0'; }
    void SetRequiresAuth(bool required) { m_requiresAuth = required; }

    // True while a session exists for the given server-assigned client id.
    bool IsClientConnected(unsigned short clientId) const { return m_sessions.find(clientId) != m_sessions.end(); }

    // Overrides the liveness window (tests use a short value to avoid waiting
    // the real CLIENT_TIMEOUT_MS). Pass 0 to restore the default.
    void SetClientTimeoutMs(unsigned int ms) { m_clientTimeoutMs = ms ? ms : net::CLIENT_TIMEOUT_MS; }

    // Server-side Lua mod: loads a script exposing on_join/on_validate/on_leave
    // hooks that can override the authoritative defaults. Returns true on success.
    bool LoadServerMod(const char* luaPath) { return m_mods.LoadScript(luaPath); }
    bool HasServerMod() const { return m_mods.HasScript(); }

// Server-authoritative per-client session record. The server is the arbiter
    // of who is admitted and what state is trusted; clients do not self-authorize.
    struct ClientSession
    {
        ENetPeer* peer;
        enet_uint32 lastStateTimeMs;
        enet_uint32 connectedAtMs;
        enet_uint32 lastSeenMs;   // last time any heartbeat/packet arrived
        bool validated;
        bool authenticated;       // passed password gate (true if no password)
    };

private:
    void HandlePacket(ENetPeer* peer, const char* data, size_t size);
    void SendTo(ENetPeer* peer, unsigned char type, const void* payload, size_t payloadSize, unsigned short clientId);

    // Welcomes an authenticated client: assigns/confirms the id via WELCOME and
    // broadcasts the updated entity list to everyone.
    void WelcomeClient(ENetPeer* peer, unsigned short clientId);

    // Returns true if the proposed state is plausible enough to relay, false if
    // it smells like a cheat/teleport. Updates the session's validation state.
    bool ValidateAndRelay(unsigned short clientId, const PlayerNetworkState& proposed);

    // Sends the server-authoritative list of present players to every client.
    void SendPlayerListToAll();

    net::ENetInit m_enetInit;
    ENetHost* m_host;
    unsigned short m_port;
    unsigned short m_nextClientId;
    uint32_t m_sequence;
    unsigned int m_clientTimeoutMs;
    unsigned short m_maxPlayers;
    char m_password[65];
    bool m_requiresAuth;

    std::map<ENetPeer*, unsigned short> m_peerIds;
    std::map<unsigned short, ENetPeer*> m_peersById;
    std::map<unsigned short, ClientSession> m_sessions;
    RemotePlayerManager m_players;
    std::map<unsigned short, MissionSyncState> m_missionStates;
    ServerModManager m_mods;
};