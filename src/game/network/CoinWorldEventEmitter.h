#pragma once

#include "../netengine/Packet.h"

#include <cstdint>

// CoinWorldEventEmitter broadcasts a WE_COIN_COLLECTED world event (over the
// open PID_USER relay) whenever a permanent world coin is picked up, so remote
// peers can mirror the collected persistent id. The payload is deliberately
// game-agnostic POD (persistent id + world position); game types never leak
// through this interface. Safe no-op when no match is active.
namespace net
{
    // Payload for WE_COIN_COLLECTED: the persistent object id of the collected
    // coin + the world position where it was collected.
    struct CoinCollectedPayload
    {
        int32_t persistentId = 0;
        float   x = 0.0f;
        float   y = 0.0f;
        float   z = 0.0f;

        static constexpr const int SERIALIZED_SIZE = sizeof(int32_t) + sizeof(float) * 3;
        static_assert(SERIALIZED_SIZE >= 0 && SERIALIZED_SIZE < MAX_PACKET_PAYLOAD,
                      "coin-collected payload must fit a packet");

        static void Encode(uint8_t* out, int32_t id, float px, float py, float pz)
        {
            Writer w(out, SERIALIZED_SIZE);
            w.WriteU32(static_cast<uint32_t>(id));
            w.WriteFloat(px);
            w.WriteFloat(py);
            w.WriteFloat(pz);
        }

        static bool Decode(const uint8_t* data, uint16_t size, CoinCollectedPayload& out)
        {
            if (size < static_cast<uint16_t>(SERIALIZED_SIZE)) return false;
            Reader r(data, size);
            uint32_t id = 0;
            if (!r.ReadU32(id)) return false;
            out.persistentId = static_cast<int32_t>(id);
            return r.ReadFloat(out.x) && r.ReadFloat(out.y) && r.ReadFloat(out.z);
        }
    };

    // Diffuses a coin-collected world event to all peers in the current match.
    // Safe no-op when there is no active match. Invoked from the game's
    // persistent-coin break point (CoinManager::CheckCollection).
    void EmitCoinCollectedWorldEvent(int32_t persistentId, float x, float y, float z);
}