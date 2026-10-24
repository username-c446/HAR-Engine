#include "NetEntityManager.h"

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
        return true;
    }

    void NetEntityMotion::Encode(EntityStateRecord& out, uint64_t entity) const
    {
        out.entity = entity;
        Writer w(out.bytes, ENTITY_STATE_MAX);
        w.WriteFloat(x); w.WriteFloat(y); w.WriteFloat(z); w.WriteFloat(yaw);
        w.WriteU16(speedKmh);
        w.WriteU8(static_cast<uint8_t>(steer));
        w.WriteU8(flags);
        out.len = static_cast<uint8_t>(w.Position());
    }

    bool NetEntityMotion::Decode(const EntityStateRecord& in)
    {
        Reader r(in.bytes, in.len);
        uint8_t s = 0;
        if (!(r.ReadFloat(x) && r.ReadFloat(y) && r.ReadFloat(z) && r.ReadFloat(yaw)
              && r.ReadU16(speedKmh) && r.ReadU8(s) && r.ReadU8(flags)))
            return false;
        steer = static_cast<int8_t>(s);
        return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::isfinite(yaw);
    }
}

// ------------------------------------------------------------- lifecycle

void NetEntityManager::OnWelcomed(net::PeerId self)
{
    m_self = self;
    m_welcomed = true;
    for (auto& kv : m_localByHandle)
        if (kv.second.id == 0) Announce(kv.first, kv.second);
}

void NetEntityManager::OnMatchEnd()
{
    for (auto& kv : m_remote)
        if (m_sink.despawn && kv.second.handle) m_sink.despawn(kv.second.handle);
    m_remote.clear();
    for (auto& kv : m_localByHandle) kv.second.id = 0; // re-announce in the next match
    m_welcomed = false;
    m_self = 0;
    m_tick = 0;
}

// ---------------------------------------------------------------- local

void NetEntityManager::Announce(void* handle, Local& l)
{
    (void)handle;
    if (!m_welcomed || !m_sendOp) return;
    net::WorldStateOp op;
    op.key.object = net::MakeDynamicEntityId(m_self, m_nextCounter++);
    op.key.channel = net::WS_CHANNEL_ENTITY;
    op.kind = net::WSO_SET;
    op.policy = net::WSP_OWNER;
    op.flags = net::WSF_ERASE_ON_OWNER_LEAVE;
    if (!l.spawn.Encode(op.value)) return;
    if (m_sendOp(op)) l.id = op.key.object;
}

void NetEntityManager::AddLocal(void* handle, const net::NetEntitySpawn& spawn)
{
    if (handle == nullptr || m_localByHandle.count(handle)) return;
    Local& l = m_localByHandle[handle];
    l.spawn = spawn;
    Announce(handle, l);
}

void NetEntityManager::RemoveLocal(void* handle)
{
    auto it = m_localByHandle.find(handle);
    if (it == m_localByHandle.end()) return;
    if (it->second.id != 0 && m_sendOp)
    {
        net::WorldStateOp op;
        op.key.object = it->second.id;
        op.key.channel = net::WS_CHANNEL_ENTITY;
        op.kind = net::WSO_ERASE;
        op.policy = net::WSP_OWNER;
        m_sendOp(op);
    }
    m_localByHandle.erase(it);
}

uint64_t NetEntityManager::LocalId(void* handle) const
{
    auto it = m_localByHandle.find(handle);
    return it == m_localByHandle.end() ? 0 : it->second.id;
}

// -------------------------------------------------------------- inbound

void NetEntityManager::OnAuthoritative(const net::WorldStateEntry& e, net::WorldStateReason reason)
{
    if (e.key.channel != net::WS_CHANNEL_ENTITY || !net::IsDynamicEntityId(e.key.object)) return;

    if (reason == net::WSR_ERASED)
    {
        DespawnRemote(e.key.object);
        return;
    }
    if (reason == net::WSR_REJECT)
    {
        // Our announcement was refused (id clash / table full): stop streaming
        // it; the car stays purely local.
        for (auto& kv : m_localByHandle)
            if (kv.second.id == e.key.object) kv.second.id = 0;
        return;
    }
    if (m_welcomed && e.owner == m_self) return; // our own entity (confirmation)
    SpawnRemote(e);
}

void NetEntityManager::SpawnRemote(const net::WorldStateEntry& e)
{
    if (m_remote.count(e.key.object)) return; // spawn description never changes
    net::NetEntitySpawn spawn;
    if (!spawn.Decode(e.value)) return;

    Remote r;
    r.owner = e.owner;
    r.visual.x = r.target.x = spawn.x;
    r.visual.y = r.target.y = spawn.y;
    r.visual.z = r.target.z = spawn.z;
    r.visual.yaw = r.target.yaw = spawn.yaw;
    r.handle = m_sink.spawn ? m_sink.spawn(e.key.object, spawn) : nullptr;
    m_remote[e.key.object] = r;
    if (r.handle && m_sink.move) m_sink.move(r.handle, r.visual);
}

void NetEntityManager::DespawnRemote(uint64_t id)
{
    auto it = m_remote.find(id);
    if (it == m_remote.end()) return;
    if (m_sink.despawn && it->second.handle) m_sink.despawn(it->second.handle);
    m_remote.erase(it);
}

void NetEntityManager::OnEntityState(net::PeerId owner, const net::EntityStateRecord& rec)
{
    auto it = m_remote.find(rec.entity);
    if (it == m_remote.end() || it->second.owner != owner) return;
    net::NetEntityMotion m;
    if (!m.Decode(rec)) return;
    it->second.target = m;
    if (!it->second.haveMotion)
    {
        it->second.visual = m; // first sample: no slide from the spawn pose
        it->second.haveMotion = true;
    }
}

// ------------------------------------------------------------ per frame

void NetEntityManager::Update(float dt)
{
    if (dt > 0.0f)
    {
        const float alpha = 1.0f - std::exp(-(3.0f / m_smoothing) * dt);
        const float kPi = 3.14159265358979f;
        for (auto& kv : m_remote)
        {
            Remote& r = kv.second;
            r.visual.x += (r.target.x - r.visual.x) * alpha;
            r.visual.y += (r.target.y - r.visual.y) * alpha;
            r.visual.z += (r.target.z - r.visual.z) * alpha;
            float d = r.target.yaw - r.visual.yaw;            // shortest arc
            while (d > kPi)  d -= 2.0f * kPi;
            while (d < -kPi) d += 2.0f * kPi;
            r.visual.yaw += d * alpha;
            r.visual.speedKmh = r.target.speedKmh;
            r.visual.steer = r.target.steer;
            r.visual.flags = r.target.flags;
            if (r.handle && m_sink.move) m_sink.move(r.handle, r.visual);
        }
    }

    if (!m_welcomed || !m_sample || !m_sendStates) return;
    if (++m_tick < m_sendEvery) return;
    m_tick = 0;

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

// --------------------------------------------------------------- queries

int NetEntityManager::CountRemoteNear(float x, float z, float radius) const
{
    int n = 0;
    float r2 = radius * radius;
    for (const auto& kv : m_remote)
    {
        float dx = kv.second.visual.x - x, dz = kv.second.visual.z - z;
        if (dx * dx + dz * dz <= r2) ++n;
    }
    return n;
}

bool NetEntityManager::IsRemoteHandle(void* handle) const
{
    if (handle == nullptr) return false;
    for (const auto& kv : m_remote)
        if (kv.second.handle == handle) return true;
    return false;
}
