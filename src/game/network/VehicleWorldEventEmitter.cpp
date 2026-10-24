#include "VehicleWorldEventEmitter.h"
#include "WorldEvent.h"
#include "NetworkMatchManager.h"

#include <worldsim/redbrick/vehicle.h>

namespace net
{
    void EmitVehicleDestroyedWorldEvent(Vehicle* vehicle)
    {
        // No match (or no manager yet): nowhere to send. Guard first so a
        // server/dedicated build without a networking game session is a no-op.
        NetworkMatchManager* match = NetworkMatchManager::GetInstance();
        if (match == nullptr || !match->IsInMatch()) return;
        if (vehicle == nullptr) return;

        // Stable net vehicle id: mirror the player-state convention, which uses
        // the VehicleCentral slot index (mVehicleCentralIndex >= 0), falling
        // back to a pointer-derived id so it is at least stable per session.
        int netVehicleId = vehicle->mVehicleCentralIndex;
        if (netVehicleId < 0)
            netVehicleId = static_cast<int>(
                reinterpret_cast<uintptr_t>(vehicle) & 0x7FFFFFFF);

        // World position of the blast so receivers know where to mirror it.
        const rmt::Vector& pos = vehicle->GetPosition();

        uint8_t payload[VehicleDestroyedPayload::SERIALIZED_SIZE];
        VehicleDestroyedPayload::Encode(payload, netVehicleId, pos.x, pos.y, pos.z);
        match->SendWorldEvent(WE_VEHICLE_DESTROYED, payload, sizeof(payload));
    }
}