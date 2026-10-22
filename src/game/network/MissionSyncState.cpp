#include "MissionSyncState.h"
#include "NetworkPacket.h"

void MissionSyncState::Serialize(char* buffer) const
{
    char* p = buffer;
    net::WriteU32(p, static_cast<uint32_t>(missionIndex));
    net::WriteU32(p, static_cast<uint32_t>(stageIndex));
    net::WriteU32(p, static_cast<uint32_t>(objectiveIndex));
    *p++ = inMission;
}

void MissionSyncState::Deserialize(const char* buffer)
{
    const char* p = buffer;
    missionIndex = static_cast<int32_t>(net::ReadU32(p));
    stageIndex = static_cast<int32_t>(net::ReadU32(p));
    objectiveIndex = static_cast<int32_t>(net::ReadU32(p));
    inMission = static_cast<uint8_t>(*p++);
}