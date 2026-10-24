#include "PlayerNetworkState.h"
#include "../netengine/Quantize.h"

#include <cstring>

int PlayerNetworkState::Serialize(char* out) const
{
    uint8_t* buf = reinterpret_cast<uint8_t*>(out);
    net::Writer w(buf, MAX_SERIALIZED_SIZE);
    const bool vehicle = inVehicle != 0;
    w.WriteU8(vehicle ? SEC_VEHICLE : SEC_LOCO);
    net::WriteCentimetres24(w, x);
    net::WriteCentimetres24(w, y);
    net::WriteCentimetres24(w, z);
    w.WriteU16(net::EncodeAngle16(rotY));
    w.WriteU8(flags);
    w.WriteU8(characterState);
    w.WriteU8(actionEvent);
    w.WriteU8(teleportSeq);
    if (vehicle)
    {
        w.WriteU32(static_cast<uint32_t>(netVehicleId));
        w.WriteU16(net::EncodeAngle16(vehYaw));
        w.WriteU8(vehSteer);
        w.WriteU16(vehSpeedKmh);
        w.WriteU8(vehState);
        w.WriteU8(vehFlags);
        w.WriteU8(vehDamage);
    }
    else
    {
        w.WriteU16(motionAngle);
        w.WriteU16(facingAngle);
        w.WriteU8(speedFactor);
    }
    return w.Ok() ? static_cast<int>(w.Position()) : 0;
}

bool PlayerNetworkState::Deserialize(const char* data, int size)
{
    std::memset(this, 0, sizeof(*this));
    if (data == nullptr || size <= 0 || size > 0xFFFF) return false;
    net::Reader r(reinterpret_cast<const uint8_t*>(data), static_cast<uint16_t>(size));
    uint8_t sections = 0;
    uint16_t rot = 0;
    if (!r.ReadU8(sections)) return false;
    if ((sections & ~(SEC_LOCO | SEC_VEHICLE)) != 0) return false;
    if (!net::ReadCentimetres24(r, x) || !net::ReadCentimetres24(r, y) || !net::ReadCentimetres24(r, z) || !r.ReadU16(rot)) return false;
    rotY = net::DecodeAngle16(rot);
    if (!r.ReadU8(flags) || !r.ReadU8(characterState) || !r.ReadU8(actionEvent) || !r.ReadU8(teleportSeq))
        return false;
    if (sections & SEC_VEHICLE)
    {
        uint32_t id = 0;
        uint16_t yaw = 0;
        if (!r.ReadU32(id) || !r.ReadU16(yaw) || !r.ReadU8(vehSteer) || !r.ReadU16(vehSpeedKmh)
            || !r.ReadU8(vehState) || !r.ReadU8(vehFlags) || !r.ReadU8(vehDamage))
            return false;
        inVehicle = 1;
        netVehicleId = static_cast<int32_t>(id);
        vehYaw = net::DecodeAngle16(yaw);
        vehicleSpeed = static_cast<float>(vehSpeedKmh);
    }
    if (sections & SEC_LOCO)
    {
        if (!r.ReadU16(motionAngle) || !r.ReadU16(facingAngle) || !r.ReadU8(speedFactor)) return false;
    }
    return r.Remaining() == 0;
}
