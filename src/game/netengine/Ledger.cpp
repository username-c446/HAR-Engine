#include "Ledger.h"

namespace net
{
    LedgerServer::Result LedgerServer::Apply(PeerId peer, CounterId counter, uint8_t reason,
                                             int32_t delta, uint64_t claim, int64_t& balance)
    {
        balance = Balance(peer, counter);

        auto cfgIt = m_config.find(counter);
        if (cfgIt == m_config.end() || reason >= LR_COUNT) return REJECTED;
        const CounterConfig& cfg = cfgIt->second;
        const LedgerRule& rule = cfg.reasons[reason];

        if (!rule.allowed) return REJECTED;
        if (delta < rule.minDelta || delta > rule.maxDelta) return REJECTED;
        if (rule.requireClaim && claim == 0) return REJECTED;
        if (rule.requireClaim && m_claims.count(std::make_tuple(peer, counter, claim))) return REJECTED;
        if (rule.oncePerPeer && m_onceUsed.count(std::make_tuple(peer, counter, reason))) return REJECTED;

        // Bounds are checked on the exact result (no overflow: deltas are 32-bit).
        int64_t next = balance + static_cast<int64_t>(delta);
        if (next < cfg.minBalance || next > cfg.maxBalance) return REJECTED;

        m_balances[std::make_pair(peer, counter)] = next;
        if (rule.requireClaim) m_claims.insert(std::make_tuple(peer, counter, claim));
        if (rule.oncePerPeer)  m_onceUsed.insert(std::make_tuple(peer, counter, reason));
        balance = next;
        return ACCEPTED;
    }

    int64_t LedgerServer::Balance(PeerId peer, CounterId counter) const
    {
        auto it = m_balances.find(std::make_pair(peer, counter));
        return it == m_balances.end() ? 0 : it->second;
    }

    void LedgerServer::OnPeerLeft(PeerId peer)
    {
        for (auto it = m_balances.begin(); it != m_balances.end(); )
            it = (it->first.first == peer) ? m_balances.erase(it) : std::next(it);
        for (auto it = m_claims.begin(); it != m_claims.end(); )
            it = (std::get<0>(*it) == peer) ? m_claims.erase(it) : std::next(it);
        for (auto it = m_onceUsed.begin(); it != m_onceUsed.end(); )
            it = (std::get<0>(*it) == peer) ? m_onceUsed.erase(it) : std::next(it);
    }

    namespace LedgerCodec
    {
        uint16_t EncodeRequest(uint8_t* out, uint16_t cap, uint32_t seq, CounterId counter,
                               uint8_t reason, int32_t delta, uint64_t claim)
        {
            Writer w(out, cap);
            bool ok = w.WriteU32(seq) && w.WriteU16(counter) && w.WriteU8(reason)
                   && w.WriteU32(static_cast<uint32_t>(delta))
                   && w.WriteU32(static_cast<uint32_t>(claim & 0xFFFFFFFFull))
                   && w.WriteU32(static_cast<uint32_t>(claim >> 32));
            return (ok && w.Ok()) ? w.Position() : 0;
        }

        bool DecodeRequest(const uint8_t* d, uint16_t n, uint32_t& seq, CounterId& counter,
                           uint8_t& reason, int32_t& delta, uint64_t& claim)
        {
            Reader r(d, n);
            uint32_t ud = 0, lo = 0, hi = 0;
            if (!r.ReadU32(seq) || !r.ReadU16(counter) || !r.ReadU8(reason) || !r.ReadU32(ud)
                || !r.ReadU32(lo) || !r.ReadU32(hi))
                return false;
            delta = static_cast<int32_t>(ud);
            claim = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
            return true;
        }

        uint16_t EncodeResult(uint8_t* out, uint16_t cap, uint32_t seq, CounterId counter,
                              bool accepted, int64_t balance)
        {
            Writer w(out, cap);
            uint64_t b = static_cast<uint64_t>(balance);
            bool ok = w.WriteU32(seq) && w.WriteU16(counter) && w.WriteU8(accepted ? 1 : 0)
                   && w.WriteU32(static_cast<uint32_t>(b & 0xFFFFFFFFull))
                   && w.WriteU32(static_cast<uint32_t>(b >> 32));
            return (ok && w.Ok()) ? w.Position() : 0;
        }

        bool DecodeResult(const uint8_t* d, uint16_t n, uint32_t& seq, CounterId& counter,
                          bool& accepted, int64_t& balance)
        {
            Reader r(d, n);
            uint8_t a = 0;
            uint32_t lo = 0, hi = 0;
            if (!r.ReadU32(seq) || !r.ReadU16(counter) || !r.ReadU8(a) || !r.ReadU32(lo) || !r.ReadU32(hi))
                return false;
            accepted = a != 0;
            balance = static_cast<int64_t>(static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32));
            return true;
        }
    }
}
