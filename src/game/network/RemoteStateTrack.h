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
//
// The server may forward a distant player's samples at a lower rate (interest
// management). With Timing::adaptive the delay follows the measured sample
// spacing (two intervals, never below interpolationDelayMs) and slews there
// gradually, so playback speed changes slightly instead of the pose jumping.
// A sample more than kGapResetMs after the newest one starts a new history
// (the player was not relevant for a while): no slide across the gap.
// World velocity of the rendered pose (m/s), from the bracketing samples.
// Receivers drive animation with it (walk/run cycle matches the real motion).
struct RemoteMotion
{
    float vx = 0.0f, vy = 0.0f, vz = 0.0f;
    float HorizontalSpeed() const { return std::sqrt(vx * vx + vz * vz); }
};

class RemoteStateTrack
{
public:
    typedef std::function<uint32_t()> ClockFn;

    struct Timing
    {
        ClockFn  clock;
        uint32_t interpolationDelayMs = 100; // two send periods at 20 Hz
        uint32_t maxExtrapolationMs   = 250;
        bool     adaptive = false;            // delay follows the sample spacing
    };

    static constexpr const uint32_t kGapResetMs = 1000;
    static constexpr const float    kDelaySlew  = 0.25f; // delay change per elapsed ms

    void Clear()
    {
        m_buffer.Clear();
        m_lastTime = 0;
        m_hasLast = false;
        m_intervalMs = 0.0f;
        m_delayMs = 0.0f;
        m_lastRenderClock = 0;
    }
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
        if (!m_buffer.Empty())
        {
            int32_t gap = net::TimeDiff(serverTimeMs, m_buffer.Newest().time);
            if (gap > static_cast<int32_t>(kGapResetMs))
            {
                m_buffer.Clear(); // a new history: do not blend across the gap
            }
            else if (gap > 0)
            {
                float g = static_cast<float>(gap);
                m_intervalMs = m_intervalMs <= 0.0f ? g : m_intervalMs + (g - m_intervalMs) * 0.25f;
            }
        }
        m_buffer.Push(serverTimeMs, state);
        m_lastTime = serverTimeMs;
        m_hasLast = true;
        return true;
    }

    const PlayerNetworkState& Newest() const { return m_buffer.Newest().value; }

    // The state to show now (and its velocity). False if nothing was received.
    bool Render(const Timing& timing, PlayerNetworkState& out, RemoteMotion* motion = nullptr)
    {
        if (motion) *motion = RemoteMotion();
        if (m_buffer.Empty()) return false;
        if (!timing.clock || m_lastTime == 0)
        {
            out = m_buffer.Newest().value;
            return true;
        }
        uint32_t now = timing.clock();
        uint32_t renderTime = now - (timing.adaptive ? AdaptDelay(timing, now) : timing.interpolationDelayMs);
        typedef net::SnapshotBuffer<PlayerNetworkState>::Item Item;
        const Item* a = nullptr;
        const Item* b = nullptr;
        float t = 0.0f;
        if (!m_buffer.SampleItems(renderTime, timing.maxExtrapolationMs, a, b, t)) return false;
        out = Blend(a->value, b->value, t);
        int32_t spanMs = net::TimeDiff(b->time, a->time);
        if (motion && spanMs > 0)
        {
            float inv = 1000.0f / static_cast<float>(spanMs);
            motion->vx = (b->value.x - a->value.x) * inv;
            motion->vy = (b->value.y - a->value.y) * inv;
            motion->vz = (b->value.z - a->value.z) * inv;
        }
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

    // The delay currently applied (adaptive timing; 0 before the first render).
    float CurrentDelayMs() const { return m_delayMs; }

private:
    uint32_t AdaptDelay(const Timing& timing, uint32_t now)
    {
        float target = 2.0f * m_intervalMs;
        float base = static_cast<float>(timing.interpolationDelayMs);
        if (target < base) target = base;
        if (m_delayMs <= 0.0f)
        {
            m_delayMs = target;
        }
        else
        {
            int32_t elapsed = net::TimeDiff(now, m_lastRenderClock);
            if (elapsed < 0) elapsed = 0;
            if (elapsed > 100) elapsed = 100;
            float step = kDelaySlew * static_cast<float>(elapsed);
            if (m_delayMs < target) m_delayMs = m_delayMs + step < target ? m_delayMs + step : target;
            else if (m_delayMs > target) m_delayMs = m_delayMs - step > target ? m_delayMs - step : target;
        }
        m_lastRenderClock = now;
        return static_cast<uint32_t>(m_delayMs + 0.5f);
    }

    net::SnapshotBuffer<PlayerNetworkState> m_buffer;
    uint32_t m_lastTime = 0;
    bool     m_hasLast = false;
    float    m_intervalMs = 0.0f;   // smoothed spacing of received samples
    float    m_delayMs = 0.0f;      // applied delay (adaptive timing)
    uint32_t m_lastRenderClock = 0;
};
