#pragma once

#include "../netengine/WorldInterfaces.h"
#include "../netengine/NetTime.h"
#include "PlayerNetworkState.h"

#include <functional>
#include <map>

// Server-side movement plausibility for the player state stream (the
// net::IStateValidator the game installs on its server — the hosting game and
// the dedicated server alike).
//
// Every inbound PlayerNetworkState is compared with the peer's last ACCEPTED
// state, using the server's own receive times (a client cannot forge dt):
//   - horizontal distance <= maxSpeed * dt + slack, where maxSpeed is the
//     on-foot or in-vehicle limit (vehicle if either state is in a machine);
//   - climbing <= maxSpeed * dt + slack, falling <= maxFallSpeed * dt + slack;
//   - non-finite or out-of-world coordinates are refused.
// A jump beyond the limits passes only with a new teleportSeq (the sender's
// deliberate respawn / interior teleport), at most teleportBurst at once and
// teleportsPerSecond on average. Otherwise the state is refused and the
// client is sent its last accepted state to snap back to (at most once per
// correctionCooldownMs; states in between are just dropped).
// An optional extra check (a server Lua mod's on_validate) runs last.
class PlayerMovementValidator : public net::IStateValidator
{
public:
    struct Limits
    {
        float    maxFootSpeed       = 20.0f;   // m/s (run + dash + slopes)
        float    maxVehicleSpeed    = 110.0f;  // m/s (~400 km/h, boosts / ramps)
        float    maxFallSpeed       = 80.0f;   // m/s downwards
        float    slackMeters        = 4.0f;    // jitter / interpolation slack
        float    worldExtent        = 100000.0f;
        float    teleportsPerSecond = 0.5f;
        float    teleportBurst      = 3.0f;
        uint32_t correctionCooldownMs = 1000;
    };

    // Extra check after the movement rules (e.g. ServerModManager::OnPlayerValidate).
    using ExtraCheckFn = std::function<bool(net::PeerId, const PlayerNetworkState&, float measuredSpeed)>;

    void SetLimits(const Limits& l) { m_limits = l; }
    const Limits& GetLimits() const { return m_limits; }
    void SetExtraCheck(ExtraCheckFn fn) { m_extra = std::move(fn); }

    net::StateVerdict ValidatePeerState(net::PeerId from, const uint8_t* data, uint16_t size,
                                        uint32_t serverNowMs, std::vector<uint8_t>& correction) override;
    void OnPeerLeft(net::PeerId peer) override { m_peers.erase(peer); }

    // Observability (tests / server console).
    uint32_t Accepted() const { return m_accepted; }
    uint32_t Refused() const { return m_refused; }

private:
    struct Track
    {
        PlayerNetworkState last;
        uint32_t           lastTime = 0;
        uint32_t           lastCorrection = 0;
        bool               corrected = false;
        net::TokenBucket   teleports;
    };

    net::StateVerdict Refuse(Track& t, uint32_t now, std::vector<uint8_t>& correction);

    Limits m_limits;
    ExtraCheckFn m_extra;
    std::map<net::PeerId, Track> m_peers;
    uint32_t m_accepted = 0;
    uint32_t m_refused = 0;
};
