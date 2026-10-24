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
    uint8_t  actionEvent;       // [seq:3 | ACTION_* id:5] latest one-shot; 0 = none yet

    // ---- Faithful kinematic vehicle mirror (meaningful when inVehicle) ----
    float    vehYaw;            // machine heading, radians (shortest-arc smoothed)
    uint8_t  vehSteer;          // signed steer (-128..127 -> -45..+45 deg), 0 = straight
    uint16_t vehSpeedKmh;       // machine speed, km/h
    uint8_t  vehState;          // VehicleState enum: 0=VS_NORMAL,1=VS_SLIP,2=VS_EBRAKE_SLIP
    uint8_t  vehFlags;          // VEH_FLAG_* below (brake / reverse)
    uint8_t  vehDamage;         // machine damage 0 (intact) .. 255 (wrecked)

    // Bumped by the sender on every deliberate discontinuity of its position
    // (respawn, interior / mission teleport). The server's movement validator
    // lets a position jump through only together with a new value (rate-
    // limited), so speed / teleport hacks are refused.
    uint8_t  teleportSeq;

    // Animation/prediction flags carried so a receiver can avoid smoothing the
    // trajectory during behaviors that are not continuous locomotion (e.g. a
    // jump: do not interpolate vertically through the arc, snap instead).
    static constexpr const uint8_t FLAG_JUMPING = 0x01;
    static constexpr const uint8_t FLAG_IN_AIR  = 0x02;
    static constexpr const uint8_t FLAG_LOCO_TURBO = 0x04; // local driver's max-speed clamp runs higher

    // One-shot animation/action channel ids. The sender HOLDS the latest action
    // in actionEvent together with a 3-bit sequence number (see LocalActionLatch),
    // so the receiver fires whenever the byte changes: a repeat of the same action
    // still differs by its sequence, and a snapshot sampled late cannot lose it.
    // Jumps ride FLAG_JUMPING (the receiver's jump sink), not ACTION_JUMP.
    // ACTION_KD / ACTION_SURF are reserved (no local source yet).
    enum {
        ACTION_NONE=0, ACTION_JUMP,
        ACTION_SLAM, ACTION_JUMPKICK, ACTION_DODGE, ACTION_CRINGE, ACTION_FLAIL,
        ACTION_GETUP, ACTION_KD, ACTION_ATTACK, ACTION_CELEB_SMALL, ACTION_CELEB_BIG,
        ACTION_SURF
    };
    static constexpr const uint8_t ACTION_ID_MASK   = 0x1F;
    static constexpr const int     ACTION_SEQ_SHIFT = 5;

    static uint8_t MakeActionEvent(uint8_t id, uint8_t seq)
    {
        return static_cast<uint8_t>(((seq & 0x07) << ACTION_SEQ_SHIFT) | (id & ACTION_ID_MASK));
    }
    static uint8_t ActionId(uint8_t actionEvent)
    {
        return static_cast<uint8_t>(actionEvent & ACTION_ID_MASK);
    }

    // Vehicle mirror bits.
    static constexpr const uint8_t VEH_FLAG_BRAKE   = 0x01;
    static constexpr const uint8_t VEH_FLAG_REVERSE = 0x02;
    static constexpr const uint8_t VEH_FLAG_DESTROYED = 0x04; // machine blown up (ghost shows the explosion)

    // Wire format (protocol 3, little-endian, bounds-checked). Only what the
    // player is doing is sent: the on-foot block on foot, the vehicle block in
    // a machine. The vehicle model name is not streamed — it rides the
    // player's presence record (PlayerPresence WOC_VEHICLE) and the receiver
    // fills vehicleModel from it.
    //
    //   u8  sections          SEC_* below; any other bit = malformed
    //   i24 x, y, z           centimetres (+-83 km)
    //   u16 rotY              angle, 2pi/65536 units
    //   u8  flags | u8 characterState | u8 actionEvent | u8 teleportSeq
    //   [SEC_LOCO]    u16 motionAngle | u16 facingAngle | u8 speedFactor        (+5)
    //   [SEC_VEHICLE] i32 netVehicleId | u16 vehYaw (angle) | u8 vehSteer
    //                 | u16 vehSpeedKmh | u8 vehState | u8 vehFlags | u8 vehDamage (+12)
    //
    // On foot 21 bytes, in a machine 28 (the old fixed layout was 60).
    // Decoded angles lie in (-pi, pi]; vehicleSpeed is restored from
    // vehSpeedKmh; fields of an absent block decode as 0.
    static constexpr const uint8_t SEC_LOCO    = 0x01;
    static constexpr const uint8_t SEC_VEHICLE = 0x02;
    static constexpr const int BASE_SIZE    = 1 + 3 * 3 + 2 + 4;
    static constexpr const int LOCO_SIZE    = 2 + 2 + 1;
    static constexpr const int VEHICLE_SIZE = 4 + 2 + 1 + 2 + 1 + 1 + 1;
    static constexpr const int MAX_SERIALIZED_SIZE = BASE_SIZE + VEHICLE_SIZE;

    // Writes the state (out holds MAX_SERIALIZED_SIZE); returns bytes written.
    int  Serialize(char* out) const;
    // False on a malformed / truncated record (the state is then undefined).
    bool Deserialize(const char* data, int size);
};