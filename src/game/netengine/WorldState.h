#pragma once

#include "Packet.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <vector>
#include <functional>

// net::WorldState
// ---------------
// Server-authoritative replicated world state. The engine layer knows nothing
// about props, doors or vehicles: the game names every shared object with a
// deterministic 64-bit id (identical on every client, see WorldObjectId.h) and
// a small channel number, and stores a short opaque value per (object, channel).
//
// Flow (prediction + arbitration):
//   client  : applies a change locally, sends PID_WS_REQUEST {seq, key, op}
//   server  : WorldStateTable::Apply enforces the write policy, then
//             - accepted  -> PID_WS_UPDATE (or PID_WS_ERASE) to EVERY peer,
//                            the requester matches it by seq (confirmation)
//             - rejected  -> PID_WS_REJECT to the requester with the current
//                            authoritative entry, so it rolls its prediction back
//   joiner  : after WELCOME the server streams PID_WS_SNAPSHOT chunks + END;
//             objects that are not loaded yet pick their state up from the
//             client's WorldStateMirror when they register.
//
// Owner-policy entries flagged WSF_ERASE_ON_OWNER_LEAVE (dynamic entities such
// as traffic) are erased server-side when their owner disconnects.
namespace net
{
    // Write policy, enforced by the server per entry.
    enum WorldStatePolicy : uint8_t
    {
        WSP_LAST_WRITE  = 0, // any peer may overwrite (doors, platforms)
        WSP_FIRST_WRITE = 1, // first write wins forever (broken / destroyed)
        WSP_OWNER       = 2, // creator owns the entry; only the owner writes/erases
        WSP_COUNT
    };

    enum WorldStateFlags : uint8_t
    {
        WSF_NONE                 = 0x00,
        WSF_ERASE_ON_OWNER_LEAVE = 0x01, // owner disconnect erases the entry
    };

    enum WorldStateOpKind : uint8_t
    {
        WSO_SET   = 0,
        WSO_ERASE = 1,
    };

    static constexpr const uint8_t  WS_MAX_VALUE   = 48;     // bytes per entry value
    static constexpr const uint32_t WS_MAX_ENTRIES = 65536;  // hard cap per match

    // Engine-reserved channel: the EXISTENCE record of a dynamic, owner-simulated
    // entity (traffic car, ...). Created with WSP_OWNER (+ usually
    // WSF_ERASE_ON_OWNER_LEAVE); its value is the game's spawn description. Only
    // the owner of this record may stream PID_ENTITY_STATE for the entity.
    // Game channels use 0x00..0xFE.
    static constexpr const uint8_t  WS_CHANNEL_ENTITY = 0xFF;

    // Per-record cap for the owner-streamed entity state (PID_ENTITY_STATE).
    static constexpr const uint8_t  ENTITY_STATE_MAX = 40;

    struct WorldStateKey
    {
        uint64_t object  = 0;
        uint8_t  channel = 0;

        bool operator<(const WorldStateKey& o) const
        {
            return object < o.object || (object == o.object && channel < o.channel);
        }
        bool operator==(const WorldStateKey& o) const
        {
            return object == o.object && channel == o.channel;
        }
    };

    struct WorldStateValue
    {
        uint8_t len = 0;
        uint8_t bytes[WS_MAX_VALUE] = { 0 };

        bool Set(const uint8_t* data, uint8_t n)
        {
            if (n > WS_MAX_VALUE) return false;
            len = n;
            if (n) std::memcpy(bytes, data, n);
            return true;
        }
        bool operator==(const WorldStateValue& o) const
        {
            return len == o.len && (len == 0 || std::memcmp(bytes, o.bytes, len) == 0);
        }
        bool operator!=(const WorldStateValue& o) const { return !(*this == o); }
    };

    struct WorldStateEntry
    {
        WorldStateKey   key;
        WorldStateValue value;
        uint32_t        version = 0;   // bumps on every accepted change of this key
        PeerId          owner   = 0;   // creator (meaningful for WSP_OWNER)
        uint8_t         policy  = WSP_LAST_WRITE;
        uint8_t         flags   = WSF_NONE;
    };

    struct WorldStateOp
    {
        WorldStateKey   key;
        uint8_t         kind   = WSO_SET;
        uint8_t         policy = WSP_LAST_WRITE;
        uint8_t         flags  = WSF_NONE;
        WorldStateValue value;
    };

    // ---------------------------------------------------------------------
    // Server side: the authoritative table.
    // ---------------------------------------------------------------------
    class WorldStateTable
    {
    public:
        enum Result
        {
            ACCEPTED,   // changed: broadcast to every peer
            UNCHANGED,  // valid but identical: confirm to the requester only
            REJECTED,   // policy refused: send REJECT with the current entry
        };

        // Applies `op` from peer `from`. `out` receives the resulting (or, on
        // rejection, the current) entry; `existed` tells whether an entry exists
        // after the call (false for an erase or a rejected write to nothing).
        Result Apply(PeerId from, const WorldStateOp& op, WorldStateEntry& out, bool& existed);

        // Owner left: erases its WSF_ERASE_ON_OWNER_LEAVE entries (keys appended
        // to `erased`) and hands its other owned entries to nobody (owner 0, so
        // they become read-only until the server reassigns them).
        void OnPeerLeft(PeerId peer, std::vector<WorldStateKey>& erased);

        bool Find(const WorldStateKey& key, WorldStateEntry& out) const;
        size_t Size() const { return m_entries.size(); }
        void Clear() { m_entries.clear(); }

        template <typename Fn>
        void ForEach(Fn fn) const
        {
            for (const auto& kv : m_entries) fn(kv.second);
        }

    private:
        std::map<WorldStateKey, WorldStateEntry> m_entries;
    };

    // ---------------------------------------------------------------------
    // Client side: the authoritative mirror + outstanding predictions.
    // ---------------------------------------------------------------------
    enum WorldStateReason : uint8_t
    {
        WSR_REMOTE   = 0, // another peer changed it
        WSR_CONFIRM  = 1, // our own prediction was accepted
        WSR_REJECT   = 2, // our own prediction was refused: roll back to `entry`
        WSR_SNAPSHOT = 3, // late-join snapshot
        WSR_ERASED   = 4, // entry removed (entry.key valid, value empty)
    };

    class WorldStateMirror
    {
    public:
        // Records a local prediction and returns the request sequence to send.
        uint32_t Predict(const WorldStateKey& key);

        // Inbound server messages. Return the reason to report to the game.
        WorldStateReason OnUpdate(const WorldStateEntry& e, PeerId origin, uint32_t seq, PeerId self);
        WorldStateReason OnReject(const WorldStateKey& key, bool exists, const WorldStateEntry& e, uint32_t seq);
        void             OnErase(const WorldStateKey& key);
        void             OnSnapshotEntry(const WorldStateEntry& e) { m_confirmed[e.key] = e; }
        void             OnSnapshotEnd() { m_synced = true; }

        bool Find(const WorldStateKey& key, WorldStateEntry& out) const;
        bool IsPending(const WorldStateKey& key) const { return m_pending.count(key) != 0; }
        bool IsSynced() const { return m_synced; }
        size_t Size() const { return m_confirmed.size(); }
        void Clear() { m_confirmed.clear(); m_pending.clear(); m_synced = false; m_nextSeq = 1; }

        template <typename Fn>
        void ForEach(Fn fn) const
        {
            for (const auto& kv : m_confirmed) fn(kv.second);
        }

    private:
        std::map<WorldStateKey, WorldStateEntry> m_confirmed;
        std::map<WorldStateKey, uint32_t>        m_pending;  // latest outstanding seq per key
        uint32_t m_nextSeq = 1;
        bool     m_synced  = false;
    };

    // ---------------------------------------------------------------------
    // Wire codec (little-endian, bounds-checked; all Decode reject truncation).
    //   REQUEST : u32 seq | u64 obj | u8 ch | u8 kind | u8 policy | u8 flags | u8 len | bytes
    //   UPDATE  : u16 origin | u32 seq | entry
    //   REJECT  : u32 seq | u64 obj | u8 ch | u8 exists | entry (if exists)
    //   ERASE   : u16 origin | u32 seq | u64 obj | u8 ch
    //   SNAPSHOT: u16 count | count * entry
    //   END     : u32 total
    //   entry   : u64 obj | u8 ch | u32 version | u16 owner | u8 policy | u8 flags | u8 len | bytes
    // ---------------------------------------------------------------------
    namespace WorldStateCodec
    {
        static constexpr const uint16_t ENTRY_FIXED = 8 + 1 + 4 + 2 + 1 + 1 + 1;
        static constexpr const uint16_t ENTRY_MAX   = ENTRY_FIXED + WS_MAX_VALUE;
        static_assert(2 + 4 + ENTRY_MAX <= MAX_PACKET_PAYLOAD, "world-state update must fit a packet");

        uint16_t EncodeRequest(uint8_t* out, uint16_t cap, uint32_t seq, const WorldStateOp& op);
        bool     DecodeRequest(const uint8_t* d, uint16_t n, uint32_t& seq, WorldStateOp& op);

        bool     WriteEntry(Writer& w, const WorldStateEntry& e);
        bool     ReadEntry(Reader& r, WorldStateEntry& e);

        uint16_t EncodeUpdate(uint8_t* out, uint16_t cap, PeerId origin, uint32_t seq, const WorldStateEntry& e);
        bool     DecodeUpdate(const uint8_t* d, uint16_t n, PeerId& origin, uint32_t& seq, WorldStateEntry& e);

        uint16_t EncodeReject(uint8_t* out, uint16_t cap, uint32_t seq, const WorldStateKey& key,
                              bool exists, const WorldStateEntry& e);
        bool     DecodeReject(const uint8_t* d, uint16_t n, uint32_t& seq, WorldStateKey& key,
                              bool& exists, WorldStateEntry& e);

        uint16_t EncodeErase(uint8_t* out, uint16_t cap, PeerId origin, uint32_t seq, const WorldStateKey& key);
        bool     DecodeErase(const uint8_t* d, uint16_t n, PeerId& origin, uint32_t& seq, WorldStateKey& key);

        // Packs as many entries as fit into snapshot chunks of <= cap bytes.
        void     EncodeSnapshot(const std::vector<WorldStateEntry>& entries, uint16_t cap,
                                std::vector<std::vector<uint8_t>>& chunks);
        bool     DecodeSnapshot(const uint8_t* d, uint16_t n, std::vector<WorldStateEntry>& out);
    }

    // Owner-streamed entity state, batched: u16 count | count * (u64 id | u8 len | bytes).
    // The server relays it framed as [u16 owner][batch] with every record the
    // sender does not own removed.
    struct EntityStateRecord
    {
        uint64_t entity = 0;
        uint8_t  len = 0;
        uint8_t  bytes[ENTITY_STATE_MAX] = { 0 };
    };

    namespace EntityStreamCodec
    {
        static constexpr const uint16_t RECORD_FIXED = 8 + 1;

        // Encodes records[first..] until the packet is full; returns bytes
        // written and advances `first` past the encoded records.
        uint16_t Encode(uint8_t* out, uint16_t cap, const std::vector<EntityStateRecord>& records, size_t& first);
        bool     Decode(const uint8_t* d, uint16_t n, std::vector<EntityStateRecord>& out);
    }
}
