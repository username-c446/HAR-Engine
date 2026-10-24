#include "NetWorldHooks.h"
#include "NetworkMatchManager.h"
#include "EconomyConfig.h"

namespace networld
{
    uint64_t RegisterObject(uint8_t kind, uint64_t sourceUid, uint64_t nameUid, void* handle,
                            NetWorldRegistry::ApplyFn apply)
    {
        return NetworkMatchManager::WorldRegistry().Register(kind, sourceUid, nameUid, handle, apply);
    }

    void UnregisterObject(void* handle)
    {
        NetworkMatchManager::WorldRegistry().Unregister(handle);
    }

    bool RequestObjectState(void* handle, uint8_t channel, uint8_t policy, const uint8_t* value, uint8_t len)
    {
        return NetworkMatchManager::WorldRegistry().Request(handle, channel, policy, value, len);
    }

    uint64_t ObjectIdOf(void* handle)
    {
        return NetworkMatchManager::WorldRegistry().IdOf(handle);
    }

    void* ObjectHandleOf(uint64_t id)
    {
        return NetworkMatchManager::WorldRegistry().HandleOf(id);
    }

    bool IsApplyingRemote()
    {
        return NetworkMatchManager::WorldRegistry().IsApplying();
    }

    bool FindObjectState(uint8_t kind, uint64_t sourceUid, uint64_t nameUid, uint8_t channel,
                         net::WorldStateValue& out)
    {
        return NetworkMatchManager::WorldRegistry().FindKnown(
            net::MakeWorldObjectId(kind, sourceUid, nameUid, 0), channel, out);
    }

    void ReportCoins(uint8_t reason, int32_t delta, uint64_t claim)
    {
        NetworkMatchManager::Economy().Report(economy::COUNTER_COINS, reason, delta, claim);
    }

    bool IsEconomyConverging()
    {
        return NetworkMatchManager::Economy().IsConverging();
    }

    void AddLocalEntity(void* handle, const net::NetEntitySpawn& spawn, uint64_t boundId)
    {
        NetworkMatchManager::Entities().AddLocal(handle, spawn, boundId);
    }

    bool IsLocalEntity(void* handle)
    {
        return NetworkMatchManager::Entities().IsLocal(handle);
    }

    bool ReportEntityHit(void* remoteHandle, const net::NetEntityHit& hit)
    {
        return NetworkMatchManager::Entities().ReportHit(remoteHandle, hit);
    }

    bool ReportRemotePlayerHit(void* character, float vx, float vy, float vz)
    {
        NetworkMatchManager* m = NetworkMatchManager::GetInstance();
        if (m == nullptr || !m->IsInMatch()) return false;
        net::NetEntityHit hit;
        hit.ix = vx; hit.iy = vy; hit.iz = vz;
        return m->ReportRemotePlayerHit(character, hit);
    }

    bool ReportGhostHit(void* ghost, const net::NetEntityHit& hit)
    {
        if (NetworkMatchManager::Entities().ReportHit(ghost, hit)) return true;
        NetworkMatchManager* m = NetworkMatchManager::GetInstance();
        return m != nullptr && m->ReportRemoteVehicleHit(ghost, hit);
    }

    void RemoveLocalEntity(void* handle)
    {
        NetworkMatchManager::Entities().RemoveLocal(handle);
    }

    bool IsRemoteEntity(void* handle)
    {
        return NetworkMatchManager::Entities().IsRemoteHandle(handle);
    }

    int CountRemoteEntitiesNear(float x, float z, float radius, uint8_t kind)
    {
        return NetworkMatchManager::Entities().CountRemoteNear(x, z, radius, kind);
    }

    bool IsInMatch()
    {
        NetworkMatchManager* m = NetworkMatchManager::GetInstance();
        return m != nullptr && m->IsInMatch();
    }

    net::PeerId LocalPeerId()
    {
        NetworkMatchManager* m = NetworkMatchManager::GetInstance();
        return (m != nullptr && m->IsInMatch()) ? static_cast<net::PeerId>(m->GetLocalClientId()) : 0;
    }
}
