#pragma once

#include "../netengine/Packet.h"

#include <cstdint>

// PlatformDirWorldEventEmitter broadcasts a WE_PLATFORM_DIR world event (over
// the open PID_USER relay) whenever an animated collision entity (platform /
// door / crane) reverses or stops its animation direction, so remote peers can
// mirror the same animation phase. The payload is deliberately game-agnostic
// POD (object name uid + signed direction); game types never leak through this
// interface. Safe no-op when no match is active.
namespace net
{
    // Payload for WE_PLATFORM_DIR: the object uid of the animated entity + its
    // new animation direction (-1 reverse / 0 stop / +1 forward).
    struct PlatformDirPayload
    {
        uint32_t objectUID = 0;
        int8_t   dir = 0;

        static constexpr const int SERIALIZED_SIZE = sizeof(uint32_t) + sizeof(int8_t);
        static_assert(SERIALIZED_SIZE >= 0 && SERIALIZED_SIZE < MAX_PACKET_PAYLOAD,
                      "platform-dir payload must fit a packet");

        static void Encode(uint8_t* out, uint32_t uid, int8_t direction)
        {
            Writer w(out, SERIALIZED_SIZE);
            w.WriteU32(uid);
            w.WriteU8(static_cast<uint8_t>(direction));
        }

        static bool Decode(const uint8_t* data, uint16_t size, PlatformDirPayload& out)
        {
            if (size < static_cast<uint16_t>(SERIALIZED_SIZE)) return false;
            Reader r(data, size);
            if (!r.ReadU32(out.objectUID)) return false;
            uint8_t d = 0;
            if (!r.ReadU8(d)) return false;
            out.dir = static_cast<int8_t>(d);
            return true;
        }
    };

    // Diffuses a platform-direction world event to all peers in the current
    // match. Safe no-op when there is no active match. Invoked from the game's
    // animation-direction apply site (AnimCollisionEntityDSG::SetAnimationDirection).
    void EmitPlatformDirWorldEvent(uint32_t objectUID, int32_t dir);
}