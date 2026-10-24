#pragma once

#include "PlayerNetworkState.h"

#include <cstdint>

// LocalActionLatch
// ----------------
// Captures the local player's one-shot actions (dodge, attack, celebration,
// knock-down, ...) at the moment the game sequences them, and holds the latest
// one for the state provider. Player state ships only every few frames, so a
// sampled "what is the character doing now" would miss short clips; a latch
// does not. Each report bumps a 3-bit sequence so the receiver can tell a
// repeated action from the same held value (PlayerNetworkState::MakeActionEvent).
//
// Header-only and game-agnostic: the game calls Report() from its action sites
// for the local avatar only; tests drive it directly.
namespace net
{
    class LocalActionLatch
    {
    public:
        static void Report(uint8_t actionId)
        {
            if (actionId == PlayerNetworkState::ACTION_NONE) return;
            State& s = Get();
            s.seq = static_cast<uint8_t>((s.seq + 1) & 0x07);
            s.value = PlayerNetworkState::MakeActionEvent(actionId, s.seq);
        }

        // Latest held action byte (0 until the first report of a session).
        static uint8_t Current() { return Get().value; }

        // New match / level: forget the held action so a late peer does not
        // replay a stale one.
        static void Reset() { Get() = State(); }

    private:
        struct State
        {
            uint8_t value = 0;
            uint8_t seq = 0;
        };
        static State& Get()
        {
            static State s_state;
            return s_state;
        }
    };
}
