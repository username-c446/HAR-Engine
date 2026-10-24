#include "PlayerMovementValidator.h"

#include <cmath>

namespace
{
    bool Finite(float v) { return std::isfinite(v); }
}

net::StateVerdict PlayerMovementValidator::Refuse(Track& t, uint32_t now, std::vector<uint8_t>& correction)
{
    ++m_refused;
    if (t.corrected && net::TimeDiff(now, t.lastCorrection) < static_cast<int32_t>(m_limits.correctionCooldownMs))
        return net::SV_DROP;
    t.corrected = true;
    t.lastCorrection = now;
    correction.resize(PlayerNetworkState::MAX_SERIALIZED_SIZE);
    correction.resize(static_cast<size_t>(t.last.Serialize(reinterpret_cast<char*>(correction.data()))));
    return net::SV_CORRECT;
}

net::StateVerdict PlayerMovementValidator::ValidatePeerState(net::PeerId from, const uint8_t* data, uint16_t size,
                                                             uint32_t serverNowMs, std::vector<uint8_t>& correction)
{
    PlayerNetworkState st;
    if (!st.Deserialize(reinterpret_cast<const char*>(data), size)) { ++m_refused; return net::SV_DROP; }

    const float ext = m_limits.worldExtent;
    bool sane = Finite(st.x) && Finite(st.y) && Finite(st.z) && Finite(st.rotY)
             && std::fabs(st.x) < ext && std::fabs(st.y) < ext && std::fabs(st.z) < ext;

    auto it = m_peers.find(from);
    if (it == m_peers.end())
    {
        // First state of this peer: nothing to compare with (its spawn).
        if (!sane) { ++m_refused; return net::SV_DROP; }
        Track t;
        t.last = st;
        t.lastTime = serverNowMs;
        t.teleports.Configure(m_limits.teleportsPerSecond, m_limits.teleportBurst);
        m_peers[from] = t;
        if (m_extra && !m_extra(from, st, 0.0f)) { ++m_refused; return net::SV_DROP; }
        ++m_accepted;
        return net::SV_ACCEPT;
    }

    Track& t = it->second;
    if (!sane) return Refuse(t, serverNowMs, correction);

    int32_t dtMs = net::TimeDiff(serverNowMs, t.lastTime);
    if (dtMs < 1) dtMs = 1;
    float dt = static_cast<float>(dtMs) / 1000.0f;

    float dx = st.x - t.last.x, dz = st.z - t.last.z, dy = st.y - t.last.y;
    float horizontal = std::sqrt(dx * dx + dz * dz);
    float maxSpeed = (st.inVehicle || t.last.inVehicle) ? m_limits.maxVehicleSpeed : m_limits.maxFootSpeed;
    bool withinLimits = horizontal <= maxSpeed * dt + m_limits.slackMeters
                     && (dy >= 0.0f ? dy <= maxSpeed * dt + m_limits.slackMeters
                                    : -dy <= m_limits.maxFallSpeed * dt + m_limits.slackMeters);

    if (!withinLimits)
    {
        // A declared teleport may jump, within its own rate budget.
        bool declared = st.teleportSeq != t.last.teleportSeq;
        if (!declared || !t.teleports.Take(serverNowMs)) return Refuse(t, serverNowMs, correction);
    }

    float speed = horizontal / dt;
    if (m_extra && !m_extra(from, st, speed)) { ++m_refused; return net::SV_DROP; }

    t.last = st;
    t.lastTime = serverNowMs;
    t.corrected = false;
    ++m_accepted;
    return net::SV_ACCEPT;
}
