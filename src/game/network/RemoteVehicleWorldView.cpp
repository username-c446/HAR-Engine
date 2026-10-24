#include "RemoteVehicleWorldView.h"

RemoteVehicleWorldView::RemoteVehicleWorldView()
{
    memset(&m_sink, 0, sizeof(m_sink));
}

RemoteVehicleWorldView::~RemoteVehicleWorldView()
{
    Clear();
}

void RemoteVehicleWorldView::SyncDriver(int playerId, const PlayerNetworkState& state, uint32_t serverTimeMs)
{
    bool inVehicle = (state.inVehicle != 0) && state.vehicleModel[0] != '\0';

    auto it = m_entries.find(playerId);
    if (it == m_entries.end())
    {
        if (inVehicle) SpawnFor(playerId, state, serverTimeMs);
        return;
    }

    Entry& entry = it->second;
    if (!inVehicle)
    {
        // The remote driver got out (or walked). Remove the ghost machine.
        DespawnIt(playerId, entry);
        m_entries.erase(it);
        return;
    }

    // A different machine: rebuild the ghost.
    bool modelChanged = strncmp(entry.model, state.vehicleModel, sizeof(entry.model)) != 0
        || entry.netVehicleId != state.netVehicleId;
    if (modelChanged)
    {
        DespawnIt(playerId, entry);
        m_entries.erase(it);
        SpawnFor(playerId, state, serverTimeMs);
        return;
    }
    // The world could not create the ghost earlier (no free slot): retry.
    if (entry.handle == nullptr && m_sink.spawn)
        entry.handle = m_sink.spawn(playerId, state.vehicleModel, state.netVehicleId);
    entry.track.Push(serverTimeMs, state);
}

void RemoteVehicleWorldView::ClearDriver(int playerId)
{
    auto it = m_entries.find(playerId);
    if (it == m_entries.end()) return;
    DespawnIt(playerId, it->second);
    m_entries.erase(it);
}

void RemoteVehicleWorldView::Prune(const RemotePlayerManager& players)
{
    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        if (!players.HasPlayer(it->first))
        {
            DespawnIt(it->first, it->second);
            it = m_entries.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void RemoteVehicleWorldView::Clear()
{
    for (auto& kv : m_entries)
    {
        if (m_sink.despawn) m_sink.despawn(kv.first, kv.second.handle);
    }
    m_entries.clear();
}

void RemoteVehicleWorldView::Update(float dt)
{
    (void)dt; // poses come from the server clock, not from frame time
    if (!m_sink.move) return;
    for (auto& kv : m_entries)
    {
        if (kv.second.handle == nullptr) continue;
        PlayerNetworkState s;
        if (kv.second.track.Render(m_timing, s)) m_sink.move(kv.first, kv.second.handle, s);
    }
}

int RemoteVehicleWorldView::FindDriver(const void* handle) const
{
    if (handle == nullptr) return 0;
    for (const auto& kv : m_entries)
        if (kv.second.handle == handle) return kv.first;
    return 0;
}

void RemoteVehicleWorldView::SpawnFor(int playerId, const PlayerNetworkState& state, uint32_t serverTimeMs)
{
    void* handle = m_sink.spawn ? m_sink.spawn(playerId, state.vehicleModel, state.netVehicleId) : nullptr;
    Entry& entry = m_entries[playerId];
    entry.handle = handle;
    strncpy(entry.model, state.vehicleModel, sizeof(entry.model) - 1);
    entry.model[sizeof(entry.model) - 1] = '\0';
    entry.netVehicleId = state.netVehicleId;
    entry.track.Clear();
    entry.track.Push(serverTimeMs, state);

    if (handle != nullptr && m_sink.move) m_sink.move(playerId, handle, state);
}

void RemoteVehicleWorldView::DespawnIt(int playerId, Entry& entry)
{
    if (entry.handle != nullptr && m_sink.despawn) m_sink.despawn(playerId, entry.handle);
    entry.handle = nullptr;
}
