#include "RemotePlayerManager.h"

RemotePlayerManager::RemotePlayerManager() {}
RemotePlayerManager::~RemotePlayerManager() {}

void RemotePlayerManager::UpdatePlayer(int playerId, const PlayerNetworkState& state)
{
    m_players[playerId] = state;
}

void RemotePlayerManager::RemovePlayer(int playerId)
{
    m_players.erase(playerId);
}

void RemotePlayerManager::Clear()
{
    m_players.clear();
}

void RemotePlayerManager::KeepOnly(const std::vector<unsigned short>& keepIds)
{
    std::map<int, PlayerNetworkState> kept;
    for (unsigned short id : keepIds) {
        auto it = m_players.find(id);
        if (it != m_players.end()) {
            kept[id] = it->second;
        }
    }
    m_players.swap(kept);
}

const PlayerNetworkState* RemotePlayerManager::GetPlayer(int playerId) const
{
    auto it = m_players.find(playerId);
    if (it == m_players.end()) return nullptr;
    return &it->second;
}

void RemotePlayerManager::ForEachPlayer(PlayerVisitor visitor, void* userData) const
{
    if (visitor == nullptr) return;
    for (auto& kv : m_players) {
        if (!visitor(kv.first, kv.second, userData)) break;
    }
}
