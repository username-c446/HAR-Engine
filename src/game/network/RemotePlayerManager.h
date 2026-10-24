#pragma once
#include "PlayerNetworkState.h"
#include <map>
#include <vector>
#include <cstdint>

class RemotePlayerManager
{
public:
    RemotePlayerManager();
    ~RemotePlayerManager();

    // `serverTimeMs`: server-clock time the sender sampled the state at.
    void UpdatePlayer(int playerId, const PlayerNetworkState& state, uint32_t serverTimeMs = 0);
    void RemovePlayer(int playerId);
    void Clear();
    const PlayerNetworkState* GetPlayer(int playerId) const;
    // Sample time of the player's latest state (0 if unknown).
    uint32_t GetPlayerTime(int playerId) const;

    // Callback receives each remote player's id + state. Returns false to stop iteration.
    typedef bool (*PlayerVisitor)(int playerId, const PlayerNetworkState& state, void* userData);
    // Iterate over all known remote players in id order.
    void ForEachPlayer(PlayerVisitor visitor, void* userData) const;
    bool HasPlayers() const { return !m_players.empty(); }
    bool HasPlayer(int playerId) const { return m_players.find(playerId) != m_players.end(); }
    int GetPlayerCount() const { return static_cast<int>(m_players.size()); }

    // Removes every player whose id is not in keepIds. Used to apply a
    // server-authoritative entity list on the client.
    void KeepOnly(const std::vector<unsigned short>& keepIds);

private:
    std::map<int, PlayerNetworkState> m_players;
    std::map<int, uint32_t> m_times;
};
