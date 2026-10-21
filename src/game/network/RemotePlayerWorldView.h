#pragma once

#include "RemotePlayerManager.h"

#include <map>
#include <string.h>
#include <cstdint>

// Bridges an online match's remote-player states to the game world. It spawns
// and moves a lightweight visual representation for each remote player.
//
// The world access is decoupled through a sink so the logic is unit-testable:
// the playing context wires the real sink (CharacterManager-based), tests wire
// a fake.
class RemotePlayerWorldView
{
public:
    // A sink that knows how to make a remote player visible/movable in-world.
    // playerId matches the network client id. Returns an opaque handle that is
    // passed back unmodified to later Move/Despawn calls.
    typedef void* (*SpawnFn)(int playerId);
    typedef void  (*MoveFn)(int playerId, void* handle, const PlayerNetworkState& state);
    typedef void  (*DespawnFn)(int playerId, void* handle);

    struct Sink
    {
        SpawnFn spawn;
        MoveFn move;
        DespawnFn despawn;
    };

    RemotePlayerWorldView();
    ~RemotePlayerWorldView();

    void SetSink(const Sink& sink) { m_sink = sink; }
    void SetSmoothing(float seconds) { m_smoothingSeconds = seconds; }

    // Called every frame with the authoritative set of remote players. Spawns
    // new players, moves changed ones, despawns removed ones.
    void Sync(const RemotePlayerManager& players);

    // Called every frame (after Sync) to advance the visual position of each
    // remote player smoothly toward its latest reported target. This removes
    // teleports caused by the refresh rate of network state packets.
    void Update(float dt);

    // Despawn everything (e.g. on leaving the match).
    void Clear();

private:
    void SyncOne(int playerId, const PlayerNetworkState& state);

    struct Entry
    {
        void* handle;
        float targetX, targetY, targetZ;
        float visualX, visualY, visualZ;
        bool smoothingApplied;
    };

    Sink m_sink;
    std::map<int, Entry> m_entries;
    float m_smoothingSeconds;

    RemotePlayerWorldView(const RemotePlayerWorldView&);
    RemotePlayerWorldView& operator=(const RemotePlayerWorldView&);
};