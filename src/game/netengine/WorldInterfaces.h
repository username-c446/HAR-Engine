#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Packet.h"

#include <cstdint>

namespace net
{
    // -------------------------------------------------------------------
    // World-attachment interfaces.
    //
    // net::Session deliberately knows nothing about Avatar/Character/world
    // objects. The game (or another engine built on top) provides implementers
    // of these two small interfaces to bridge the session to its own world.
    //
    // Keeping world sync behind these interfaces is what makes the network
    // layer reusable: the transport/session logic is identical for any game,
    // and the game-specific replication lives in the implementer.
    // -------------------------------------------------------------------

    // What the local world wants to publish about itself, and what it accepts
    // about remote players. Both sides exchange state blobs whose wire format is
    // up to the developer (a struct + serialize/deserialize pair); the session
    // only carries the bytes opaquely.

    // ILocalStateProvider: the session asks "what is the current local state
    // to send to peers?" Implementers read their live avatar/vehicle and fill
    // `out`. Returns false if there is no state to send yet.
    class ILocalStateProvider
    {
    public:
        virtual ~ILocalStateProvider() = default;
        virtual bool GetLocalState(uint8_t* out, uint16_t capacity, uint16_t& size) = 0;
    };

    // IRemoteStateSink: the session delivers a state blob that arrived from a
    // remote peer. Implementers spawn/move/despawn the visual representation.
    class IRemoteStateSink
    {
    public:
        virtual ~IRemoteStateSink() = default;
        // before=false = peer left the match (cleanup).
        virtual void ApplyRemoteState(PeerId from, const uint8_t* data, uint16_t size) = 0;
        virtual void OnPeerLeft(PeerId peer) = 0;
    };

    // IStateValidator: optional server-authoritative plausibility gate for
    // relayed state. The session invokes this on every inbound player state
    // from a remote peer BEFORE broadcasting it to the others. Returning false
    // drops the state (so a teleporting/cheating client is not relayed) while
    // keeping the last accepted state authoritative. This hook is world-agnostic:
    // the transport/session only asks "is this blob plausible?"; interpreting
    // the bytes (position, speed, teleport heuristics) is the implementer's job.
    class IStateValidator
    {
    public:
        virtual ~IStateValidator() = default;
        // `from` is the origin peer, `data/size` the raw serialized state blob.
        // The validator may track per-peer history to detect teleports.
        // Returns true if the state may be relayed, false to drop it.
        virtual bool ValidatePeerState(PeerId from, const uint8_t* data, uint16_t size) = 0;
    };

    // -------------------------------------------------------------------
    // Dedicated mission-progress channel (PID_MISSION_STATE).
    //
    // Mission progress is small (~13 bytes) and slow-moving compared to the
    // per-frame player transform, so it rides its own packet id + cadence
    // instead of bloating every transform snapshot. The same relay semantics as
    // player state apply: the server relays a framed [sender][payload] to all
    // other peers (omitting the sender), and every session (including the host,
    // through its loopback client) delivers remote blobs to the sink.
    // -------------------------------------------------------------------

    // ILocalMissionProvider: the session asks "what is the local mission
    // progress to send to peers?" Fills `out` with the serialized blob (the
    // game's MissionSyncState bytes). Returns false when there is nothing to
    // send yet.
    class IMissionStateProvider
    {
    public:
        virtual ~IMissionStateProvider() = default;
        virtual bool GetMissionState(uint8_t* out, uint16_t capacity, uint16_t& size) = 0;
    };
}