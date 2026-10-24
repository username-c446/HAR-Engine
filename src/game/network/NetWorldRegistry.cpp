#include "NetWorldRegistry.h"

uint64_t NetWorldRegistry::Register(uint8_t kind, uint64_t sourceUid, uint64_t nameUid, void* handle, ApplyFn apply)
{
    if (handle == nullptr) return 0;
    if (m_byHandle.count(handle)) return m_byHandle[handle];

    NameKey nk(kind, std::make_pair(sourceUid, nameUid));
    std::set<uint32_t>& used = m_ordinals[nk];
    uint32_t ordinal = 0;
    while (used.count(ordinal)) ++ordinal;

    uint64_t id = net::MakeWorldObjectId(kind, sourceUid, nameUid, ordinal);
    if (m_byId.count(id))
    {
        // A 64-bit collision between different (kind, source, name, ordinal)
        // tuples: refuse to network the newcomer rather than alias two objects.
        return 0;
    }
    used.insert(ordinal);

    Binding b;
    b.handle = handle;
    b.apply = apply;
    b.kind = kind;
    b.source = sourceUid;
    b.name = nameUid;
    b.ordinal = ordinal;
    m_byId[id] = b;
    m_byHandle[handle] = id;

    // State the server already decided (late join / sector loaded later).
    auto known = m_known.find(id);
    if (known != m_known.end())
        for (const auto& ch : known->second)
            Enqueue(id, ch.first, ch.second, false);
    return id;
}

void NetWorldRegistry::Unregister(void* handle)
{
    auto h = m_byHandle.find(handle);
    if (h == m_byHandle.end()) return;
    auto b = m_byId.find(h->second);
    if (b != m_byId.end())
    {
        NameKey nk(b->second.kind, std::make_pair(b->second.source, b->second.name));
        auto o = m_ordinals.find(nk);
        if (o != m_ordinals.end())
        {
            o->second.erase(b->second.ordinal);
            if (o->second.empty()) m_ordinals.erase(o);
        }
        m_byId.erase(b);
    }
    m_byHandle.erase(h);
}

uint64_t NetWorldRegistry::IdOf(void* handle) const
{
    auto h = m_byHandle.find(handle);
    return h == m_byHandle.end() ? 0 : h->second;
}

void* NetWorldRegistry::HandleOf(uint64_t id) const
{
    auto b = m_byId.find(id);
    return b == m_byId.end() ? nullptr : b->second.handle;
}

bool NetWorldRegistry::FindKnown(uint64_t id, uint8_t channel, net::WorldStateValue& out) const
{
    auto k = m_known.find(id);
    if (k == m_known.end()) return false;
    auto c = k->second.find(channel);
    if (c == k->second.end()) return false;
    out = c->second;
    return true;
}

bool NetWorldRegistry::Request(void* handle, uint8_t channel, uint8_t policy, const uint8_t* value, uint8_t len)
{
    if (m_applying > 0 || !m_send) return false;
    uint64_t id = IdOf(handle);
    if (id == 0) return false;

    net::WorldStateOp op;
    op.key.object = id;
    op.key.channel = channel;
    op.kind = net::WSO_SET;
    op.policy = policy;
    if (!op.value.Set(value, len)) return false;
    // Our own prediction is the best local knowledge until the server answers.
    m_known[id][channel] = op.value;
    return m_send(op);
}

void NetWorldRegistry::OnAuthoritative(const net::WorldStateEntry& e, net::WorldStateReason reason)
{
    if (net::IsDynamicEntityId(e.key.object)) return; // entities are NetEntityManager's

    if (reason == net::WSR_ERASED)
    {
        auto k = m_known.find(e.key.object);
        if (k != m_known.end())
        {
            k->second.erase(e.key.channel);
            if (k->second.empty()) m_known.erase(k);
        }
        Enqueue(e.key.object, e.key.channel, net::WorldStateValue(), true);
        return;
    }

    m_known[e.key.object][e.key.channel] = e.value;
    // Re-applied even for a confirmation: a remote change ordered before ours
    // may have been applied in between, and applies are idempotent.
    Enqueue(e.key.object, e.key.channel, e.value, false);
}

void NetWorldRegistry::Enqueue(uint64_t id, uint8_t channel, const net::WorldStateValue& v, bool erased)
{
    Pending p;
    p.id = id;
    p.channel = channel;
    p.value = v;
    p.erased = erased;
    m_queue.push_back(p);
}

void NetWorldRegistry::Pump()
{
    if (m_queue.empty()) return;
    std::vector<Pending> work;
    work.swap(m_queue);
    ++m_applying;
    for (const Pending& p : work)
    {
        // Looked up now: the object may have unloaded since it was queued.
        auto b = m_byId.find(p.id);
        if (b == m_byId.end() || b->second.apply == nullptr) continue;
        b->second.apply(b->second.handle, p.channel, p.value, p.erased);
    }
    --m_applying;
}
