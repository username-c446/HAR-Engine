#pragma once

#include "../netengine/Ledger.h"

#include <cstdint>
#include <functional>

// PlayerEconomy
// -------------
// Game-side gateway between the local economy (coin bank) and the server
// ledger (net::Ledger). Every local change is reported with its reason; the
// local value is a prediction. Whenever no request of ours is outstanding, the
// local balance converges to the server's authoritative balance — which rolls
// back refused changes and also repairs any drift. Game-agnostic: the coin bank
// is reached through the two callbacks.
class PlayerEconomy
{
public:
    typedef std::function<bool(net::CounterId, uint8_t reason, int32_t delta, uint64_t claim)> SendFn;
    typedef int64_t (*GetBalanceFn)(net::CounterId);
    typedef void    (*SetBalanceFn)(net::CounterId, int64_t); // must not report back

    void SetSender(SendFn fn) { m_send = std::move(fn); }
    void SetBalanceAccess(GetBalanceFn get, SetBalanceFn set) { m_get = get; m_set = set; }

    // Match start: declares the opening balance (once per match).
    void OnMatchStart(net::CounterId counter);
    void OnMatchEnd() { m_active = false; m_corrections = 0; m_refused = 0; }

    // A local change already applied to the bank. Ignored while converging.
    bool Report(net::CounterId counter, uint8_t reason, int32_t delta, uint64_t claim = 0);

    // Server decision for one of our requests; `stillPending` = our requests
    // that remain outstanding after this one.
    void OnOutcome(const net::LedgerOutcome& o, size_t stillPending);

    bool     IsActive() const { return m_active; }
    bool     IsConverging() const { return m_converging; }
    unsigned Refused() const { return m_refused; }
    unsigned Corrections() const { return m_corrections; }

private:
    SendFn       m_send;
    GetBalanceFn m_get = nullptr;
    SetBalanceFn m_set = nullptr;
    bool     m_active = false;
    bool     m_converging = false;
    unsigned m_refused = 0;
    unsigned m_corrections = 0;
};
