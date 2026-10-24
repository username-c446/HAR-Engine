#include "PlayerEconomy.h"

void PlayerEconomy::OnMatchStart(net::CounterId counter)
{
    m_active = true;
    m_refused = 0;
    m_corrections = 0;
    if (!m_send || !m_get) return;
    int64_t opening = m_get(counter);
    if (opening < 0) opening = 0;
    if (opening > INT32_MAX) opening = INT32_MAX;
    m_send(counter, net::LR_OPENING, static_cast<int32_t>(opening), 0);
}

bool PlayerEconomy::Report(net::CounterId counter, uint8_t reason, int32_t delta, uint64_t claim)
{
    if (!m_active || m_converging || !m_send || delta == 0) return false;
    return m_send(counter, reason, delta, claim);
}

void PlayerEconomy::OnOutcome(const net::LedgerOutcome& o, size_t stillPending)
{
    if (!m_active) return;
    if (o.known && !o.accepted) ++m_refused;
    if (o.known && o.reason == net::LR_OPENING && !o.accepted)
    {
        // The server would not take our opening balance (e.g. over its cap):
        // the ledger cannot vouch for this player, so stop syncing rather than
        // "converge" a real save-game balance to zero.
        m_active = false;
        return;
    }

    // Converge only when nothing of ours is in flight: later predictions are
    // not yet reflected in the server balance.
    if (stillPending != 0 || !m_get || !m_set) return;
    int64_t local = m_get(o.counter);
    if (local == o.balance) return;

    m_converging = true;
    m_set(o.counter, o.balance);
    m_converging = false;
    ++m_corrections;
}
