#pragma once

class MissionSyncManager
{
public:
    MissionSyncManager();
    ~MissionSyncManager();

    void SyncStage(int stageIndex);
    void SyncObjective(int objectiveIndex);
    int GetCurrentStage() const { return m_currentStage; }

private:
    int m_currentStage;
    int m_currentObjective;
};
