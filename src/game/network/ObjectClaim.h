#pragma once

#include "../netengine/WorldState.h"

#include <cstdint>

// Exclusive claims on a world object ("this parked car is mine"): the record's
// value is the claimant's peer id (u16 little-endian) written with
// WSP_FIRST_WRITE, so the server keeps the first claim and every client learns
// the winner from the authoritative record — the loser through its WSR_REJECT,
// which carries the current entry. A client that acted on its own claim before
// the verdict (predicted) rolls back when the winner is someone else.
namespace net
{
    static constexpr const uint8_t kClaimSize = 2;

    inline uint8_t EncodeClaim(PeerId claimant, uint8_t* out)
    {
        out[0] = static_cast<uint8_t>(claimant & 0xFF);
        out[1] = static_cast<uint8_t>((claimant >> 8) & 0xFF);
        return kClaimSize;
    }

    enum ClaimOutcome : uint8_t
    {
        CLAIM_NONE,   // not a claim record (or claimant 0)
        CLAIM_OURS,   // we hold the object
        CLAIM_THEIRS, // another player does
    };

    // `self` 0 (not in a match) never owns a claim.
    inline ClaimOutcome ClaimOutcomeOf(const WorldStateValue& v, PeerId self)
    {
        if (v.len != kClaimSize) return CLAIM_NONE;
        PeerId who = static_cast<PeerId>(v.bytes[0] | (v.bytes[1] << 8));
        if (who == 0) return CLAIM_NONE;
        return (self != 0 && who == self) ? CLAIM_OURS : CLAIM_THEIRS;
    }

    // Rest pose of a movable static object (a pushed parked car), WOC_POSE:
    // f32 x, y, z, yaw (16 bytes). Late joiners and a reloaded zone place the
    // object here instead of at its locator.
    struct ObjectPose
    {
        float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;

        static constexpr const uint8_t SIZE = 16;
        bool Encode(WorldStateValue& out) const
        {
            Writer w(out.bytes, WS_MAX_VALUE);
            w.WriteFloat(x); w.WriteFloat(y); w.WriteFloat(z); w.WriteFloat(yaw);
            if (!w.Ok()) return false;
            out.len = static_cast<uint8_t>(w.Position());
            return true;
        }
        bool Decode(const WorldStateValue& in)
        {
            if (in.len != SIZE) return false;
            Reader r(in.bytes, in.len);
            if (!(r.ReadFloat(x) && r.ReadFloat(y) && r.ReadFloat(z) && r.ReadFloat(yaw))) return false;
            return x == x && y == y && z == z && yaw == yaw; // never a NaN pose
        }
    };
}
