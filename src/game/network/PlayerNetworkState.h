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
    uint8_t flags;        // bitfield of FLAG_* below (replaces the old unused padding)

    // ---- Continuous locomotion sync (meaningful when on foot / characterState) ----
    uint16_t motionAngle;       // desired motion direction, radians * 1000 (0..~6283)
    uint16_t facingAngle;       // desired facing, radians * 1000
    uint8_t  speedFactor;       // desired speed as 0..255 fraction of max speed
    uint8_t  characterState;    // CharacterAi::CharacterState value (gating: LOCO etc.)

    // ---- One-shot animation event (rising-edge latched by the receiver) ----
    uint8_t  actionEvent;       // PlayerNetworkState::ACTION_* one-shot; 0 = none

    // ---- Faithful kinematic vehicle mirror (meaningful when inVehicle) ----
    float    vehYaw;            // machine heading, radians (shortest-arc smoothed)
    uint8_t  vehSteer;          // signed steer (-128..127 -> -45..+45 deg), 0 = straight
    uint16_t vehSpeedKmh;       // machine speed, km/h
    uint8_t  vehState;          // VehicleState enum: 0=VS_NORMAL,1=VS_SLIP,2=VS_EBRAKE_SLIP
    uint8_t  vehFlags;          // VEH_FLAG_* below (brake / reverse)

    // Animation/prediction flags carried so a receiver can avoid smoothing the
    // trajectory during behaviors that are not continuous locomotion (e.g. a
    // jump: do not interpolate vertically through the arc, snap instead).
    static constexpr const uint8_t FLAG_JUMPING = 0x01;
    static constexpr const uint8_t FLAG_IN_AIR  = 0x02;
    static constexpr const uint8_t FLAG_LOCO_TURBO = 0x04; // local driver's max-speed clamp runs higher

    // One-shot animation/action channel ids (rising-edge events, see ACTION_*).
    // Only ACTION_NONE / ACTION_JUMP are currently populated by the local provider;
    // the rest are reserved for later passes (slam, dodge, celebration, ...).
    enum {
        ACTION_NONE=0, ACTION_JUMP,
        ACTION_SLAM, ACTION_JUMPKICK, ACTION_DODGE, ACTION_CRINGE, ACTION_FLAIL,
        ACTION_GETUP, ACTION_KD, ACTION_ATTACK, ACTION_CELEB_SMALL, ACTION_CELEB_BIG,
        ACTION_SURF
    };

    // Vehicle mirror bits.
    static constexpr const uint8_t VEH_FLAG_BRAKE   = 0x01;
    static constexpr const uint8_t VEH_FLAG_REVERSE = 0x02;

    // Number of bytes Serialize writes / Deserialize reads.
    // 4 floats + vehicleModel[16] + int32 + float + 2*uint8  (base, 42)
    // + 2*uint16 + 2*uint8 + uint8                          (loco, +7)
    // + uint8                                               (actionEvent, +1)
    // + float + uint8 + uint16 + uint8 + uint8              (vehicle mirror, +9)  => 58 total.
    static constexpr const int SERIALIZED_SIZE = sizeof(float) * 4
        + sizeof(vehicleModel)
        + sizeof(int32_t)
        + sizeof(float)
        + sizeof(uint8_t) * 2
        // loco
        + sizeof(uint16_t) * 2
        + sizeof(uint8_t) * 2
        // actionEvent
        + sizeof(uint8_t)
        // vehicle mirror
        + sizeof(float)
        + sizeof(uint8_t)
        + sizeof(uint16_t)
        + sizeof(uint8_t)
        + sizeof(uint8_t);

    void Serialize(char* buffer) const;
    void Deserialize(const char* buffer);
};