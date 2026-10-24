#pragma once

// Forward declaration only — the implementation pulls in the heavy worldsim
// headers. VehicleWorldEventEmitter broadcasts a WE_VEHICLE_DESTROYED world
// event (over the open PID_USER relay) whenever a vehicle is destroyed, so
// remote peers can mirror the husk / drop their ghost of that machine.
class Vehicle;

namespace net
{
    // Diffuses a vehicle-destroyed world event (netVehicleId + position) to all
    // peers in the current match. Safe no-op when there is no active match or
    // no net id can be derived. Invoked from the game's EVENT_VEHICLE_DESTROYED
    // firing sites (world engine side). Takes a non-const Vehicle because
    // Vehicle::GetPosition()/mVehicleCentralIndex are non-const members.
    void EmitVehicleDestroyedWorldEvent(Vehicle* vehicle);
}