#pragma once

#include <cstdint>

struct PlayerNetworkState
{
    float x, y, z;
    float rotY;
    // Vehicle sync: when inVehicle != 0, the driver's machine is described by a
    // stable network id plus the model name, so a remote side can mirror it. On
    // foot, vehicleModel[0] == 0 and netVehicleId == 0.
    char vehicleModel[16];
    int32_t netVehicleId;
    float vehicleSpeed;   // current machine speed, used by server plausibility checks
    uint8_t inVehicle;
    uint8_t padding;      // keep struct byte layout stable

    // Number of bytes Serialize writes / Deserialize reads.
    static constexpr const int SERIALIZED_SIZE = sizeof(float) * 4
        + sizeof(vehicleModel)
        + sizeof(int32_t)
        + sizeof(float)
        + sizeof(uint8_t) * 2;

    void Serialize(char* buffer) const;
    void Deserialize(const char* buffer);
};