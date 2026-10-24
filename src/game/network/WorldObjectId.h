#pragma once

#include "../netengine/Packet.h"

#include <cstdint>
#include <cstddef>

// Deterministic network identities for world objects.
//
// A STATIC world object (stateprop, animated collision entity, parked car,
// world coin) is named by what loaded it — the same on every client because
// every client loads the same data:
//   kind | source (file / sector uid) | instance name uid | ordinal
// The ordinal disambiguates identically named instances of one source and is
// assigned in load order (NetWorldRegistry). The 64-bit FNV-1a mix of those
// fields is the id; the top bit is always clear.
//
// A DYNAMIC entity (traffic car, ...) has no load identity; its id is minted by
// the owning peer from its server-assigned peer id and a local counter, with
// the top bit set, so the two namespaces never collide and two peers can never
// mint the same id.
namespace net
{
    enum WorldObjectKind : uint8_t
    {
        WOK_NONE       = 0,
        WOK_STATEPROP  = 1, // StatePropDSG placed by a level/zone file
        WOK_ANIMCOLL   = 2, // AnimCollisionEntityDSG (doors, platforms)
        WOK_PARKED_CAR = 3, // parked car slot
        WOK_WORLD_COIN = 4, // permanent world coin locator
    };

    static constexpr const uint64_t kDynamicEntityBit = 0x8000000000000000ull;

    inline uint64_t Fnv1a64(uint64_t h, const void* data, size_t n)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i)
        {
            h ^= p[i];
            h *= 0x100000001B3ull;
        }
        return h;
    }

    inline uint64_t MakeWorldObjectId(uint8_t kind, uint64_t sourceUid, uint64_t nameUid, uint32_t ordinal)
    {
        uint64_t h = 0xCBF29CE484222325ull;
        h = Fnv1a64(h, &kind, sizeof(kind));
        h = Fnv1a64(h, &sourceUid, sizeof(sourceUid));
        h = Fnv1a64(h, &nameUid, sizeof(nameUid));
        h = Fnv1a64(h, &ordinal, sizeof(ordinal));
        h &= ~kDynamicEntityBit;
        return h ? h : 1; // 0 means "no id"
    }

    inline uint64_t MakeDynamicEntityId(PeerId owner, uint32_t counter)
    {
        return kDynamicEntityBit | (static_cast<uint64_t>(owner) << 32) | counter;
    }

    inline bool IsDynamicEntityId(uint64_t id) { return (id & kDynamicEntityBit) != 0; }
}
