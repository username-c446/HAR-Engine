#pragma once

#include "PlayerNetworkState.h"
#include "RemotePlayerManager.h"
#include "RemoteStateTrack.h"

#include <functional>
#include <map>
#include <cstring>

// Bridges remote players' vehicle state (inVehicle + vehicleModel +
// netVehicleId + the kinematic mirror) to a world-side "ghost" machine.
//
// Only the data-level concern lives here: knowing when a remote driver entered
// or left a machine, telling the sink to spawn a vehicle of the reported model,
// and rendering it by snapshot interpolation (RemoteStateTrack). The in-world
// vehicle creation/movement is done by the sink, so this class stays
// unit-testable.
class RemoteVehicleWorldView
{
public:
    // Sink that creates/moves/removes a visible vehicle for a remote driver.
    // playerId matches the network client id; model is the reported vehicle
    // model name (e.g. "homer", "cops_v"). Returns an opaque handle passed back
    // to later Move/Despawn calls.
    typedef void* (*SpawnFn)(int playerId, const char* model, int netVehicleId);
    typedef void  (*MoveFn)(int playerId, void* handle, const PlayerNetworkState& state);
    typedef void  (*DespawnFn)(int playerId, void* handle);

    struct Sink
    {
        SpawnFn spawn;
        MoveFn move;
        DespawnFn despawn;
    };

    RemoteVehicleWorldView();
    ~RemoteVehicleWorldView();

    void SetSink(const Sink& sink) { m_sink = sink; }
    void SetTiming(const RemoteStateTrack::Timing& timing) { m_timing = timing; }
    // A driver outside our context (level / interior) has no ghost machine.
    typedef std::function<bool(int playerId)> VisibleFn;
    void SetVisibility(VisibleFn fn) { m_visible = std::move(fn); }

    // Ingest one remote driver's latest state (with its server sample time).
    // Spawns a ghost when the driver is in a machine, despawns it when the
    // driver gets out or the machine changes.
    void SyncDriver(int playerId, const PlayerNetworkState& state, uint32_t serverTimeMs = 0);

    // Removes the ghost of one driver (e.g. a remote player left).
    void ClearDriver(int playerId);

    // Despawns the ghost of any remote player no longer reported by the
    // authoritative player set. Call each frame after driving SyncDriver.
    void Prune(const RemotePlayerManager& players);

    // Removes all ghosts (e.g. leaving the match).
    void Clear();

    // Moves every ghost to its interpolated pose (call after SyncDriver).
    void Update(float dt);

    // The remote driver of ghost machine `handle` (0 if none).
    int FindDriver(const void* handle) const;

private:
    struct Entry
    {
        void* handle = nullptr;
        char model[16] = { 0 };
        int netVehicleId = 0;
        RemoteStateTrack track;
    };

    void SpawnFor(int playerId, const PlayerNetworkState& state, uint32_t serverTimeMs);
    void DespawnIt(int playerId, Entry& entry);

    Sink m_sink;
    RemoteStateTrack::Timing m_timing;
    VisibleFn m_visible;
    std::map<int, Entry> m_entries;

    RemoteVehicleWorldView(const RemoteVehicleWorldView&);
    RemoteVehicleWorldView& operator=(const RemoteVehicleWorldView&);
};
