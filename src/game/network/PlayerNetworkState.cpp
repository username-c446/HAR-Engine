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
    // ---- loco ----
    net::WriteU16(p, motionAngle);
    net::WriteU16(p, facingAngle);
    *p++ = speedFactor;
    *p++ = characterState;
    // ---- action ----
    *p++ = actionEvent;
    // ---- vehicle mirror ----
    net::WriteFloat(p, vehYaw);
    *p++ = vehSteer;
    net::WriteU16(p, vehSpeedKmh);
    *p++ = vehState;
    *p++ = vehFlags;
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
    // ---- loco ----
    motionAngle = net::ReadU16(p);
    facingAngle = net::ReadU16(p);
    speedFactor = static_cast<uint8_t>(*p++);
    characterState = static_cast<uint8_t>(*p++);
    // ---- action ----
    actionEvent = static_cast<uint8_t>(*p++);
    // ---- vehicle mirror ----
    vehYaw = net::ReadFloat(p);
    vehSteer = static_cast<uint8_t>(*p++);
    vehSpeedKmh = net::ReadU16(p);
    vehState = static_cast<uint8_t>(*p++);
    vehFlags = static_cast<uint8_t>(*p++);
}