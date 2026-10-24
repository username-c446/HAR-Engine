#pragma once

#include "WorldEvent.h"
#include "BreakableWorldEventEmitter.h"
#include "CoinWorldEventEmitter.h"
#include "PlatformDirWorldEventEmitter.h"
#include "StatePropWorldEventEmitter.h"

#include <cstdint>
#include <functional>

// WorldEventApply
// ---------------
// Receiving half of the world-event channel. Game-agnostic so it is unit
// testable: the gameplay context binds the typed handlers to real world
// lookups (AnimEntityDSGManager, CoinManager, VehicleCentral).
//
// Echo suppression: applying a remote change goes through the same game code
// that emits the local event (StatePropDSG::SetState, SetAnimationDirection,
// ...). Every emitter checks IsApplyingRemoteWorldEvent() and stays silent
// while a RemoteWorldEventScope is alive, so a mirrored change is never
// re-broadcast back into the match (no ping-pong between peers).
namespace net
{
    // Depth counter for nested remote applies (a stateprop state change can
    // cascade into a break callback in the same call stack).
    inline int& RemoteWorldEventDepth()
    {
        static int s_depth = 0;
        return s_depth;
    }

    inline bool IsApplyingRemoteWorldEvent()
    {
        return RemoteWorldEventDepth() > 0;
    }

    struct RemoteWorldEventScope
    {
        RemoteWorldEventScope() { ++RemoteWorldEventDepth(); }
        ~RemoteWorldEventScope() { --RemoteWorldEventDepth(); }
        RemoteWorldEventScope(const RemoteWorldEventScope&) = delete;
        RemoteWorldEventScope& operator=(const RemoteWorldEventScope&) = delete;
    };

    // Typed handlers for inbound world events. Unset handlers ignore that type.
    struct WorldEventHandlers
    {
        std::function<void(uint16_t origin, const StatePropPayload&)>        stateProp;
        std::function<void(uint16_t origin, const BreakablePayload&)>        breakable;
        std::function<void(uint16_t origin, const VehicleDestroyedPayload&)> vehicleDestroyed;
        std::function<void(uint16_t origin, const CoinCollectedPayload&)>    coinCollected;
        std::function<void(uint16_t origin, const PlatformDirPayload&)>      platformDir;
    };

    // Decodes one inbound event and runs the matching handler inside a
    // RemoteWorldEventScope. Returns true if a handler was invoked; false for
    // invalid/truncated payloads, unknown types (newer peers) or unset handlers.
    inline bool DispatchWorldEvent(const WorldEventCodec::Decoded* ev, const WorldEventHandlers& h)
    {
        if (ev == nullptr || !ev->valid) return false;
        RemoteWorldEventScope scope;
        switch (ev->type)
        {
        case WE_STATEPROP:
        {
            StatePropPayload p;
            if (!h.stateProp || !StatePropPayload::Decode(ev->payload, ev->payloadSize, p)) return false;
            h.stateProp(ev->originPeer, p);
            return true;
        }
        case WE_BREAKABLE:
        {
            BreakablePayload p;
            if (!h.breakable || !BreakablePayload::Decode(ev->payload, ev->payloadSize, p)) return false;
            h.breakable(ev->originPeer, p);
            return true;
        }
        case WE_VEHICLE_DESTROYED:
        {
            VehicleDestroyedPayload p;
            if (!h.vehicleDestroyed || !VehicleDestroyedPayload::Decode(ev->payload, ev->payloadSize, p)) return false;
            h.vehicleDestroyed(ev->originPeer, p);
            return true;
        }
        case WE_COIN_COLLECTED:
        {
            CoinCollectedPayload p;
            if (!h.coinCollected || !CoinCollectedPayload::Decode(ev->payload, ev->payloadSize, p)) return false;
            h.coinCollected(ev->originPeer, p);
            return true;
        }
        case WE_PLATFORM_DIR:
        {
            PlatformDirPayload p;
            if (!h.platformDir || !PlatformDirPayload::Decode(ev->payload, ev->payloadSize, p)) return false;
            h.platformDir(ev->originPeer, p);
            return true;
        }
        default:
            return false; // unknown type from a newer peer: skip
        }
    }

    // World objects are identified by a type/name uid that is NOT unique per
    // instance (every vending machine shares its stateprop uid) plus the world
    // position the sender saw. Pick the candidate with a matching uid that is
    // nearest to that position, within maxDist. Returns the index into the
    // candidate set, or -1 when nothing qualifies.
    //   count       — number of candidates
    //   getUid(i)   — uid of candidate i (pass matchUid = false to ignore it)
    //   getPos(i, x, y, z) — world position of candidate i
    template <typename UidFn, typename PosFn>
    int FindNearestWorldObject(int count, bool matchUid, uint32_t uid,
                               float x, float y, float z, float maxDist,
                               UidFn getUid, PosFn getPos)
    {
        int best = -1;
        float bestSq = maxDist * maxDist;
        for (int i = 0; i < count; ++i)
        {
            if (matchUid && static_cast<uint32_t>(getUid(i)) != uid) continue;
            float px = 0.0f, py = 0.0f, pz = 0.0f;
            getPos(i, px, py, pz);
            float dx = px - x, dy = py - y, dz = pz - z;
            float dSq = dx * dx + dy * dy + dz * dz;
            if (dSq <= bestSq)
            {
                bestSq = dSq;
                best = i;
            }
        }
        return best;
    }
}
