#include "RemotePlayerManager.h"

#include <iterator>

RemotePlayerManager::RemotePlayerManager() {}
RemotePlayerManager::~RemotePlayerManager() {}

void RemotePlayerManager::UpdatePlayer(int playerId, const PlayerNetworkState& state, uint32_t serverTimeMs)
{
    m_players[playerId] = state;
    m_times[playerId] = serverTimeMs;
}

uint32_t RemotePlayerManager::GetPlayerTime(int playerId) const
{
    auto it = m_times.find(playerId);
    return it == m_times.end() ? 0 : it->second;
}

void RemotePlayerManager::RemovePlayer(int playerId)
{
    m_players.erase(playerId);
    m_times.erase(playerId);
}

void RemotePlayerManager::Clear()
{
    m_players.clear();
    m_times.clear();
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
    for (auto it = m_times.begin(); it != m_times.end();)
        it = m_players.count(it->first) ? std::next(it) : m_times.erase(it);
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
