#include "PlatformDirWorldEventEmitter.h"
#include "WorldEvent.h"
#include "NetworkMatchManager.h"

namespace net
{
    void EmitPlatformDirWorldEvent(uint32_t objectUID, int32_t dir)
    {
        // No match (or no manager yet): nowhere to send. Guard first so a
        // server/dedicated build without a networking game session is a no-op.
        NetworkMatchManager* match = NetworkMatchManager::GetInstance();
        if (match == nullptr || !match->IsInMatch()) return;

        uint8_t payload[PlatformDirPayload::SERIALIZED_SIZE];
        PlatformDirPayload::Encode(payload, objectUID, static_cast<int8_t>(dir));
        match->SendWorldEvent(WE_PLATFORM_DIR, payload, sizeof(payload));
    }
}