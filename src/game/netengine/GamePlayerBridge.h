#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "WorldInterfaces.h"
#include "Packet.h"
// forward-declared (Session.h pulls in transport headers we don't need here)
namespace net { class Session; }

#include <cstdint>
#include <map>

// Forward declarations (game world side).
class RemotePlayerManager;       // authoritative set of remote players (interpolated)
struct PlayerNetworkState;       // game-side typed player state

namespace net
{
    // GamePlayerBridge
    // -----------------
    // Game-specific adapter between the engine's opaque net::Session and the
    // SRR2 world. The core Session only moves byte blobs; this adapter knows
    // about PlayerNetworkState and feeds the game's RemotePlayerWorldView /
    // RemoteVehicleWorldView.
    //
    // It implements ILocalStateProvider (publish local player) and
    // IRemoteStateSink (consume remote players) that Session calls. Because it
    // is game-specific it lives beside the engine modules but is not part of
    // the reusable transport/session core.
    class GamePlayerBridge : public ILocalStateProvider,
                             public IRemoteStateSink
    {
    public:
        GamePlayerBridge();
        ~GamePlayerBridge() override;

        // ---- wiring (called by GameplayContext) ------------------------------

        // Where to get the live local player state. The provider signature is
        // fixed by the engine; the game typically reads the Avatar each tick.
        void SetLocalProvider(ILocalStateProvider* p) { m_localSource = p; }

        // Game-typed local provider: reads the live Avatar into a
        // PlayerNetworkState. The bridge serializes it into the opaque blob the
        // engine session ships. Exactly one of the two providers is used.
        typedef bool (*TypedLocalProvider)(PlayerNetworkState& out);
        void SetTypedLocalProvider(TypedLocalProvider p) { m_typedLocal = p; }

        // The authoritative manager of remote players (the game's
        // RemotePlayerManager). Applied from incoming blobs.
        void SetRemotePlayers(RemotePlayerManager* p) { m_remotePlayers = p; }

        // The network peer id of ourselves; used to skip our own echo.
        void SetLocalPeerId(PeerId id) { m_selfId = id; }

        // ---- ILocalStateProvider ---------------------------------------------

        bool GetLocalState(uint8_t* out, uint16_t capacity, uint16_t& size) override;

        // ---- IRemoteStateSink ------------------------------------------------

        void ApplyRemoteState(PeerId from, const uint8_t* data, uint16_t size, uint32_t serverTimeMs) override;
        void OnPeerLeft(PeerId peer) override;

        // Wires this bridge into the engine session: the session pulls the
        // local player state (ILocalStateProvider) and delivers remote players
        // and their departures (IRemoteStateSink) to this bridge.
        void AttachToSession(Session* session);

    private:
        ILocalStateProvider* m_localSource = nullptr;
        TypedLocalProvider m_typedLocal = nullptr;   // game-typed Avatar reader
        RemotePlayerManager* m_remotePlayers = nullptr;
        PeerId m_selfId = 0;
        // Scratch for local serialization.
        uint8_t m_scratch[MAX_PACKET_PAYLOAD];
    };
}