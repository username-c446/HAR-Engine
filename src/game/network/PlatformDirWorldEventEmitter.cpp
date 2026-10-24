#include "PlatformDirWorldEventEmitter.h"
#include "WorldEvent.h"
#include "NetworkMatchManager.h"
#include "WorldEventApply.h"

namespace net
{
    void EmitPlatformDirWorldEvent(uint32_t objectUID, int32_t dir, float x, float y, float z)
    {
        // No match (or no manager yet): nowhere to send. Guard first so a
        // server/dedicated build without a networking game session is a no-op.
        NetworkMatchManager* match = NetworkMatchManager::GetInstance();
        if (match == nullptr || !match->IsInMatch()) return;
        // Mirroring a remote peer's change: never re-broadcast it.
        if (IsApplyingRemoteWorldEvent()) return;

        uint8_t payload[PlatformDirPayload::SERIALIZED_SIZE];
        PlatformDirPayload::Encode(payload, objectUID, static_cast<int8_t>(dir), x, y, z);
        match->SendWorldEvent(WE_PLATFORM_DIR, payload, sizeof(payload));
    }
}