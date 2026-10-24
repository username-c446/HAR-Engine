#pragma once

#include "WorldObjectId.h"
#include "../netengine/WorldState.h"

#include <cstdint>
#include <functional>
#include <map>
#include <vector>

// NetEntityManager
// ----------------
// Dynamic, owner-simulated entities (traffic cars today; any spawned actor
// later). The SERVER is the authority on which entities exist, what their ids
// are and who owns them (a WS_CHANNEL_ENTITY record, WSP_OWNER, erased when the
// owner leaves); the OWNER simulates and streams motion (PID_ENTITY_STATE, the
// server relays only records the sender owns); everyone else shows a smoothed
// kinematic ghost. A late joiner receives every live entity in the snapshot.
//
// Game-agnostic: ghosts are created/moved/destroyed through a Sink, local
// entities are sampled through a SampleFn.
namespace net
{
    enum NetEntityKind : uint8_t
    {
        NEK_NONE        = 0,
        NEK_TRAFFIC_CAR = 1,
    };

    // Existence record value: u8 kind | char model[16] | f32 x,y,z,yaw  (33 bytes)
    struct NetEntitySpawn
    {
        uint8_t kind = NEK_NONE;
        char    model[16] = { 0 };
        float   x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;

        static constexpr const uint8_t SIZE = 1 + 16 + 4 * 4;
        static_assert(SIZE <= WS_MAX_VALUE, "spawn record must fit a world-state value");
        bool Encode(WorldStateValue& out) const;
        bool Decode(const WorldStateValue& in);
    };

    // Streamed motion: f32 x,y,z,yaw | u16 speedKmh | i8 steer | u8 flags (20 bytes)
    struct NetEntityMotion
    {
        float    x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;
        uint16_t speedKmh = 0;
        int8_t   steer = 0;    // -127..127 -> -45..+45 deg
        uint8_t  flags = 0;    // MF_* below

        static constexpr const uint8_t MF_BRAKE     = 0x01;
        static constexpr const uint8_t MF_REVERSE   = 0x02;
        static constexpr const uint8_t MF_DESTROYED = 0x04;

        static constexpr const uint8_t SIZE = 4 * 4 + 2 + 1 + 1;
        static_assert(SIZE <= ENTITY_STATE_MAX, "motion record must fit an entity state record");
        void Encode(EntityStateRecord& out, uint64_t entity) const;
        bool Decode(const EntityStateRecord& in);
    };
}

class NetEntityManager
{
public:
    struct Sink
    {
        void* (*spawn)(uint64_t id, const net::NetEntitySpawn& spawn);
        void  (*move)(void* handle, const net::NetEntityMotion& motion);
        void  (*despawn)(void* handle);
    };
    typedef std::function<bool(const net::WorldStateOp&)> SendOpFn;
    typedef std::function<bool(const std::vector<net::EntityStateRecord>&)> SendStatesFn;
    typedef bool (*SampleFn)(void* handle, net::NetEntityMotion& out);

    void SetSink(const Sink& s) { m_sink = s; }
    void SetSenders(SendOpFn op, SendStatesFn states) { m_sendOp = std::move(op); m_sendStates = std::move(states); }
    void SetSampler(SampleFn fn) { m_sample = fn; }
    void SetSendEvery(unsigned frames) { m_sendEvery = frames ? frames : 1; }
    void SetSmoothing(float seconds) { m_smoothing = seconds > 0.0f ? seconds : 0.12f; }

    // Match lifecycle. OnWelcomed announces entities created before our peer
    // id was known; OnMatchEnd removes every ghost and forgets announcements.
    void OnWelcomed(net::PeerId self);
    void OnMatchEnd();

    // ---- local (owned) entities ----
    void AddLocal(void* handle, const net::NetEntitySpawn& spawn);
    void RemoveLocal(void* handle);
    bool IsLocal(void* handle) const { return m_localByHandle.count(handle) != 0; }
    uint64_t LocalId(void* handle) const;

    // ---- inbound ----
    void OnAuthoritative(const net::WorldStateEntry& e, net::WorldStateReason reason);
    void OnEntityState(net::PeerId owner, const net::EntityStateRecord& rec);

    // ---- per frame ----
    void Update(float dtSeconds);   // smooth ghosts + stream owned motion on cadence

    // ---- queries ----
    size_t RemoteCount() const { return m_remote.size(); }
    size_t LocalCount() const { return m_localByHandle.size(); }
    // Counts ghosts within `radius` of (x,z) — used by local spawners to keep
    // the shared density budget (two players side by side do not double traffic).
    int CountRemoteNear(float x, float z, float radius) const;
    bool IsRemoteHandle(void* handle) const;

private:
    struct Local
    {
        uint64_t            id = 0;      // 0 until announced (needs our peer id)
        net::NetEntitySpawn spawn;
    };
    struct Remote
    {
        void*               handle = nullptr;
        net::PeerId         owner = 0;
        net::NetEntityMotion target;
        net::NetEntityMotion visual;
        bool                haveMotion = false;
    };

    void Announce(void* handle, Local& l);
    void SpawnRemote(const net::WorldStateEntry& e);
    void DespawnRemote(uint64_t id);

    Sink         m_sink = { nullptr, nullptr, nullptr };
    SendOpFn     m_sendOp;
    SendStatesFn m_sendStates;
    SampleFn     m_sample = nullptr;
    net::PeerId  m_self = 0;
    bool         m_welcomed = false;
    uint32_t     m_nextCounter = 1;
    unsigned     m_sendEvery = 6;
    unsigned     m_tick = 0;
    float        m_smoothing = 0.12f;

    std::map<void*, Local>     m_localByHandle;
    std::map<uint64_t, Remote> m_remote;
};
