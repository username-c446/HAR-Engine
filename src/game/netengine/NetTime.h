#pragma once

#include <cstdint>
#include <deque>

// Time primitives of the network layer, header-only and free of any transport
// so they are unit-testable with synthetic clocks.
//
//   ClockSync          - client estimate of the server clock from PING/PONG
//   SnapshotBuffer<T>  - timestamped samples + bracketing lookup (jitter buffer)
//   TokenBucket        - per-peer rate limiting on the server
//
// All times are u32 milliseconds. Differences are taken as signed 32-bit, so a
// wrap of the server clock (~49 days) is harmless.
namespace net
{
    inline int32_t TimeDiff(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b); }

    // ---------------------------------------------------------------------
    // ClockSync
    // ---------------------------------------------------------------------
    // Each PONG gives (clientSent, serverTime, clientReceived). The server
    // stamped serverTime somewhere inside the round trip, so the best estimate of
    // the server clock at clientReceived is serverTime + rtt/2; the sample with
    // the smallest RTT has the smallest error bound (rtt/2). The offset is taken
    // from the lowest-RTT sample of the last kWindow samples, which rejects
    // samples delayed by queueing. The RTT reported to the game is smoothed.
    class ClockSync
    {
    public:
        static constexpr const size_t kWindow = 8;

        void Reset()
        {
            m_samples.clear();
            m_synced = false;
            m_offset = 0;
            m_rttMs = 0;
            m_smoothedRtt = 0.0f;
        }

        void OnPong(uint32_t clientSentMs, uint32_t serverTimeMs, uint32_t clientNowMs)
        {
            int32_t rtt = TimeDiff(clientNowMs, clientSentMs);
            if (rtt < 0) return; // a stale echo from before a reset
            Sample s;
            s.rtt = static_cast<uint32_t>(rtt);
            s.offset = TimeDiff(serverTimeMs + s.rtt / 2, clientNowMs);
            m_samples.push_back(s);
            if (m_samples.size() > kWindow) m_samples.pop_front();

            const Sample* best = &m_samples.front();
            for (const Sample& c : m_samples)
                if (c.rtt < best->rtt) best = &c;
            m_offset = best->offset;

            m_smoothedRtt = m_synced ? m_smoothedRtt + (static_cast<float>(s.rtt) - m_smoothedRtt) * 0.125f
                                     : static_cast<float>(s.rtt);
            m_rttMs = static_cast<uint32_t>(m_smoothedRtt + 0.5f);
            m_synced = true;
        }

        // Seeds the estimate from WELCOME (no RTT known yet: assume 0).
        void SeedFromWelcome(uint32_t serverTimeMs, uint32_t clientNowMs)
        {
            if (m_synced) return;
            m_offset = TimeDiff(serverTimeMs, clientNowMs);
            m_synced = true;
        }

        bool     IsSynced() const { return m_synced; }
        uint32_t ServerTime(uint32_t clientNowMs) const { return clientNowMs + static_cast<uint32_t>(m_offset); }
        uint32_t RttMs() const { return m_rttMs; }

    private:
        struct Sample { uint32_t rtt; int32_t offset; };
        std::deque<Sample> m_samples;
        bool     m_synced = false;
        int32_t  m_offset = 0;
        uint32_t m_rttMs = 0;
        float    m_smoothedRtt = 0.0f;
    };

    // ---------------------------------------------------------------------
    // SnapshotBuffer<T>
    // ---------------------------------------------------------------------
    // Holds timestamped samples of one remote object (server time). A viewer
    // renders the object at renderTime = serverNow - interpolationDelay, which
    // lies between two received samples whenever packets arrive on time; the
    // delay absorbs network jitter. Sample() returns the bracketing pair and the
    // blend factor, or extrapolation past the newest sample (alpha > 1, capped by
    // the caller's limit), or the oldest sample before the history starts.
    template <typename T>
    class SnapshotBuffer
    {
    public:
        static constexpr const size_t kCapacity = 32;

        struct Item { uint32_t time; T value; };

        void Clear() { m_items.clear(); }
        bool Empty() const { return m_items.empty(); }
        size_t Size() const { return m_items.size(); }
        const Item& Newest() const { return m_items.back(); }

        // Samples older than the newest are inserted in order; duplicates and
        // samples older than the whole history are dropped.
        void Push(uint32_t time, const T& value)
        {
            if (!m_items.empty())
            {
                if (TimeDiff(time, m_items.front().time) < 0 && m_items.size() >= kCapacity) return;
                for (auto it = m_items.rbegin(); it != m_items.rend(); ++it)
                {
                    int32_t d = TimeDiff(time, it->time);
                    if (d == 0) { it->value = value; return; }
                    if (d > 0)
                    {
                        m_items.insert(it.base(), Item{ time, value });
                        Trim();
                        return;
                    }
                }
                m_items.push_front(Item{ time, value });
                Trim();
                return;
            }
            m_items.push_back(Item{ time, value });
        }

        // Finds the pair to blend for `renderTime`. Returns false when empty.
        // `from`/`to` may be the same item (single sample / before history).
        // alpha in [0,1] interpolates; alpha > 1 extrapolates past `to`, limited
        // to maxExtrapolateMs beyond the newest sample.
        bool Sample(uint32_t renderTime, uint32_t maxExtrapolateMs,
                    const T*& from, const T*& to, float& alpha) const
        {
            if (m_items.empty()) return false;
            if (m_items.size() == 1 || TimeDiff(renderTime, m_items.front().time) <= 0)
            {
                from = to = &m_items.front().value;
                alpha = 0.0f;
                return true;
            }
            for (size_t i = 1; i < m_items.size(); ++i)
            {
                const Item& b = m_items[i];
                if (TimeDiff(renderTime, b.time) <= 0)
                {
                    const Item& a = m_items[i - 1];
                    int32_t span = TimeDiff(b.time, a.time);
                    from = &a.value;
                    to = &b.value;
                    alpha = span > 0 ? static_cast<float>(TimeDiff(renderTime, a.time)) / span : 1.0f;
                    return true;
                }
            }
            // Past the newest sample: extrapolate along the last segment.
            const Item& a = m_items[m_items.size() - 2];
            const Item& b = m_items.back();
            int32_t span = TimeDiff(b.time, a.time);
            int32_t past = TimeDiff(renderTime, b.time);
            if (past > static_cast<int32_t>(maxExtrapolateMs)) past = static_cast<int32_t>(maxExtrapolateMs);
            from = &a.value;
            to = &b.value;
            alpha = span > 0 ? 1.0f + static_cast<float>(past) / span : 1.0f;
            return true;
        }

        // Drops samples no longer needed to bracket renderTime (keeps one older).
        void DiscardBefore(uint32_t renderTime)
        {
            while (m_items.size() > 2 && TimeDiff(renderTime, m_items[1].time) > 0)
                m_items.pop_front();
        }

    private:
        void Trim()
        {
            while (m_items.size() > kCapacity) m_items.pop_front();
        }

        std::deque<Item> m_items;
    };

    // ---------------------------------------------------------------------
    // TokenBucket
    // ---------------------------------------------------------------------
    // `rate` tokens per second refill up to `burst`; Take(n) spends n tokens or
    // refuses. Used by the server to bound what one peer may send per class.
    class TokenBucket
    {
    public:
        void Configure(float ratePerSecond, float burst)
        {
            m_rate = ratePerSecond;
            m_burst = burst;
            m_tokens = burst;
            m_primed = false;
        }

        bool Take(uint32_t nowMs, float n = 1.0f)
        {
            if (m_rate <= 0.0f) return true; // unlimited
            if (!m_primed) { m_last = nowMs; m_primed = true; }
            int32_t dt = TimeDiff(nowMs, m_last);
            if (dt > 0)
            {
                m_tokens += m_rate * (static_cast<float>(dt) / 1000.0f);
                if (m_tokens > m_burst) m_tokens = m_burst;
                m_last = nowMs;
            }
            if (m_tokens < n) return false;
            m_tokens -= n;
            return true;
        }

    private:
        float    m_rate = 0.0f;
        float    m_burst = 0.0f;
        float    m_tokens = 0.0f;
        uint32_t m_last = 0;
        bool     m_primed = false;
    };
}
