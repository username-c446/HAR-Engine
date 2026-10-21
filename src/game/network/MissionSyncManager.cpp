#include "MissionSyncManager.h"

MissionSyncManager::MissionSyncManager() : m_currentStage(0), m_currentObjective(0) {}
MissionSyncManager::~MissionSyncManager() {}

void MissionSyncManager::SyncStage(int stageIndex)
{
    m_currentStage = stageIndex;
}

void MissionSyncManager::SyncObjective(int objectiveIndex)
{
    m_currentObjective = objectiveIndex;
}
