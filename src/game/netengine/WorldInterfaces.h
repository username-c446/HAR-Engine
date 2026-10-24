#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Packet.h"

#include <cstdint>
#include <vector>

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
    // `serverTimeMs` is the server-clock time the sender sampled the state at
    // (for interpolation buffers).
    class IRemoteStateSink
    {
    public:
        virtual ~IRemoteStateSink() = default;
        virtual void ApplyRemoteState(PeerId from, const uint8_t* data, uint16_t size, uint32_t serverTimeMs) = 0;
        // The peer left the match (cleanup).
        virtual void OnPeerLeft(PeerId peer) = 0;
    };

    // IStateValidator: optional server-authoritative plausibility gate for
    // relayed player state. The server asks it about every inbound state blob
    // BEFORE relaying it. The engine never interprets the bytes; the validator
    // (game code) decodes them and keeps its own per-peer history.
    //   SV_ACCEPT  - relay it
    //   SV_DROP    - do not relay (the last accepted state stays authoritative)
    //   SV_CORRECT - do not relay, and send `correction` (a state blob the
    //                client must snap to) as PID_STATE_CORRECTION
    // `serverNowMs` is the server's receive time (never the client's claim).
    enum StateVerdict : uint8_t
    {
        SV_ACCEPT  = 0,
        SV_DROP    = 1,
        SV_CORRECT = 2,
    };

    class IStateValidator
    {
    public:
        virtual ~IStateValidator() = default;
        virtual StateVerdict ValidatePeerState(PeerId from, const uint8_t* data, uint16_t size,
                                               uint32_t serverNowMs, std::vector<uint8_t>& correction) = 0;
        // The peer left: forget its history.
        virtual void OnPeerLeft(PeerId peer) { (void)peer; }
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