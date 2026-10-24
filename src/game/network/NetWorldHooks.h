#pragma once

#include "WorldObjectId.h"
#include "NetWorldRegistry.h"
#include "NetEntityManager.h"
#include "../netengine/Ledger.h"

#include <cstdint>

// Thin entry points for game systems (DSGs, loaders, coins, traffic) into the
// replicated-world services, so those systems do not depend on the match
// manager or the transport. All calls are safe outside a match: registration
// always works (objects load before any match exists), requests are ignored.
namespace networld
{
    // ---- static world objects (NetWorldRegistry) ----
    uint64_t RegisterObject(uint8_t kind, uint64_t sourceUid, uint64_t nameUid, void* handle,
                            NetWorldRegistry::ApplyFn apply);
    void     UnregisterObject(void* handle);
    // A local change already applied by the caller. No-op while a remote change
    // is being applied (so mirrors are never re-sent) or outside a match.
    bool     RequestObjectState(void* handle, uint8_t channel, uint8_t policy, const uint8_t* value, uint8_t len);
    bool     IsApplyingRemote();
    // Authoritative state already known for a unique object (ordinal 0).
    bool     FindObjectState(uint8_t kind, uint64_t sourceUid, uint64_t nameUid, uint8_t channel,
                             net::WorldStateValue& out);

    // ---- economy (PlayerEconomy -> server ledger) ----
    // Reports a change already applied to the local coin bank.
    void     ReportCoins(uint8_t reason, int32_t delta, uint64_t claim = 0);
    bool     IsEconomyConverging();

    // ---- owner-simulated entities (NetEntityManager) ----
    void     AddLocalEntity(void* handle, const net::NetEntitySpawn& spawn);
    void     RemoveLocalEntity(void* handle);
    bool     IsRemoteEntity(void* handle);
    int      CountRemoteEntitiesNear(float x, float z, float radius);
    bool     IsInMatch();
}
