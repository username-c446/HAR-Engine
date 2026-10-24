#pragma once

#include "../netengine/Packet.h"

#include <cstdint>

// StatePropWorldEventEmitter broadcasts a WE_STATEPROP world event (over the
// open PID_USER relay) whenever a state prop (door / gate / mechanism) changes
// its current state, so remote peers can mirror the transition. The payload is
// deliberately game-agnostic POD (object name uid + new state index); game
// types never leak through this interface. Safe no-op when no match is active.
namespace net
{
    // Payload for WE_STATEPROP: the object uid of the state prop + its new
    // current state index.
    struct StatePropPayload
    {
        uint32_t objectUID = 0;
        uint8_t  state = 0;

        static constexpr const int SERIALIZED_SIZE = sizeof(uint32_t) + sizeof(uint8_t);
        static_assert(SERIALIZED_SIZE >= 0 && SERIALIZED_SIZE < MAX_PACKET_PAYLOAD,
                      "stateprop payload must fit a packet");

        static void Encode(uint8_t* out, uint32_t uid, uint8_t newState)
        {
            Writer w(out, SERIALIZED_SIZE);
            w.WriteU32(uid);
            w.WriteU8(newState);
        }

        static bool Decode(const uint8_t* data, uint16_t size, StatePropPayload& out)
        {
            if (size < static_cast<uint16_t>(SERIALIZED_SIZE)) return false;
            Reader r(data, size);
            if (!r.ReadU32(out.objectUID)) return false;
            uint8_t s = 0;
            if (!r.ReadU8(s)) return false;
            out.state = s;
            return true;
        }
    };

    // Diffuses a state-prop change world event to all peers in the current
    // match. Safe no-op when there is no active match. Invoked from the game's
    // state-prop state-change site (StatePropDSG::SetState).
    void EmitStatePropWorldEvent(uint32_t objectUID, uint8_t state);
}