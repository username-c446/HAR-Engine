#include "PlayerNetworkState.h"
#include "NetworkPacket.h"

void PlayerNetworkState::Serialize(char* buffer) const
{
    char* p = buffer;
    net::WriteFloat(p, x);
    net::WriteFloat(p, y);
    net::WriteFloat(p, z);
    net::WriteFloat(p, rotY);
    for (int i = 0; i < static_cast<int>(sizeof(vehicleModel)); ++i) *p++ = vehicleModel[i];
    net::WriteU32(p, static_cast<uint32_t>(netVehicleId));
    net::WriteFloat(p, vehicleSpeed);
    *p++ = inVehicle;
    *p++ = flags;
}

void PlayerNetworkState::Deserialize(const char* buffer)
{
    const char* p = buffer;
    x = net::ReadFloat(p);
    y = net::ReadFloat(p);
    z = net::ReadFloat(p);
    rotY = net::ReadFloat(p);
    for (int i = 0; i < static_cast<int>(sizeof(vehicleModel)); ++i) vehicleModel[i] = *p++;
    netVehicleId = static_cast<int32_t>(net::ReadU32(p));
    vehicleSpeed = net::ReadFloat(p);
    inVehicle = static_cast<uint8_t>(*p++);
    flags = static_cast<uint8_t>(*p++);
}