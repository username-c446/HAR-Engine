#pragma once

#include "../netengine/Ledger.h"

// Game economy definition, shared by every server flavour (hosting game and
// the dedicated mp_standalone server) so both enforce the same rules.
//
// Coins are PER PLAYER: each player collects their own world coins; the server
// keeps each player's balance and refuses impossible changes (a world coin
// redeemed twice, a spend below zero, an oversized award, cheats unless the
// session allows them).
namespace economy
{
    enum Counter : net::CounterId
    {
        COUNTER_COINS = 1,
    };

    // Upper bounds of a single legitimate change, by source.
    static constexpr const int32_t kMaxOpeningCoins     = 1000000; // save-game balance declared on join
    static constexpr const int32_t kMaxDynamicPickup    = 50;      // crate / car-loss burst collected at once
    static constexpr const int32_t kMaxRewardCoins      = 100000;  // race prize (fee + odds * fee)
    static constexpr const int32_t kMaxSpendCoins       = 100000;  // purchase / fee / repair
    static constexpr const int32_t kMaxCheatCoins       = 100;

    inline net::CounterConfig MakeCoinConfig(bool allowCheats)
    {
        net::CounterConfig c;
        c.minBalance = 0;
        c.maxBalance = 100000000;

        net::LedgerRule& opening = c.reasons[net::LR_OPENING];
        opening.allowed = true; opening.minDelta = 0; opening.maxDelta = kMaxOpeningCoins; opening.oncePerPeer = true;

        net::LedgerRule& claimed = c.reasons[net::LR_PICKUP_CLAIMED];
        claimed.allowed = true; claimed.minDelta = 1; claimed.maxDelta = 1; claimed.requireClaim = true;

        net::LedgerRule& pickup = c.reasons[net::LR_PICKUP];
        pickup.allowed = true; pickup.minDelta = 1; pickup.maxDelta = kMaxDynamicPickup;

        net::LedgerRule& reward = c.reasons[net::LR_REWARD];
        reward.allowed = true; reward.minDelta = 1; reward.maxDelta = kMaxRewardCoins;

        net::LedgerRule& spend = c.reasons[net::LR_SPEND];
        spend.allowed = true; spend.minDelta = -kMaxSpendCoins; spend.maxDelta = -1;

        net::LedgerRule& loss = c.reasons[net::LR_LOSS];
        loss.allowed = true; loss.minDelta = -kMaxSpendCoins; loss.maxDelta = -1;

        net::LedgerRule& cheat = c.reasons[net::LR_CHEAT];
        cheat.allowed = allowCheats; cheat.minDelta = 1; cheat.maxDelta = kMaxCheatCoins;
        return c;
    }
}
