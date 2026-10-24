#include "WorldState.h"

namespace net
{
    // ---------------------------------------------------------------- table

    WorldStateTable::Result WorldStateTable::Apply(PeerId from, const WorldStateOp& op,
                                                   WorldStateEntry& out, bool& existed)
    {
        auto it = m_entries.find(op.key);
        if (it == m_entries.end())
        {
            existed = false;
            if (op.kind == WSO_ERASE) return UNCHANGED;          // nothing to erase
            if (op.policy >= WSP_COUNT) return REJECTED;          // malformed policy
            if (op.value.len > WS_MAX_VALUE) return REJECTED;
            if (m_entries.size() >= WS_MAX_ENTRIES) return REJECTED;

            WorldStateEntry e;
            e.key     = op.key;
            e.value   = op.value;
            e.version = 1;
            e.owner   = from;
            e.policy  = op.policy;
            e.flags   = op.flags;
            m_entries[op.key] = e;
            out = e;
            existed = true;
            return ACCEPTED;
        }

        // The policy is fixed by the entry's creator; a later request cannot
        // downgrade it (e.g. turn a first-write "broken" into last-write).
        WorldStateEntry& e = it->second;
        existed = true;
        out = e;

        switch (e.policy)
        {
        case WSP_FIRST_WRITE:
            if (op.kind == WSO_SET && op.value == e.value) return UNCHANGED;
            return REJECTED;

        case WSP_OWNER:
            if (from != e.owner) return REJECTED;
            break;

        default: // WSP_LAST_WRITE
            break;
        }

        if (op.kind == WSO_ERASE)
        {
            m_entries.erase(it);
            existed = false;
            return ACCEPTED;
        }
        if (op.value == e.value) return UNCHANGED;
        e.value = op.value;
        ++e.version;
        out = e;
        return ACCEPTED;
    }

    void WorldStateTable::OnPeerLeft(PeerId peer, std::vector<WorldStateKey>& erased)
    {
        for (auto it = m_entries.begin(); it != m_entries.end(); )
        {
            WorldStateEntry& e = it->second;
            if (e.policy == WSP_OWNER && e.owner == peer)
            {
                if (e.flags & WSF_ERASE_ON_OWNER_LEAVE)
                {
                    erased.push_back(e.key);
                    it = m_entries.erase(it);
                    continue;
                }
                e.owner = 0; // orphaned: read-only until reassigned
            }
            ++it;
        }
    }

    bool WorldStateTable::Find(const WorldStateKey& key, WorldStateEntry& out) const
    {
        auto it = m_entries.find(key);
        if (it == m_entries.end()) return false;
        out = it->second;
        return true;
    }

    // --------------------------------------------------------------- mirror

    uint32_t WorldStateMirror::Predict(const WorldStateKey& key)
    {
        uint32_t seq = m_nextSeq++;
        if (m_nextSeq == 0) m_nextSeq = 1; // 0 is reserved for "server-originated"
        m_pending[key] = seq;
        return seq;
    }

    WorldStateReason WorldStateMirror::OnUpdate(const WorldStateEntry& e, PeerId origin,
                                                uint32_t seq, PeerId self)
    {
        m_confirmed[e.key] = e;
        if (origin == self && seq != 0)
        {
            auto p = m_pending.find(e.key);
            if (p != m_pending.end() && p->second == seq) m_pending.erase(p);
            return WSR_CONFIRM;
        }
        return WSR_REMOTE;
    }

    WorldStateReason WorldStateMirror::OnReject(const WorldStateKey& key, bool exists,
                                                const WorldStateEntry& e, uint32_t seq)
    {
        if (exists) m_confirmed[key] = e;
        else        m_confirmed.erase(key);
        auto p = m_pending.find(key);
        if (p != m_pending.end() && p->second == seq) m_pending.erase(p);
        return WSR_REJECT;
    }

    void WorldStateMirror::OnErase(const WorldStateKey& key)
    {
        m_confirmed.erase(key);
        m_pending.erase(key);
    }

    bool WorldStateMirror::Find(const WorldStateKey& key, WorldStateEntry& out) const
    {
        auto it = m_confirmed.find(key);
        if (it == m_confirmed.end()) return false;
        out = it->second;
        return true;
    }

    // ---------------------------------------------------------------- codec

    namespace
    {
        bool WriteU64(Writer& w, uint64_t v)
        {
            return w.WriteU32(static_cast<uint32_t>(v & 0xFFFFFFFFull))
                && w.WriteU32(static_cast<uint32_t>(v >> 32));
        }
        bool ReadU64(Reader& r, uint64_t& v)
        {
            uint32_t lo = 0, hi = 0;
            if (!r.ReadU32(lo) || !r.ReadU32(hi)) return false;
            v = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
            return true;
        }
        bool WriteKey(Writer& w, const WorldStateKey& k)
        {
            return WriteU64(w, k.object) && w.WriteU8(k.channel);
        }
        bool ReadKey(Reader& r, WorldStateKey& k)
        {
            return ReadU64(r, k.object) && r.ReadU8(k.channel);
        }
        bool ReadValue(Reader& r, WorldStateValue& v)
        {
            uint8_t len = 0;
            if (!r.ReadU8(len) || len > WS_MAX_VALUE) return false;
            v.len = len;
            return len == 0 || r.ReadBytes(v.bytes, len);
        }
    }

    namespace WorldStateCodec
    {
        uint16_t EncodeRequest(uint8_t* out, uint16_t cap, uint32_t seq, const WorldStateOp& op)
        {
            Writer w(out, cap);
            bool ok = w.WriteU32(seq) && WriteKey(w, op.key) && w.WriteU8(op.kind)
                   && w.WriteU8(op.policy) && w.WriteU8(op.flags) && w.WriteU8(op.value.len)
                   && (op.value.len == 0 || w.WriteBytes(op.value.bytes, op.value.len));
            return (ok && w.Ok()) ? w.Position() : 0;
        }

        bool DecodeRequest(const uint8_t* d, uint16_t n, uint32_t& seq, WorldStateOp& op)
        {
            Reader r(d, n);
            return r.ReadU32(seq) && ReadKey(r, op.key) && r.ReadU8(op.kind) && op.kind <= WSO_ERASE
                && r.ReadU8(op.policy) && r.ReadU8(op.flags) && ReadValue(r, op.value);
        }

        bool WriteEntry(Writer& w, const WorldStateEntry& e)
        {
            return WriteKey(w, e.key) && w.WriteU32(e.version) && w.WriteU16(e.owner)
                && w.WriteU8(e.policy) && w.WriteU8(e.flags) && w.WriteU8(e.value.len)
                && (e.value.len == 0 || w.WriteBytes(e.value.bytes, e.value.len));
        }

        bool ReadEntry(Reader& r, WorldStateEntry& e)
        {
            uint16_t owner = 0;
            if (!ReadKey(r, e.key) || !r.ReadU32(e.version) || !r.ReadU16(owner)
                || !r.ReadU8(e.policy) || !r.ReadU8(e.flags))
                return false;
            e.owner = owner;
            return ReadValue(r, e.value);
        }

        uint16_t EncodeUpdate(uint8_t* out, uint16_t cap, PeerId origin, uint32_t seq, const WorldStateEntry& e)
        {
            Writer w(out, cap);
            bool ok = w.WriteU16(origin) && w.WriteU32(seq) && WriteEntry(w, e);
            return (ok && w.Ok()) ? w.Position() : 0;
        }

        bool DecodeUpdate(const uint8_t* d, uint16_t n, PeerId& origin, uint32_t& seq, WorldStateEntry& e)
        {
            Reader r(d, n);
            uint16_t o = 0;
            if (!r.ReadU16(o) || !r.ReadU32(seq)) return false;
            origin = o;
            return ReadEntry(r, e);
        }

        uint16_t EncodeReject(uint8_t* out, uint16_t cap, uint32_t seq, const WorldStateKey& key,
                              bool exists, const WorldStateEntry& e)
        {
            Writer w(out, cap);
            bool ok = w.WriteU32(seq) && WriteKey(w, key) && w.WriteU8(exists ? 1 : 0)
                   && (!exists || WriteEntry(w, e));
            return (ok && w.Ok()) ? w.Position() : 0;
        }

        bool DecodeReject(const uint8_t* d, uint16_t n, uint32_t& seq, WorldStateKey& key,
                          bool& exists, WorldStateEntry& e)
        {
            Reader r(d, n);
            uint8_t ex = 0;
            if (!r.ReadU32(seq) || !ReadKey(r, key) || !r.ReadU8(ex)) return false;
            exists = ex != 0;
            return !exists || ReadEntry(r, e);
        }

        uint16_t EncodeErase(uint8_t* out, uint16_t cap, PeerId origin, uint32_t seq, const WorldStateKey& key)
        {
            Writer w(out, cap);
            bool ok = w.WriteU16(origin) && w.WriteU32(seq) && WriteKey(w, key);
            return (ok && w.Ok()) ? w.Position() : 0;
        }

        bool DecodeErase(const uint8_t* d, uint16_t n, PeerId& origin, uint32_t& seq, WorldStateKey& key)
        {
            Reader r(d, n);
            uint16_t o = 0;
            if (!r.ReadU16(o) || !r.ReadU32(seq)) return false;
            origin = o;
            return ReadKey(r, key);
        }

        void EncodeSnapshot(const std::vector<WorldStateEntry>& entries, uint16_t cap,
                            std::vector<std::vector<uint8_t>>& chunks)
        {
            size_t i = 0;
            while (i < entries.size())
            {
                std::vector<uint8_t> buf(cap, 0);
                Writer w(buf.data(), cap);
                w.WriteU16(0); // count, patched below
                uint16_t count = 0;
                while (i < entries.size())
                {
                    uint16_t before = w.Position();
                    uint16_t need = ENTRY_FIXED + entries[i].value.len;
                    if (before + need > cap) break;
                    WriteEntry(w, entries[i]);
                    ++count;
                    ++i;
                }
                if (count == 0) { ++i; continue; } // cannot happen: an entry always fits
                buf[0] = static_cast<uint8_t>(count & 0xFF);
                buf[1] = static_cast<uint8_t>(count >> 8);
                buf.resize(w.Position());
                chunks.push_back(buf);
            }
        }

        bool DecodeSnapshot(const uint8_t* d, uint16_t n, std::vector<WorldStateEntry>& out)
        {
            Reader r(d, n);
            uint16_t count = 0;
            if (!r.ReadU16(count)) return false;
            for (uint16_t k = 0; k < count; ++k)
            {
                WorldStateEntry e;
                if (!ReadEntry(r, e)) return false;
                out.push_back(e);
            }
            return true;
        }
    }

    namespace EntityStreamCodec
    {
        uint16_t Encode(uint8_t* out, uint16_t cap, const std::vector<EntityStateRecord>& records, size_t& first)
        {
            if (cap < 2) return 0;
            Writer w(out, cap);
            w.WriteU16(0); // count, patched below
            uint16_t count = 0;
            while (first < records.size())
            {
                const EntityStateRecord& rec = records[first];
                if (rec.len > ENTITY_STATE_MAX) { ++first; continue; } // malformed: skip
                if (w.Position() + RECORD_FIXED + rec.len > cap) break;
                WriteU64(w, rec.entity);
                w.WriteU8(rec.len);
                if (rec.len) w.WriteBytes(rec.bytes, rec.len);
                ++count;
                ++first;
            }
            out[0] = static_cast<uint8_t>(count & 0xFF);
            out[1] = static_cast<uint8_t>(count >> 8);
            return count ? w.Position() : 0;
        }

        bool Decode(const uint8_t* d, uint16_t n, std::vector<EntityStateRecord>& out)
        {
            Reader r(d, n);
            uint16_t count = 0;
            if (!r.ReadU16(count)) return false;
            for (uint16_t k = 0; k < count; ++k)
            {
                EntityStateRecord rec;
                if (!ReadU64(r, rec.entity) || !r.ReadU8(rec.len) || rec.len > ENTITY_STATE_MAX)
                    return false;
                if (rec.len && !r.ReadBytes(rec.bytes, rec.len)) return false;
                out.push_back(rec);
            }
            return true;
        }
    }
}
