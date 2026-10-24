#include "CoinWorldEventEmitter.h"
#include "WorldEvent.h"
#include "NetworkMatchManager.h"

namespace net
{
    void EmitCoinCollectedWorldEvent(int32_t persistentId, float x, float y, float z)
    {
        // No match (or no manager yet): nowhere to send. Guard first so a
        // server/dedicated build without a networking game session is a no-op.
        NetworkMatchManager* match = NetworkMatchManager::GetInstance();
        if (match == nullptr || !match->IsInMatch()) return;

        uint8_t payload[CoinCollectedPayload::SERIALIZED_SIZE];
        CoinCollectedPayload::Encode(payload, persistentId, x, y, z);
        match->SendWorldEvent(WE_COIN_COLLECTED, payload, sizeof(payload));
    }
}