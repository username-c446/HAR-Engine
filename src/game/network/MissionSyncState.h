#pragma once

#include <cstdint>
#include <cstring>

// Serialized per-player mission progress for the "together" case: mirrors what
// mission/stage/objective the player is currently on, so co-op partners can
// track each other's progress. Relayed by the server like PlayerNetworkState.
struct MissionSyncState
{
    int32_t missionIndex;      // current mission (or -1 if none)
    int32_t stageIndex;        // current stage within the mission
    int32_t objectiveIndex;    // current objective within the stage
    uint8_t  inMission;        // 1 = actively playing a mission, 0 = free roam

    static constexpr const int SERIALIZED_SIZE = sizeof(int32_t) * 3 + sizeof(uint8_t);

    void Serialize(char* buffer) const;
    void Deserialize(const char* buffer);

    void Clear()
    {
        missionIndex = -1;
        stageIndex = -1;
        objectiveIndex = -1;
        inMission = 0;
    }
};