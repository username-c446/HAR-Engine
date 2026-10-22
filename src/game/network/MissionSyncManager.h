#pragma once

#include "MissionSyncState.h"

#include <map>

// Tracks the local player's mission progress and mirrors the progress of remote
// players (so co-op partners progressing through a mission together stay in
// sync). The local state is fed by the game (GameplayManager); the manager then
// exposes it for network relay and stores remote players' mission states.
class MissionSyncManager
{
public:
    MissionSyncManager();
    ~MissionSyncManager();

    // Local side: update what the local player is doing.
    void SetLocalMission(int missionIndex, int stageIndex, int objectiveIndex, bool inMission);
    void SyncStage(int stageIndex);
    void SyncObjective(int objectiveIndex);

    const MissionSyncState& GetLocalState() const { return m_localState; }

    // Remote side: store the reported progress of a networked player.
    void UpdateRemotePlayer(int playerId, const MissionSyncState& state);
    void RemoveRemotePlayer(int playerId);
    void ClearRemotePlayers();

    bool HasRemotePlayer(int playerId) const { return m_remoteStates.find(playerId) != m_remoteStates.end(); }

    int GetCurrentStage() const { return m_localState.stageIndex; }
    int GetCurrentObjective() const { return m_localState.objectiveIndex; }

    // Iteration over remote players' mission states for tests/UI.
    typedef bool (*Visitor)(int playerId, const MissionSyncState& state, void* userData);
    void ForEachRemotePlayer(Visitor visitor, void* userData) const;

private:
    MissionSyncState m_localState;
    std::map<int, MissionSyncState> m_remoteStates;

    MissionSyncManager(const MissionSyncManager&);
    MissionSyncManager& operator=(const MissionSyncManager&);
};