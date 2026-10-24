#pragma once

#include "Packet.h"

#include <cstdint>
#include <map>
#include <set>
#include <tuple>

// net::Ledger
// -----------
// Server-authoritative per-player counters: the base the engine offers for
// economies (currency, tokens, ammo, reputation, ...). The engine knows nothing
// about coins: a game declares counters and, per counter, which REASONS may
// change it and by how much (CounterConfig). The server owns every balance and
// validates each change; unknown counters/reasons are refused (secure default).
//
// Flow (prediction + arbitration, same as WorldState):
//   client : applies the delta locally, sends PID_LEDGER_REQUEST
//            {seq, counter, reason, delta, claim}
//   server : LedgerServer::Apply checks the rule, the balance bounds and, for
//            claimed changes, that this peer never redeemed `claim` before
//            (e.g. a world pickup id) -> PID_LEDGER_RESULT {seq, accepted, balance}
//   client : LedgerClient::OnResult; a refused delta is rolled back by the game.
//
// Balances live for the session (peer ids are per connection); persistence and
// cross-session identity are a separate, future layer.
namespace net
{
    using CounterId = uint16_t;

    enum LedgerReason : uint8_t
    {
        LR_OPENING        = 0, // declare the starting balance (once per peer)
        LR_PICKUP_CLAIMED = 1, // credit for a uniquely identified pickup (needs a claim)
        LR_PICKUP         = 2, // credit for an anonymous dynamic pickup
        LR_REWARD         = 3, // credit from gameplay (race prize, mission reward)
        LR_SPEND          = 4, // debit for a purchase / fee / repair
        LR_LOSS           = 5, // debit by penalty (hit, busted)
        LR_CHEAT          = 6, // debug/cheat credit (refused unless enabled)
        LR_COUNT
    };

    struct LedgerRule
    {
        bool    allowed      = false;
        int32_t minDelta     = 0;      // inclusive
        int32_t maxDelta     = 0;      // inclusive
        bool    requireClaim = false;  // claim != 0, unique per (peer, counter)
        bool    oncePerPeer  = false;  // the reason may be used once per peer
    };

    struct CounterConfig
    {
        int64_t    minBalance = 0;
        int64_t    maxBalance = INT64_MAX;
        LedgerRule reasons[LR_COUNT];
    };

    class LedgerServer
    {
    public:
        enum Result { ACCEPTED, REJECTED };

        void Configure(CounterId counter, const CounterConfig& cfg) { m_config[counter] = cfg; }
        bool IsConfigured(CounterId counter) const { return m_config.count(counter) != 0; }

        // Validates and applies one change. `balance` receives the resulting
        // (accepted) or unchanged (rejected) balance of `peer` for `counter`.
        Result Apply(PeerId peer, CounterId counter, uint8_t reason, int32_t delta,
                     uint64_t claim, int64_t& balance);

        int64_t Balance(PeerId peer, CounterId counter) const;
        void    OnPeerLeft(PeerId peer);
        void    Clear() { m_balances.clear(); m_claims.clear(); m_onceUsed.clear(); }
        void    ClearConfig() { m_config.clear(); }

    private:
        std::map<CounterId, CounterConfig>                       m_config;
        std::map<std::pair<PeerId, CounterId>, int64_t>          m_balances;
        std::set<std::tuple<PeerId, CounterId, uint64_t>>        m_claims;
        std::set<std::tuple<PeerId, CounterId, uint8_t>>         m_onceUsed;
    };

    // What the server decided about one of our predicted changes.
    struct LedgerOutcome
    {
        bool      known    = false; // false: result for a request we never made
        CounterId counter  = 0;
        uint8_t   reason   = 0;
        int32_t   delta    = 0;     // the predicted delta (roll it back if !accepted)
        bool      accepted = false;
        int64_t   balance  = 0;     // authoritative balance after the decision
    };

    class LedgerClient
    {
    public:
        // Records a predicted change; returns the request sequence to send.
        uint32_t Predict(CounterId counter, uint8_t reason, int32_t delta)
        {
            uint32_t s = m_nextSeq++;
            if (m_nextSeq == 0) m_nextSeq = 1;
            Pend p; p.counter = counter; p.reason = reason; p.delta = delta;
            m_pending[s] = p;
            return s;
        }
        LedgerOutcome OnResult(uint32_t seq, CounterId counter, bool accepted, int64_t balance)
        {
            LedgerOutcome o;
            o.counter = counter; o.accepted = accepted; o.balance = balance;
            m_balances[counter] = balance;
            auto it = m_pending.find(seq);
            if (it != m_pending.end() && it->second.counter == counter)
            {
                o.known = true;
                o.reason = it->second.reason;
                o.delta = it->second.delta;
                m_pending.erase(it);
            }
            return o;
        }
        bool     HasBalance(CounterId counter) const { return m_balances.count(counter) != 0; }
        int64_t  Balance(CounterId counter) const
        {
            auto it = m_balances.find(counter);
            return it == m_balances.end() ? 0 : it->second;
        }
        size_t   Pending() const { return m_pending.size(); }
        void     Clear() { m_balances.clear(); m_pending.clear(); m_nextSeq = 1; }

    private:
        struct Pend { CounterId counter; uint8_t reason; int32_t delta; };
        std::map<CounterId, int64_t> m_balances; // last authoritative balances
        std::map<uint32_t, Pend>     m_pending;  // outstanding predictions by seq
        uint32_t m_nextSeq = 1;
    };

    // REQUEST: u32 seq | u16 counter | u8 reason | i32 delta | u64 claim
    // RESULT : u32 seq | u16 counter | u8 accepted | i64 balance
    namespace LedgerCodec
    {
        static constexpr const uint16_t REQUEST_SIZE = 4 + 2 + 1 + 4 + 8;
        static constexpr const uint16_t RESULT_SIZE  = 4 + 2 + 1 + 8;

        uint16_t EncodeRequest(uint8_t* out, uint16_t cap, uint32_t seq, CounterId counter,
                               uint8_t reason, int32_t delta, uint64_t claim);
        bool     DecodeRequest(const uint8_t* d, uint16_t n, uint32_t& seq, CounterId& counter,
                               uint8_t& reason, int32_t& delta, uint64_t& claim);
        uint16_t EncodeResult(uint8_t* out, uint16_t cap, uint32_t seq, CounterId counter,
                              bool accepted, int64_t balance);
        bool     DecodeResult(const uint8_t* d, uint16_t n, uint32_t& seq, CounterId& counter,
                              bool& accepted, int64_t& balance);
    }
}
