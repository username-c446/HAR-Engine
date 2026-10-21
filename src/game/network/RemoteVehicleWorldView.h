#pragma once

#include "PlayerNetworkState.h"
#include "RemotePlayerManager.h"

#include <map>
#include <cstring>

// Bridges an online match's remote players' vehangleModel state (inVehicle +
// vehicleModel + netVehicleId) to a world-side "ghost" machine.
//
// Only the data-level concern lives here: knowing when a remote driver entered
// or left the same machine, and telling the world sink to spawn a vehicle of
// the reported model. The actual in-world vehicle creation/movement is done by
// the sink, so this class stays unit-testable.
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
    void SetSmoothing(float seconds) { m_smoothingSeconds = seconds; }

    // Ingest one remote driver's latest state. Spawns a ghost vehicle when the
    // driver reports being in a machine, moves it when driving, despawns it when
    // the driver exits or the model changes.
    void SyncDriver(int playerId, const PlayerNetworkState& state);

    // Removes every ghost (e.g. a remote player left).
    void ClearDriver(int playerId);

    // Despawns the ghost of any remote player no longer reported by the
    // authoritative player set. Call each frame after driving SyncDriver.
    void Prune(const RemotePlayerManager& players);

    // Removes all ghosts (e.g. leaving the match).
    void Clear();

    // Advances ghost positions smoothly toward their targets (call after Sync).
    void Update(float dt);

private:
    struct Entry
    {
        void* handle;
        char model[16];
        int netVehicleId;
        float targetX, targetY, targetZ;
        float visualX, visualY, visualZ;
        bool smoothingApplied;
    };

    void SpawnFor(int playerId, const PlayerNetworkState& state);
    void DespawnIt(int playerId, Entry& entry);

    Sink m_sink;
    std::map<int, Entry> m_entries;
    float m_smoothingSeconds;

    RemoteVehicleWorldView(const RemoteVehicleWorldView&);
    RemoteVehicleWorldView& operator=(const RemoteVehicleWorldView&);
};