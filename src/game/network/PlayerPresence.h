#pragma once

#include "WorldObjectId.h"
#include "../netengine/WorldState.h"

#include <cstdint>
#include <functional>
#include <map>

// PlayerPresence
// --------------
// Slow-changing facts about each player, replicated as server-owned world
// state records (reliable, in the late-join snapshot, erased when the player
// leaves) instead of riding the 20 Hz movement stream:
//
//   WOC_PLAYER_CONTEXT : where the player is — level and zone (interior uid,
//                        0 = outside). Players are only shown to players in
//                        the same context.
//   WOC_APPEARANCE     : how the player looks — character model (skin
//                        included) and animation bank. Any change (costume
//                        purchase, mission, cheat, a future outfit system)
//                        replicates and remote puppets are restyled.
//   WOC_VEHICLE        : the machine the player drives — its network id and
//                        model name (the stream carries only the id; the
//                        receiver takes the model from here).
//
// Object id: the player's own dynamic id (MakePlayerObjectId), WSP_OWNER +
// WSF_ERASE_ON_OWNER_LEAVE — only the player writes its records.
namespace net
{
    enum PresenceChannel : uint8_t
    {
        WOC_PLAYER_CONTEXT = 6,
        WOC_APPEARANCE     = 7,
        WOC_VEHICLE        = 8,
    };

    // Counter 0 of a peer's dynamic range is reserved for the player itself
    // (entities count from 1).
    inline uint64_t MakePlayerObjectId(PeerId peer) { return MakeDynamicEntityId(peer, 0); }
    inline bool IsPlayerObjectId(uint64_t id) { return IsDynamicEntityId(id) && (id & 0xFFFFFFFFu) == 0; }
    inline PeerId PlayerOfObjectId(uint64_t id) { return static_cast<PeerId>((id >> 32) & 0x7FFFFFFFu); }

    // u8 level | u64 zone  (9 bytes)
    struct PlayerContext
    {
        uint8_t  level = 0xFF;  // 0xFF = unknown (not in a level yet)
        uint64_t zone  = 0;     // interior uid, 0 = outside

        bool operator==(const PlayerContext& o) const { return level == o.level && zone == o.zone; }
        bool operator!=(const PlayerContext& o) const { return !(*this == o); }
        bool Encode(WorldStateValue& out) const;
        bool Decode(const WorldStateValue& in);
    };

    // char model[32] | char anim[16]  (48 bytes, NUL-terminated)
    struct PlayerAppearance
    {
        char model[32] = { 0 };
        char anim[16]  = { 0 };

        bool operator==(const PlayerAppearance& o) const;
        bool operator!=(const PlayerAppearance& o) const { return !(*this == o); }
        bool Set(const char* modelName, const char* animBank);
        bool Encode(WorldStateValue& out) const;
        bool Decode(const WorldStateValue& in);
    };

    // i32 netVehicleId | char model[16]  (20 bytes; empty model = on foot)
    struct PlayerVehicle
    {
        int32_t netVehicleId = 0;
        char    model[16] = { 0 };

        bool operator==(const PlayerVehicle& o) const;
        bool operator!=(const PlayerVehicle& o) const { return !(*this == o); }
        void Set(int32_t id, const char* modelName);
        bool Encode(WorldStateValue& out) const;
        bool Decode(const WorldStateValue& in);
    };
}

class PlayerPresence
{
public:
    typedef std::function<bool(const net::WorldStateOp&)> SendFn;

    void SetSender(SendFn fn) { m_send = std::move(fn); }

    // Match lifecycle: OnWelcomed publishes what is already known locally.
    void OnWelcomed(net::PeerId self);
    void OnMatchEnd();

    // Local facts; published when they change (and on welcome).
    void SetLocalContext(const net::PlayerContext& ctx);
    void SetLocalAppearance(const net::PlayerAppearance& app);
    void SetLocalVehicle(const net::PlayerVehicle& vehicle);
    const net::PlayerContext& LocalContext() const { return m_localContext; }

    // Server decisions (the session's world-state sink).
    void OnAuthoritative(const net::WorldStateEntry& e, net::WorldStateReason reason);

    // Remote facts (null / false if not known yet).
    const net::PlayerContext*    Context(net::PeerId peer) const;
    const net::PlayerAppearance* Appearance(net::PeerId peer) const;
    const net::PlayerVehicle*    Vehicle(net::PeerId peer) const;
    // Bumps on every appearance change of `peer` (0 = none known).
    uint32_t AppearanceVersion(net::PeerId peer) const;
    // A remote player is shown only in our context. Unknown context = shown
    // (an older peer, or the record has not arrived yet).
    bool SharesContext(net::PeerId peer) const;

private:
    struct Remote
    {
        bool                  hasContext = false;
        net::PlayerContext    context;
        bool                  hasAppearance = false;
        net::PlayerAppearance appearance;
        uint32_t              appearanceVersion = 0;
        bool                  hasVehicle = false;
        net::PlayerVehicle    vehicle;
    };

    void Publish(uint8_t channel);

    SendFn                m_send;
    net::PeerId           m_self = 0;
    bool                  m_welcomed = false;
    net::PlayerContext    m_localContext;
    bool                  m_hasLocalAppearance = false;
    net::PlayerAppearance m_localAppearance;
    bool                  m_hasLocalVehicle = false;
    net::PlayerVehicle    m_localVehicle;
    std::map<net::PeerId, Remote> m_remote;
};
