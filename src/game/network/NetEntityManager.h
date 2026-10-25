#pragma once

#include "WorldObjectId.h"
#include "../netengine/WorldState.h"
#include "../netengine/NetTime.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>

// NetEntityManager
// ----------------
// Owner-simulated entities: traffic cars, pedestrians, actors, and static world
// objects while something moves them (a pushed parked car). The SERVER is the
// authority on which entities exist, what their ids are and who owns them (a
// WS_CHANNEL_ENTITY record, WSP_OWNER); the OWNER simulates and streams motion
// (PID_ENTITY_STATE, relayed only for records the sender owns); everyone else
// shows a kinematic ghost rendered from a timestamped snapshot buffer
// (interpolated interpolationDelay behind the server clock, briefly
// extrapolated when a packet is late). A late joiner receives every live
// entity in the snapshot.
//
// Ownership migration: when the owner gives an entity up (it left the owner's
// relevance range) while another player is near it, the entity is RELEASED
// instead of erased; a nearby peer CLAIMS it (first claim wins) and adopts the
// ghost into a simulated entity, so the car keeps driving for the player who
// sees it. An owner disconnecting releases its migratable entities the same way.
//
// Hits: a collision of local physics with a ghost is sent to the owner
// (PID_ENTITY_EVENT); the owner applies it to the real entity, whose stream then
// shows the reaction to everyone.
//
// Game-agnostic: ghosts are created/moved/destroyed/adopted through a Sink,
// local entities are sampled through a SampleFn.
namespace net
{
    enum NetEntityKind : uint8_t
    {
        NEK_NONE        = 0,
        NEK_TRAFFIC_CAR = 1, // dynamic id, migratable
        NEK_PARKED_CAR  = 2, // bound to a static world object (moved by a push)
        NEK_PEDESTRIAN  = 3, // dynamic id, migratable
        NEK_ACTOR       = 4, // dynamic id (wasps, collectible actors, ...)
    };

    inline bool IsMigratableKind(uint8_t kind) { return kind == NEK_TRAFFIC_CAR || kind == NEK_PEDESTRIAN; }

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

    // Streamed motion: i24 x,y,z (cm) | u16 yaw (2pi/65536) | u16 speedKmh | i8 steer
    //                  | u8 flags | u8 damage | u8 state (17 bytes; yaw decodes into (-pi, pi])
    struct NetEntityMotion
    {
        float    x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f;
        uint16_t speedKmh = 0;
        int8_t   steer = 0;    // -127..127 -> -45..+45 deg (vehicles)
        uint8_t  flags = 0;    // MF_* below
        uint8_t  damage = 0;   // 0 intact .. 255 wrecked (vehicles)
        uint8_t  state = 0;    // kind-specific animation / behaviour state (pedestrians, actors)

        static constexpr const uint8_t MF_BRAKE     = 0x01;
        static constexpr const uint8_t MF_REVERSE   = 0x02;
        static constexpr const uint8_t MF_DESTROYED = 0x04;
        static constexpr const uint8_t MF_KNOCKED   = 0x08; // pedestrian knocked down / ragdoll

        static constexpr const uint8_t SIZE = 3 * 3 + 2 + 2 + 1 + 1 + 1 + 1;
        static_assert(SIZE <= ENTITY_STATE_MAX, "motion record must fit an entity state record");
        void Encode(EntityStateRecord& out, uint64_t entity) const;
        bool Decode(const EntityStateRecord& in);
    };

    // Event kinds carried by PID_ENTITY_EVENT.
    enum NetEntityEventKind : uint8_t
    {
        NEE_HIT = 1,
    };

    // A hit on an entity: impulse (N*s, world), contact point (world), damage.
    // f32 ix,iy,iz | f32 px,py,pz | u8 damage  (25 bytes)
    struct NetEntityHit
    {
        float   ix = 0.0f, iy = 0.0f, iz = 0.0f;
        float   px = 0.0f, py = 0.0f, pz = 0.0f;
        uint8_t damage = 0;

        static constexpr const uint8_t SIZE = 6 * 4 + 1;
        uint16_t Encode(uint8_t* out, uint16_t cap) const;
        bool     Decode(const uint8_t* d, uint16_t n);
    };
}

class NetEntityManager
{
public:
    struct Sink
    {
        // Show a ghost (dynamic kinds) or take over the existing local object
        // (bound kinds); returns the handle passed back below.
        void* (*spawn)(uint64_t id, const net::NetEntitySpawn& spawn);
        void  (*move)(void* handle, const net::NetEntityMotion& motion);
        // Remove the ghost (dynamic) / hand the object back to local physics (bound).
        void  (*despawn)(void* handle, uint8_t kind);
        // We won the claim: turn the ghost into a simulated local entity at
        // `motion`. Returns its handle (the ghost is consumed) or null to refuse.
        void* (*adopt)(uint64_t id, void* ghostHandle, const net::NetEntitySpawn& spawn,
                       const net::NetEntityMotion& motion);
        // Someone hit our entity.
        void  (*hit)(void* localHandle, const net::NetEntityHit& hit);
    };
    typedef std::function<bool(const net::WorldStateOp&)> SendOpFn;
    typedef std::function<bool(const std::vector<net::EntityStateRecord>&)> SendStatesFn;
    typedef std::function<bool(uint64_t entity, uint8_t kind, const uint8_t* data, uint16_t size)> SendEventFn;
    typedef bool (*SampleFn)(void* handle, net::NetEntityMotion& out);
    // Server clock (ms). Without one ghosts show the newest sample directly.
    typedef std::function<uint32_t()> ClockFn;
    // Presence for migration: our player's position / whether another player is near.
    typedef std::function<bool(float& x, float& z)> LocalPlayerFn;
    typedef std::function<bool(float x, float z, float radius)> RemotePlayerNearFn;
    // Whether we may take over one more entity of this kind (local budget).
    // Whether our game would keep simulating an entity of `kind` at (x,y,z)
    // if we took it over (budget, and the same rules that would remove it
    // again — otherwise two players hand it back and forth).
    typedef std::function<bool(uint8_t kind, float x, float y, float z)> CanAdoptFn;

    void SetSink(const Sink& s) { m_sink = s; }
    void SetSenders(SendOpFn op, SendStatesFn states, SendEventFn events = SendEventFn())
    {
        m_sendOp = std::move(op);
        m_sendStates = std::move(states);
        m_sendEvent = std::move(events);
    }
    void SetSampler(SampleFn fn) { m_sample = fn; }
    void SetClock(ClockFn fn) { m_clock = std::move(fn); }
    void SetPresence(LocalPlayerFn local, RemotePlayerNearFn remote, CanAdoptFn canAdopt)
    {
        m_localPlayer = std::move(local);
        m_remoteNear = std::move(remote);
        m_canAdopt = std::move(canAdopt);
    }
    void SetSendIntervalMs(uint32_t ms) { m_sendIntervalMs = ms; }
    void SetInterpolationDelayMs(uint32_t ms) { m_interpDelayMs = ms; }
    void SetMaxExtrapolationMs(uint32_t ms) { m_maxExtrapMs = ms; }
    // Hand an entity over when another player is within `handover` of it;
    // claim released entities within `claim` of our player.
    void SetMigrationRadii(float handover, float claim) { m_handoverRadius = handover; m_claimRadius = claim; }
    // Interest management: a ghost of a dynamic entity is shown only within
    // `radius` of our player (removed beyond 1.25x, the record is kept); 0 =
    // everywhere. A ghost the sink could not create is retried every frame.
    // With a clock, a moving entity is shown only while its owner's samples
    // keep arriving (the server forwards them only near us): a record whose
    // newest sample is older than kStaleMs is not trusted as a position.
    void SetRelevanceRadius(float radius) { m_relevanceRadius = radius; }
    static constexpr const uint32_t kStaleMs = 1000;

    // Match lifecycle. OnWelcomed announces entities created before our peer
    // id was known; OnMatchEnd removes every ghost and forgets announcements.
    void OnWelcomed(net::PeerId self);
    void OnMatchEnd();

    // ---- local (owned) entities ----
    // `boundId` != 0: the entity is a static world object with that id (a
    // parked car being pushed); otherwise a dynamic id is minted.
    void AddLocal(void* handle, const net::NetEntitySpawn& spawn, uint64_t boundId = 0);
    // The local simulation drops the entity: released to a nearby player when
    // migratable and someone is near, erased otherwise.
    void RemoveLocal(void* handle);
    bool IsLocal(void* handle) const { return m_localByHandle.count(handle) != 0; }
    uint64_t LocalId(void* handle) const;
    // Server id of a ghost / taken-over object (0 if `handle` is not remote).
    uint64_t RemoteId(void* handle) const;

    // ---- hits ----
    // Our physics hit a ghost: forward to its owner. False if not a ghost.
    bool ReportHit(void* remoteHandle, const net::NetEntityHit& hit);

    // ---- inbound ----
    void OnAuthoritative(const net::WorldStateEntry& e, net::WorldStateReason reason);
    void OnEntityState(net::PeerId owner, const net::EntityStateRecord& rec, uint32_t serverTimeMs = 0);
    void OnEntityEvent(net::PeerId from, uint64_t entity, uint8_t kind, const uint8_t* data, uint16_t size);

    // ---- per frame ----
    void Update(float dtSeconds);   // render ghosts, claim / stream owned motion

    // ---- queries ----
    size_t RemoteCount() const { return m_remote.size(); }
    size_t LocalCount() const { return m_localByHandle.size(); }
    // Ghosts of `kind` (NEK_NONE = any) within `radius` of (x,z) — the shared
    // density budget, so two players side by side do not double the traffic.
    int CountRemoteNear(float x, float z, float radius, uint8_t kind = net::NEK_NONE) const;
    bool IsRemoteHandle(void* handle) const { return RemoteId(handle) != 0; }

private:
    struct Local
    {
        uint64_t            id = 0;      // 0 until announced (needs our peer id)
        uint64_t            boundId = 0; // static object id for bound kinds
        net::NetEntitySpawn spawn;
    };
    struct Remote
    {
        void*               handle = nullptr;
        net::PeerId         owner = 0;       // 0 = released, waiting for a claim
        net::NetEntitySpawn spawn;
        net::NetEntityMotion visual;
        net::SnapshotBuffer<net::NetEntityMotion> samples;
        bool                claimSent = false;
    };

    void Announce(Local& l);
    void SpawnRemote(const net::WorldStateEntry& e);
    void DespawnRemote(uint64_t id);
    void Adopt(uint64_t id);
    void RenderGhosts();
    void UpdateRelevance();
    bool PoseKnown(const Remote& r) const;
    void TryClaims();
    void StreamOwned();
    bool SendOp(uint64_t id, uint8_t kind, uint8_t flags, const net::WorldStateValue* value);

    Sink         m_sink = { nullptr, nullptr, nullptr, nullptr, nullptr };
    SendOpFn     m_sendOp;
    SendStatesFn m_sendStates;
    SendEventFn  m_sendEvent;
    SampleFn     m_sample = nullptr;
    ClockFn      m_clock;
    LocalPlayerFn      m_localPlayer;
    RemotePlayerNearFn m_remoteNear;
    CanAdoptFn         m_canAdopt;

    net::PeerId  m_self = 0;
    bool         m_welcomed = false;
    uint32_t     m_nextCounter = 1;
    uint32_t     m_sendIntervalMs = 100;
    uint32_t     m_interpDelayMs = 150;
    uint32_t     m_maxExtrapMs = 250;
    float        m_sinceSend = 0.0f;
    float        m_handoverRadius = 150.0f;
    float        m_claimRadius = 120.0f;
    float        m_relevanceRadius = 0.0f;

    std::map<void*, Local>     m_localByHandle;
    std::map<uint64_t, Remote> m_remote;
};
