#include "MissionSyncManager.h"

MissionSyncManager::MissionSyncManager()
{
    m_localState.Clear();
}

MissionSyncManager::~MissionSyncManager()
{
}

void MissionSyncManager::SetLocalMission(int missionIndex, int stageIndex, int objectiveIndex, bool inMission)
{
    m_localState.missionIndex = missionIndex;
    m_localState.stageIndex = stageIndex;
    m_localState.objectiveIndex = objectiveIndex;
    m_localState.inMission = inMission ? 1 : 0;
}

void MissionSyncManager::SyncStage(int stageIndex)
{
    m_localState.stageIndex = stageIndex;
    m_localState.inMission = 1;
}

void MissionSyncManager::SyncObjective(int objectiveIndex)
{
    m_localState.objectiveIndex = objectiveIndex;
    m_localState.inMission = 1;
}

void MissionSyncManager::UpdateRemotePlayer(int playerId, const MissionSyncState& state)
{
    m_remoteStates[playerId] = state;
}

void MissionSyncManager::RemoveRemotePlayer(int playerId)
{
    m_remoteStates.erase(playerId);
}

void MissionSyncManager::ClearRemotePlayers()
{
    m_remoteStates.clear();
}

void MissionSyncManager::ForEachRemotePlayer(Visitor visitor, void* userData) const
{
    for (auto& kv : m_remoteStates)
    {
        if (!visitor(kv.first, kv.second, userData)) break;
    }
}