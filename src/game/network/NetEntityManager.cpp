#include "NetEntityManager.h"
#include "../netengine/Quantize.h"

#include <cmath>
#include <cstring>

namespace net
{
    bool NetEntitySpawn::Encode(WorldStateValue& out) const
    {
        Writer w(out.bytes, WS_MAX_VALUE);
        bool ok = w.WriteU8(kind) && w.WriteBytes(model, sizeof(model)) && w.WriteFloat(x)
               && w.WriteFloat(y) && w.WriteFloat(z) && w.WriteFloat(yaw);
        if (!ok || !w.Ok()) return false;
        out.len = static_cast<uint8_t>(w.Position());
        return true;
    }

    bool NetEntitySpawn::Decode(const WorldStateValue& in)
    {
        Reader r(in.bytes, in.len);
        if (!(r.ReadU8(kind) && r.ReadBytes(model, sizeof(model)) && r.ReadFloat(x)
              && r.ReadFloat(y) && r.ReadFloat(z) && r.ReadFloat(yaw)))
            return false;
        model[sizeof(model) - 1] = '\0'; // never trust a peer's string terminator
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(yaw);
    }

    void NetEntityMotion::Encode(EntityStateRecord& out, uint64_t entity) const
    {
        out.entity = entity;
        Writer w(out.bytes, ENTITY_STATE_MAX);
        WriteCentimetres24(w, x);
        WriteCentimetres24(w, y);
        WriteCentimetres24(w, z);
        w.WriteU16(EncodeAngle16(yaw));
        w.WriteU16(speedKmh);
        w.WriteU8(static_cast<uint8_t>(steer));
        w.WriteU8(flags);
        w.WriteU8(damage);
        w.WriteU8(state);
        out.len = static_cast<uint8_t>(w.Position());
    }

    bool NetEntityMotion::Decode(const EntityStateRecord& in)
    {
        Reader r(in.bytes, in.len);
        uint8_t s = 0;
        uint16_t a = 0;
        if (!(ReadCentimetres24(r, x) && ReadCentimetres24(r, y) && ReadCentimetres24(r, z) && r.ReadU16(a)
              && r.ReadU16(speedKmh) && r.ReadU8(s) && r.ReadU8(flags) && r.ReadU8(damage) && r.ReadU8(state)))
            return false;
        yaw = DecodeAngle16(a);
        steer = static_cast<int8_t>(s);
        return true;
    }

    uint16_t NetEntityHit::Encode(uint8_t* out, uint16_t cap) const
    {
        Writer w(out, cap);
        w.WriteFloat(ix); w.WriteFloat(iy); w.WriteFloat(iz);
        w.WriteFloat(px); w.WriteFloat(py); w.WriteFloat(pz);
        w.WriteU8(damage);
        return w.Ok() ? w.Position() : 0;
    }

    bool NetEntityHit::Decode(const uint8_t* d, uint16_t n)
    {
        Reader r(d, n);
        if (!(r.ReadFloat(ix) && r.ReadFloat(iy) && r.ReadFloat(iz)
              && r.ReadFloat(px) && r.ReadFloat(py) && r.ReadFloat(pz) && r.ReadU8(damage)))
            return false;
        return std::isfinite(ix) && std::isfinite(iy) && std::isfinite(iz)
            && std::isfinite(px) && std::isfinite(py) && std::isfinite(pz);
    }
}

namespace
{
    const float kPi = 3.14159265358979f;

    float LerpAngle(float a, float b, float t)
    {
        float d = b - a;
        while (d > kPi)  d -= 2.0f * kPi;
        while (d < -kPi) d += 2.0f * kPi;
        return a + d * t;
    }

    net::NetEntityMotion Blend(const net::NetEntityMotion& a, const net::NetEntityMotion& b, float t)
    {
        net::NetEntityMotion m = t < 0.5f ? a : b; // discrete fields from the nearer sample
        m.x = a.x + (b.x - a.x) * t;
        m.y = a.y + (b.y - a.y) * t;
        m.z = a.z + (b.z - a.z) * t;
        m.yaw = LerpAngle(a.yaw, b.yaw, t);
        return m;
    }
}

// ------------------------------------------------------------- lifecycle

void NetEntityManager::OnWelcomed(net::PeerId self)
{
    m_self = self;
    m_welcomed = true;
    for (auto& kv : m_localByHandle)
        if (kv.second.id == 0) Announce(kv.second);
}

void NetEntityManager::OnMatchEnd()
{
    for (auto& kv : m_remote)
        if (m_sink.despawn && kv.second.handle) m_sink.despawn(kv.second.handle, kv.second.spawn.kind);
    m_remote.clear();
    for (auto& kv : m_localByHandle) kv.second.id = 0; // re-announce in the next match
    m_welcomed = false;
    m_self = 0;
    m_sinceSend = 0.0f;
}

// ---------------------------------------------------------------- local

bool NetEntityManager::SendOp(uint64_t id, uint8_t kind, uint8_t flags, const net::WorldStateValue* value)
{
    if (!m_sendOp) return false;
    net::WorldStateOp op;
    op.key.object = id;
    op.key.channel = net::WS_CHANNEL_ENTITY;
    op.kind = kind;
    op.policy = net::WSP_OWNER;
    op.flags = flags;
    if (value) op.value = *value;
    return m_sendOp(op);
}

void NetEntityManager::Announce(Local& l)
{
    if (!m_welcomed || !m_sendOp) return;
    uint64_t id = l.boundId ? l.boundId : net::MakeDynamicEntityId(m_self, m_nextCounter++);
    // Migratable entities survive their owner leaving (another player takes
    // them over); the others disappear with it.
    uint8_t flags = net::IsMigratableKind(l.spawn.kind) ? net::WSF_RELEASE_ON_OWNER_LEAVE
                                                        : net::WSF_ERASE_ON_OWNER_LEAVE;
    net::WorldStateValue v;
    if (!l.spawn.Encode(v)) return;
    if (SendOp(id, net::WSO_SET, flags, &v)) l.id = id;
}

void NetEntityManager::AddLocal(void* handle, const net::NetEntitySpawn& spawn, uint64_t boundId)
{
    if (handle == nullptr || m_localByHandle.count(handle)) return;
    Local& l = m_localByHandle[handle];
    l.spawn = spawn;
    l.boundId = boundId;
    Announce(l);
}

void NetEntityManager::RemoveLocal(void* handle)
{
    auto it = m_localByHandle.find(handle);
    if (it == m_localByHandle.end()) return;
    Local& l = it->second;
    if (l.id != 0)
    {
        // Hand over instead of deleting when another player can see it.
        net::NetEntityMotion m;
        bool sampled = m_sample && m_sample(handle, m);
        if (sampled && net::IsMigratableKind(l.spawn.kind) && m_remoteNear
            && m_remoteNear(m.x, m.z, m_handoverRadius))
        {
            net::NetEntitySpawn pose = l.spawn;
            pose.x = m.x; pose.y = m.y; pose.z = m.z; pose.yaw = m.yaw;
            net::WorldStateValue v;
            if (pose.Encode(v)) SendOp(l.id, net::WSO_RELEASE, 0, &v);
        }
        else
        {
            SendOp(l.id, net::WSO_ERASE, 0, nullptr);
        }
    }
    m_localByHandle.erase(it);
}

uint64_t NetEntityManager::LocalId(void* handle) const
{
    auto it = m_localByHandle.find(handle);
    return it == m_localByHandle.end() ? 0 : it->second.id;
}

uint64_t NetEntityManager::RemoteId(void* handle) const
{
    if (handle == nullptr) return 0;
    for (const auto& kv : m_remote)
        if (kv.second.handle == handle) return kv.first;
    return 0;
}

// ----------------------------------------------------------------- hits

bool NetEntityManager::ReportHit(void* remoteHandle, const net::NetEntityHit& hit)
{
    uint64_t id = RemoteId(remoteHandle);
    if (id == 0 || !m_sendEvent) return false;
    if (m_remote[id].owner == 0) return false; // released: nobody simulates it
    uint8_t buf[net::NetEntityHit::SIZE];
    uint16_t n = hit.Encode(buf, sizeof(buf));
    return n != 0 && m_sendEvent(id, net::NEE_HIT, buf, n);
}

void NetEntityManager::OnEntityEvent(net::PeerId from, uint64_t entity, uint8_t kind,
                                     const uint8_t* data, uint16_t size)
{
    (void)from;
    if (kind != net::NEE_HIT || !m_sink.hit) return;
    net::NetEntityHit hit;
    if (!hit.Decode(data, size)) return;
    for (auto& kv : m_localByHandle)
        if (kv.second.id == entity)
        {
            m_sink.hit(kv.first, hit);
            return;
        }
}

// -------------------------------------------------------------- inbound

void NetEntityManager::OnAuthoritative(const net::WorldStateEntry& e, net::WorldStateReason reason)
{
    if (e.key.channel != net::WS_CHANNEL_ENTITY) return;
    const uint64_t id = e.key.object;

    if (reason == net::WSR_ERASED)
    {
        DespawnRemote(id);
        for (auto& kv : m_localByHandle)
            if (kv.second.id == id) kv.second.id = 0; // no longer known to the server
        return;
    }

    if (reason == net::WSR_REJECT)
    {
        // Our announcement lost (someone else already moves this bound object)
        // or our claim lost: follow the authoritative owner instead.
        for (auto it = m_localByHandle.begin(); it != m_localByHandle.end(); ++it)
            if (it->second.id == id)
            {
                m_localByHandle.erase(it);
                break;
            }
        auto r = m_remote.find(id);
        if (r != m_remote.end())
        {
            r->second.owner = e.owner;
            r->second.claimSent = false;
            return;
        }
        if (e.owner != 0 && e.owner != m_self) SpawnRemote(e);
        return;
    }

    auto r = m_remote.find(id);
    if (m_welcomed && e.owner == m_self)
    {
        if (r != m_remote.end()) Adopt(id); // our claim was granted
        return;                              // otherwise: confirmation of our own
    }
    if (r != m_remote.end())
    {
        // Owner change (released / claimed by someone else) or a new pose.
        Remote& rem = r->second;
        rem.owner = e.owner;
        if (e.owner != 0) rem.claimSent = false;
        net::NetEntitySpawn s;
        if (s.Decode(e.value)) rem.spawn = s;
        if (e.owner == 0)
        {
            // Released: the record carries its final pose, nothing streams it.
            rem.samples.Clear();
            rem.visual.x = rem.spawn.x;
            rem.visual.y = rem.spawn.y;
            rem.visual.z = rem.spawn.z;
            rem.visual.yaw = rem.spawn.yaw;
        }
        return;
    }
    SpawnRemote(e);
}

void NetEntityManager::SpawnRemote(const net::WorldStateEntry& e)
{
    net::NetEntitySpawn spawn;
    if (!spawn.Decode(e.value)) return;

    Remote r;
    r.owner = e.owner;
    r.spawn = spawn;
    r.visual.x = spawn.x;
    r.visual.y = spawn.y;
    r.visual.z = spawn.z;
    r.visual.yaw = spawn.yaw;
    // The ghost itself is created by UpdateRelevance (within range, retried
    // when the sink cannot create one now); bound objects take over at once.
    r.handle = nullptr;
    if (m_sink.spawn && (m_relevanceRadius <= 0.0f || spawn.kind == net::NEK_PARKED_CAR))
        r.handle = m_sink.spawn(e.key.object, spawn);
    Remote& stored = m_remote[e.key.object];
    stored = r;
    if (stored.handle && m_sink.move) m_sink.move(stored.handle, stored.visual);
}

void NetEntityManager::DespawnRemote(uint64_t id)
{
    auto it = m_remote.find(id);
    if (it == m_remote.end()) return;
    if (m_sink.despawn && it->second.handle) m_sink.despawn(it->second.handle, it->second.spawn.kind);
    m_remote.erase(it);
}

void NetEntityManager::Adopt(uint64_t id)
{
    auto it = m_remote.find(id);
    if (it == m_remote.end()) return;
    Remote& r = it->second;
    net::NetEntityMotion m = r.samples.Empty() ? r.visual : r.samples.Newest().value;
    void* local = (m_sink.adopt && r.handle) ? m_sink.adopt(id, r.handle, r.spawn, m) : nullptr;
    if (local == nullptr)
    {
        // Cannot simulate it after all: give it back for someone else.
        net::NetEntitySpawn pose = r.spawn;
        pose.x = m.x; pose.y = m.y; pose.z = m.z; pose.yaw = m.yaw;
        net::WorldStateValue v;
        if (pose.Encode(v)) SendOp(id, net::WSO_RELEASE, 0, &v);
        r.owner = 0;
        return; // claimSent stays set: we do not claim it again
    }
    Local l;
    l.id = id;
    l.spawn = r.spawn;
    m_remote.erase(it); // the ghost handle was consumed by adopt
    m_localByHandle[local] = l;
}

void NetEntityManager::OnEntityState(net::PeerId owner, const net::EntityStateRecord& rec, uint32_t serverTimeMs)
{
    auto it = m_remote.find(rec.entity);
    if (it == m_remote.end() || it->second.owner != owner) return;
    net::NetEntityMotion m;
    if (!m.Decode(rec)) return;
    it->second.samples.Push(serverTimeMs, m);
}

// ------------------------------------------------------------ per frame

bool NetEntityManager::PoseKnown(const Remote& r) const
{
    // Bound objects stand where the world put them, a released entity at its
    // final pose; a moving one is only known while its samples are fresh.
    if (r.spawn.kind == net::NEK_PARKED_CAR || r.owner == 0) return true;
    if (!m_clock || m_relevanceRadius <= 0.0f) return true;
    if (r.samples.Empty()) return false;
    return net::TimeDiff(m_clock() - m_interpDelayMs, r.samples.Newest().time) <= static_cast<int32_t>(kStaleMs);
}

void NetEntityManager::UpdateRelevance()
{
    float px = 0.0f, pz = 0.0f;
    bool havePlayer = m_relevanceRadius > 0.0f && m_localPlayer && m_localPlayer(px, pz);
    const float showR2 = m_relevanceRadius * m_relevanceRadius;
    const float hideR2 = showR2 * 1.5625f; // 1.25x radius
    for (auto& kv : m_remote)
    {
        Remote& r = kv.second;
        bool bound = r.spawn.kind == net::NEK_PARKED_CAR; // an existing local object
        float d2 = 0.0f;
        if (havePlayer && !bound)
        {
            float dx = r.visual.x - px, dz = r.visual.z - pz;
            d2 = dx * dx + dz * dz;
        }
        const bool known = PoseKnown(r);
        if (r.handle == nullptr)
        {
            if (known && (!havePlayer || bound || d2 <= showR2) && m_sink.spawn)
            {
                r.handle = m_sink.spawn(kv.first, r.spawn);
                if (r.handle && m_sink.move) m_sink.move(r.handle, r.visual);
            }
        }
        else if (!known || (havePlayer && !bound && d2 > hideR2))
        {
            if (m_sink.despawn) m_sink.despawn(r.handle, r.spawn.kind);
            r.handle = nullptr;
        }
    }
}

void NetEntityManager::RenderGhosts()
{
    const bool timed = static_cast<bool>(m_clock);
    const uint32_t renderTime = timed ? m_clock() - m_interpDelayMs : 0;
    for (auto& kv : m_remote)
    {
        Remote& r = kv.second;
        if (!r.samples.Empty())
        {
            if (!timed)
            {
                r.visual = r.samples.Newest().value;
            }
            else
            {
                const net::NetEntityMotion* a = nullptr;
                const net::NetEntityMotion* b = nullptr;
                float t = 0.0f;
                if (r.samples.Sample(renderTime, m_maxExtrapMs, a, b, t)) r.visual = Blend(*a, *b, t);
                r.samples.DiscardBefore(renderTime);
            }
        }
        if (r.handle && m_sink.move) m_sink.move(r.handle, r.visual);
    }
}

void NetEntityManager::TryClaims()
{
    if (!m_localPlayer || !m_sendOp) return;
    float px = 0.0f, pz = 0.0f;
    if (!m_localPlayer(px, pz)) return;
    const float r2 = m_claimRadius * m_claimRadius;
    for (auto& kv : m_remote)
    {
        Remote& r = kv.second;
        if (r.owner != 0 || r.claimSent || !net::IsMigratableKind(r.spawn.kind)) continue;
        float dx = r.visual.x - px, dz = r.visual.z - pz;
        if (dx * dx + dz * dz > r2) continue;
        if (m_canAdopt && !m_canAdopt(r.spawn.kind, r.visual.x, r.visual.y, r.visual.z)) continue;
        if (SendOp(kv.first, net::WSO_CLAIM, 0, nullptr)) r.claimSent = true;
    }
}

void NetEntityManager::StreamOwned()
{
    if (!m_sample || !m_sendStates) return;
    std::vector<net::EntityStateRecord> out;
    out.reserve(m_localByHandle.size());
    for (auto& kv : m_localByHandle)
    {
        if (kv.second.id == 0) continue;
        net::NetEntityMotion m;
        if (!m_sample(kv.first, m)) continue;
        net::EntityStateRecord rec;
        m.Encode(rec, kv.second.id);
        out.push_back(rec);
    }
    if (!out.empty()) m_sendStates(out);
}

void NetEntityManager::Update(float dt)
{
    RenderGhosts();
    UpdateRelevance();
    if (!m_welcomed) return;
    TryClaims();

    m_sinceSend += dt > 0.0f ? dt : 0.0f;
    if (m_sinceSend * 1000.0f < static_cast<float>(m_sendIntervalMs)) return;
    m_sinceSend = 0.0f;
    StreamOwned();
}

// --------------------------------------------------------------- queries

int NetEntityManager::CountRemoteNear(float x, float z, float radius, uint8_t kind) const
{
    int n = 0;
    float r2 = radius * radius;
    for (const auto& kv : m_remote)
    {
        if (kind != net::NEK_NONE && kv.second.spawn.kind != kind) continue;
        if (!PoseKnown(kv.second)) continue;
        float dx = kv.second.visual.x - x, dz = kv.second.visual.z - z;
        if (dx * dx + dz * dz <= r2) ++n;
    }
    return n;
}
