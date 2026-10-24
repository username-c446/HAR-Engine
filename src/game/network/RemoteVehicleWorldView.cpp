#include "RemoteVehicleWorldView.h"

#include <cmath>

RemoteVehicleWorldView::RemoteVehicleWorldView()
    : m_smoothingSeconds(0.12f)
{
    memset(&m_sink, 0, sizeof(m_sink));
}

RemoteVehicleWorldView::~RemoteVehicleWorldView()
{
    Clear();
}

void RemoteVehicleWorldView::SyncDriver(int playerId, const PlayerNetworkState& state)
{
    bool inVehicle = (state.inVehicle != 0) && state.vehicleModel[0] != '\0';

    auto it = m_entries.find(playerId);
    if (it == m_entries.end())
    {
        if (inVehicle) SpawnFor(playerId, state);
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

    // Still in a machine: if the model changed, rebuild the ghost; otherwise
    // just refresh the movement target.
    bool modelChanged = strncmp(entry.model, state.vehicleModel, sizeof(entry.model)) != 0
        || entry.netVehicleId != state.netVehicleId;
    if (modelChanged)
    {
        DespawnIt(playerId, entry);
        m_entries.erase(it);
        SpawnFor(playerId, state);
        return;
    }

    entry.targetX = state.x;
    entry.targetY = state.y;
    entry.targetZ = state.z;
    entry.targetYaw = state.vehYaw;
    entry.targetSteer = state.vehSteer;
    entry.targetSpeedKmh = state.vehSpeedKmh;
    entry.targetState = state.vehState;
    entry.targetFlags = state.vehFlags;
    entry.smoothingApplied = false;
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
    if (dt <= 0.0f) return;

    const float lambda = 3.0f / (m_smoothingSeconds > 0.0f ? m_smoothingSeconds : 0.12f);
    const float alpha = 1.0f - expf(-lambda * dt);

    for (auto& kv : m_entries)
    {
        Entry& entry = kv.second;
        if (entry.smoothingApplied) continue;

        entry.visualX += (entry.targetX - entry.visualX) * alpha;
        entry.visualY += (entry.targetY - entry.visualY) * alpha;
        entry.visualZ += (entry.targetZ - entry.visualZ) * alpha;
        entry.smoothingApplied = true;

        // Decrementally rotate the yaw toward its target over the shortest arc
        // (mirrors RemotePlayerWorldView's facing smoothing).
        static const float kPi = 3.14159265f;
        float diff = entry.targetYaw - entry.visualYaw;
        while (diff >  kPi) diff -= 2.0f * kPi;
        while (diff < -kPi) diff += 2.0f * kPi;
        if (diff != 0.0f) entry.visualYaw += diff * alpha;
        if (entry.visualYaw >  kPi) entry.visualYaw -= 2.0f * kPi;
        if (entry.visualYaw < -kPi) entry.visualYaw += 2.0f * kPi;

        if (m_sink.move)
        {
            PlayerNetworkState state;
            state.x = entry.visualX;
            state.y = entry.visualY;
            state.z = entry.visualZ;
            state.vehYaw = entry.visualYaw;
            state.vehSteer = entry.targetSteer;
            state.vehSpeedKmh = entry.targetSpeedKmh;
            state.vehState = entry.targetState;
            state.vehFlags = entry.targetFlags;
            m_sink.move(kv.first, entry.handle, state);
        }
    }
}

void RemoteVehicleWorldView::SpawnFor(int playerId, const PlayerNetworkState& state)
{
    void* handle = m_sink.spawn ? m_sink.spawn(playerId, state.vehicleModel, state.netVehicleId) : nullptr;
    Entry entry;
    memset(&entry, 0, sizeof(entry));
    entry.handle = handle;
    strncpy(entry.model, state.vehicleModel, sizeof(entry.model) - 1);
    entry.model[sizeof(entry.model) - 1] = '\0';
    entry.netVehicleId = state.netVehicleId;
    entry.targetX = entry.visualX = state.x;
    entry.targetY = entry.visualY = state.y;
    entry.targetZ = entry.visualZ = state.z;
    entry.targetYaw = entry.visualYaw = state.vehYaw;
    entry.targetSteer = state.vehSteer;
    entry.targetSpeedKmh = state.vehSpeedKmh;
    entry.targetState = state.vehState;
    entry.targetFlags = state.vehFlags;
    entry.smoothingApplied = false;
    m_entries[playerId] = entry;

    if (handle != nullptr && m_sink.move) m_sink.move(playerId, handle, state);
}

void RemoteVehicleWorldView::DespawnIt(int playerId, Entry& entry)
{
    if (entry.handle != nullptr && m_sink.despawn) m_sink.despawn(playerId, entry.handle);
    entry.handle = nullptr;
}