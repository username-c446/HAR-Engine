#pragma once

#include "WorldObjectId.h"
#include "../netengine/WorldState.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <vector>

// NetWorldRegistry
// ----------------
// Binds live static world objects to their deterministic network ids and to
// the server-authoritative world state (net::WorldState). Game-agnostic: an
// object registers with an opaque handle and an apply callback.
//
//   * Register(kind, source, name, handle, apply)  -> id (ordinal in load order).
//     Any state the server already holds for the id (late join, a sector that
//     loads after the change happened) is applied — deferred to Pump() so it
//     never runs inside a loader or a destructor.
//   * Request(handle, channel, policy, value)  — a LOCAL change the game has
//     already applied (prediction). Ignored while a remote change is being
//     applied, so mirrored changes are never re-sent.
//   * OnAuthoritative(entry, reason)  — every server decision (remote change,
//     confirmation, rejection, snapshot, erase); applied to the live object in
//     Pump(), idempotently. A rejected prediction is rolled back this way.
//
// Game channels for static objects (values are tiny):
namespace net
{
    enum WorldObjectChannel : uint8_t
    {
        WOC_STATE     = 1, // u8 stateprop state           (WSP_LAST_WRITE)
        WOC_BROKEN    = 2, // u8 1 = broken / removed       (WSP_FIRST_WRITE)
        WOC_ANIM_DIR  = 3, // i8 animation direction -1/0/1 (WSP_LAST_WRITE)
        WOC_TAKEN     = 4, // u16 claimant peer id: a player got into the parked car (WSP_FIRST_WRITE)
        WOC_POPULATION= 5, // zone parked-car placement plan (WSP_FIRST_WRITE)
        WOC_POSE      = 6, // ObjectPose: where a pushed parked car came to rest (WSP_LAST_WRITE)
    };
}

class NetWorldRegistry
{
public:
    // Applies an authoritative value to a live object. `erased` = the entry was
    // removed (value empty).
    typedef void (*ApplyFn)(void* handle, uint8_t channel, const net::WorldStateValue& value, bool erased);
    // Ships a request to the server (net::Session::RequestWorldState).
    typedef std::function<bool(const net::WorldStateOp&)> SendFn;

    void SetSender(SendFn fn) { m_send = std::move(fn); }

    uint64_t Register(uint8_t kind, uint64_t sourceUid, uint64_t nameUid, void* handle, ApplyFn apply);
    void     Unregister(void* handle);

    uint64_t IdOf(void* handle) const;
    void*    HandleOf(uint64_t id) const;
    // Authoritative value already known for (id, channel), loaded or not.
    bool     FindKnown(uint64_t id, uint8_t channel, net::WorldStateValue& out) const;
    size_t   Size() const { return m_byId.size(); }

    // Local change (already applied by the caller). False if unregistered,
    // while applying a remote change, or without a sender (no match).
    bool Request(void* handle, uint8_t channel, uint8_t policy, const uint8_t* value, uint8_t len);

    void OnAuthoritative(const net::WorldStateEntry& e, net::WorldStateReason reason);

    // Applies queued authoritative values to live objects. Call once a frame.
    void Pump();

    // True while Pump() applies a value (game hooks must not Request then).
    bool IsApplying() const { return m_applying > 0; }

    // Match ended: forget the authoritative state (objects stay registered).
    void ClearState() { m_known.clear(); m_queue.clear(); }
    void Clear() { ClearState(); m_byId.clear(); m_byHandle.clear(); m_ordinals.clear(); }

private:
    struct Binding
    {
        void*    handle;
        ApplyFn  apply;
        uint8_t  kind;
        uint64_t source;
        uint64_t name;
        uint32_t ordinal;
    };
    struct Pending
    {
        uint64_t                id;
        uint8_t                 channel;
        net::WorldStateValue    value;
        bool                    erased;
    };
    typedef std::pair<uint8_t, std::pair<uint64_t, uint64_t> > NameKey;

    void Enqueue(uint64_t id, uint8_t channel, const net::WorldStateValue& v, bool erased);

    SendFn m_send;
    std::map<uint64_t, Binding>  m_byId;
    std::map<void*, uint64_t>    m_byHandle;
    // Ordinals in use per (kind, source, name). A new object takes the lowest
    // free one, so a source that unloads and reloads gets the same ids again.
    std::map<NameKey, std::set<uint32_t> > m_ordinals;
    // Authoritative values known for objects (loaded or not): id -> channel -> value.
    std::map<uint64_t, std::map<uint8_t, net::WorldStateValue> > m_known;
    std::vector<Pending> m_queue;
    int m_applying = 0;
};
