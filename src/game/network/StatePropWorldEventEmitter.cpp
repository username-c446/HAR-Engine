#include "StatePropWorldEventEmitter.h"
#include "WorldEvent.h"
#include "NetworkMatchManager.h"
#include "WorldEventApply.h"

namespace net
{
    void EmitStatePropWorldEvent(uint32_t objectUID, uint8_t state, float x, float y, float z)
    {
        // No match (or no manager yet): nowhere to send. Guard first so a
        // server/dedicated build without a networking game session is a no-op.
        NetworkMatchManager* match = NetworkMatchManager::GetInstance();
        if (match == nullptr || !match->IsInMatch()) return;
        // Mirroring a remote peer's change: never re-broadcast it.
        if (IsApplyingRemoteWorldEvent()) return;

        uint8_t payload[StatePropPayload::SERIALIZED_SIZE];
        StatePropPayload::Encode(payload, objectUID, state, x, y, z);
        match->SendWorldEvent(WE_STATEPROP, payload, sizeof(payload));
    }
}