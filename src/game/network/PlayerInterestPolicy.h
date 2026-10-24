#pragma once

#include "../netengine/WorldInterfaces.h"
#include "../netengine/WorldState.h"

#include <cstdint>
#include <map>
#include <utility>

// PlayerInterestPolicy
// --------------------
// The game's interest management on the server (net::IInterestPolicy): who
// receives which player and entity stream samples. Server-authoritative — a
// client is never sent what it has no business seeing.
//
//   players : a sender in another context (level / interior, PlayerPresence
//             WOC_PLAYER_CONTEXT) is not relevant; within nearRadius every
//             sample is forwarded, beyond it one per farIntervalMs (the
//             receiver interpolates with an adaptive delay). The tier has
//             hysteresis so a pair at the boundary does not flap.
//   entities: an entity record goes to receivers in its owner's context
//             within entityRadius of it (wider than the client's ghost hide
//             radius, so a shown ghost never runs out of samples).
//
// Unknown facts are relevant: a peer without a context record or without an
// accepted state yet gets everything (fail open for visibility).
class PlayerInterestPolicy : public net::IInterestPolicy
{
public:
    struct Config
    {
        bool     enabled = true;
        float    nearRadius = 150.0f;      // m: every sample within
        float    nearHysteresis = 25.0f;   // m: a near pair turns far beyond nearRadius + this
        uint32_t farIntervalMs = 200;      // 5 Hz beyond
        float    entityRadius = 300.0f;    // m: entity records within (client hides ghosts at 250)
    };

    void SetConfig(const Config& c) { m_config = c; }
    const Config& GetConfig() const { return m_config; }

    // Where player contexts are read from (the server's world-state table).
    void SetWorld(const net::WorldStateTable* world) { m_world = world; }

    // ---- net::IInterestPolicy ------------------------------------------
    void     OnPlayerState(net::PeerId peer, const uint8_t* data, uint16_t size, uint32_t serverNowMs) override;
    uint32_t PlayerStateIntervalMs(net::PeerId receiver, net::PeerId sender) override;
    bool     EntityRelevant(net::PeerId receiver, net::PeerId owner, const net::EntityStateRecord& rec) override;
    void     OnPeerLeft(net::PeerId peer) override;

    void Clear();

private:
    struct Position { float x = 0.0f, z = 0.0f; };

    bool SameContext(net::PeerId a, net::PeerId b) const;
    const Position* PositionOf(net::PeerId peer) const;

    Config                     m_config;
    const net::WorldStateTable* m_world = nullptr;
    std::map<net::PeerId, Position> m_positions;
    std::map<std::pair<net::PeerId, net::PeerId>, bool> m_near; // (receiver, sender) -> near tier
};
