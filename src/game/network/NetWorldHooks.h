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
    // Network id of a registered object (0 = not registered) and back.
    uint64_t ObjectIdOf(void* handle);
    void*    ObjectHandleOf(uint64_t id);
    // Authoritative state already known for a unique object (ordinal 0).
    bool     FindObjectState(uint8_t kind, uint64_t sourceUid, uint64_t nameUid, uint8_t channel,
                             net::WorldStateValue& out);

    // ---- economy (PlayerEconomy -> server ledger) ----
    // Reports a change already applied to the local coin bank.
    void     ReportCoins(uint8_t reason, int32_t delta, uint64_t claim = 0);
    bool     IsEconomyConverging();

    // ---- owner-simulated entities (NetEntityManager) ----
    // `boundId`: the static world object being moved (parked car), 0 = dynamic.
    void     AddLocalEntity(void* handle, const net::NetEntitySpawn& spawn, uint64_t boundId = 0);
    void     RemoveLocalEntity(void* handle);
    bool     IsLocalEntity(void* handle);
    bool     IsRemoteEntity(void* handle);
    int      CountRemoteEntitiesNear(float x, float z, float radius, uint8_t kind = net::NEK_NONE);
    // Our physics hit another player's entity (a ghost): forward to its owner.
    bool     ReportEntityHit(void* remoteHandle, const net::NetEntityHit& hit);
    // Our vehicle hit a ghost machine: an entity's (to its owner) or a remote
    // player's own car (to that player, only with PvP).
    bool     ReportGhostHit(void* ghost, const net::NetEntityHit& hit);
    // Our attack hit a character: if it is a remote player's puppet, the knock
    // is sent to that player (PvP) and true is returned — the caller must not
    // apply it locally.
    bool     ReportRemotePlayerHit(void* character, float vx, float vy, float vz);
    bool     IsInMatch();
    // Our peer id in the running match (0 = none).
    net::PeerId LocalPeerId();
}
