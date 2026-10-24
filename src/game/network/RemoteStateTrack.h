#pragma once

#include "PlayerNetworkState.h"
#include "../netengine/NetTime.h"

#include <cmath>
#include <functional>

// Timestamped history of one remote player's state, rendered behind the server
// clock (snapshot interpolation). Shared by the on-foot and the vehicle views.
//
// Every relayed state carries the server time its sender sampled it at. The
// viewer renders at serverNow - interpolationDelay, which normally lies between
// two received samples, so motion follows the sender's real trajectory
// (including jump arcs) regardless of packet jitter; a late packet is covered
// by a short extrapolation along the last segment.
//
// Untimed input (time 0: no clock, e.g. unit tests) degrades to "show the
// newest state".
class RemoteStateTrack
{
public:
    typedef std::function<uint32_t()> ClockFn;

    struct Timing
    {
        ClockFn  clock;
        uint32_t interpolationDelayMs = 150; // ~1.5 send periods at 10 Hz
        uint32_t maxExtrapolationMs   = 250;
    };

    void Clear() { m_buffer.Clear(); m_lastTime = 0; m_hasLast = false; }
    bool Empty() const { return m_buffer.Empty(); }

    // Returns true if `state` is a new sample (not the same packet seen again).
    bool Push(uint32_t serverTimeMs, const PlayerNetworkState& state)
    {
        if (serverTimeMs == 0)
        {
            m_buffer.Clear();
            m_buffer.Push(0, state);
            m_hasLast = true;
            m_lastTime = 0;
            return true;
        }
        if (m_hasLast && serverTimeMs == m_lastTime) return false;
        if (m_hasLast && m_lastTime == 0) m_buffer.Clear(); // switching to timed samples
        m_buffer.Push(serverTimeMs, state);
        m_lastTime = serverTimeMs;
        m_hasLast = true;
        return true;
    }

    const PlayerNetworkState& Newest() const { return m_buffer.Newest().value; }

    // The state to show now. False if nothing was received yet.
    bool Render(const Timing& timing, PlayerNetworkState& out)
    {
        if (m_buffer.Empty()) return false;
        if (!timing.clock || m_lastTime == 0)
        {
            out = m_buffer.Newest().value;
            return true;
        }
        uint32_t renderTime = timing.clock() - timing.interpolationDelayMs;
        const PlayerNetworkState* a = nullptr;
        const PlayerNetworkState* b = nullptr;
        float t = 0.0f;
        if (!m_buffer.Sample(renderTime, timing.maxExtrapolationMs, a, b, t)) return false;
        out = Blend(*a, *b, t);
        m_buffer.DiscardBefore(renderTime);
        return true;
    }

    static float LerpAngle(float a, float b, float t)
    {
        const float kPi = 3.14159265f;
        float d = b - a;
        while (d > kPi)  d -= 2.0f * kPi;
        while (d < -kPi) d += 2.0f * kPi;
        float r = a + d * t;
        while (r > kPi)  r -= 2.0f * kPi;
        while (r < -kPi) r += 2.0f * kPi;
        return r;
    }

    // Continuous fields are blended; discrete ones (flags, states, locomotion
    // inputs, model) come from the nearer sample.
    static PlayerNetworkState Blend(const PlayerNetworkState& a, const PlayerNetworkState& b, float t)
    {
        PlayerNetworkState s = t < 0.5f ? a : b;
        s.x = a.x + (b.x - a.x) * t;
        s.y = a.y + (b.y - a.y) * t;
        s.z = a.z + (b.z - a.z) * t;
        s.rotY = LerpAngle(a.rotY, b.rotY, t);
        s.vehYaw = LerpAngle(a.vehYaw, b.vehYaw, t);
        return s;
    }

private:
    net::SnapshotBuffer<PlayerNetworkState> m_buffer;
    uint32_t m_lastTime = 0;
    bool     m_hasLast = false;
};
