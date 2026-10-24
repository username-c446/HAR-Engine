#pragma once

#include "RemotePlayerManager.h"
#include "RemoteStateTrack.h"

#include <functional>
#include <map>
#include <string.h>
#include <cstdint>

// Bridges an online match's remote-player states to the game world. It spawns
// and moves a lightweight visual representation for each remote player.
//
// Motion is snapshot interpolation (RemoteStateTrack): each received state is
// kept with its server sample time and the player is rendered behind the
// server clock, so it follows the sender's real path at any packet rate.
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
    // `motion`: the pose's world velocity (0 for untimed input).
    typedef void  (*MoveFn)(int playerId, void* handle, const PlayerNetworkState& state, const RemoteMotion& motion);
    typedef void  (*JumpFn)(int playerId, void* handle); // rising edge of a remote jump
    typedef void  (*ActionFn)(int playerId, void* handle, uint8_t actionId); // new one-shot ACTION_* id (sequence bits stripped)
    typedef void  (*DespawnFn)(int playerId, void* handle);
    // The player's appearance changed: restyle the puppet in place.
    typedef void  (*RestyleFn)(int playerId, void* handle);

    struct Sink
    {
        SpawnFn spawn;
        MoveFn move;
        JumpFn jump;
        ActionFn action;
        DespawnFn despawn;
        RestyleFn restyle;
    };

    // Presence (PlayerPresence): whether a player shares our context (level /
    // interior) and its appearance version. A player outside our context has
    // no puppet (its state is still tracked); a new appearance version restyles.
    typedef std::function<bool(int playerId)> VisibleFn;
    typedef std::function<uint32_t(int playerId)> AppearanceVersionFn;

    RemotePlayerWorldView();
    ~RemotePlayerWorldView();

    void SetSink(const Sink& sink) { m_sink = sink; }
    // Server clock + interpolation parameters (no clock: newest state shown).
    void SetTiming(const RemoteStateTrack::Timing& timing) { m_timing = timing; }
    void SetPresence(VisibleFn visible, AppearanceVersionFn appearance)
    {
        m_visible = std::move(visible);
        m_appearance = std::move(appearance);
    }

    // Called every frame with the authoritative set of remote players. Spawns
    // new players, records new samples, despawns removed ones.
    void Sync(const RemotePlayerManager& players);

    // Called every frame (after Sync): moves each remote player to its
    // interpolated pose and fires the jump / action edges the shown state
    // reaches. Nothing moves between spawns without Update().
    void Update(float dt);

    // Despawn everything (e.g. on leaving the match).
    void Clear();

    // The remote player whose puppet `handle` is (0 if none).
    int FindPlayer(const void* handle) const;

private:
    void SyncOne(int playerId, const PlayerNetworkState& state, uint32_t serverTimeMs);

    struct Entry
    {
        void*            handle = nullptr;   // null while outside our context
        uint32_t         appearanceVersion = 0;
        RemoteStateTrack track;
        bool             edgesPrimed = false; // first shown state seen (no replay)
        uint8_t          lastActionEvent = 0; // fires the action sink only on change
        bool             jumping = false;     // FLAG_JUMPING / FLAG_IN_AIR rising edge
    };

    void UpdatePresence(int playerId, Entry& entry, const PlayerNetworkState& state);
    void FireEdges(int playerId, Entry& entry, const PlayerNetworkState& shown);

    Sink m_sink;
    RemoteStateTrack::Timing m_timing;
    VisibleFn m_visible;
    AppearanceVersionFn m_appearance;
    std::map<int, Entry> m_entries;

    RemotePlayerWorldView(const RemotePlayerWorldView&);
    RemotePlayerWorldView& operator=(const RemotePlayerWorldView&);
};
