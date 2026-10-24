#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <enet/enet.h>

// RAII guard ensuring ENet is initialized exactly once per process and
// deinitialized on the last release. Safe to nest across server+client.
namespace net
{
    class ENetInit
    {
    public:
        ENetInit()
        {
            if (s_refCount++ == 0) {
                enet_initialize();
            }
        }
        ~ENetInit()
        {
            if (--s_refCount == 0) {
                enet_deinitialize();
            }
        }
        ENetInit(const ENetInit&) = delete;
        ENetInit& operator=(const ENetInit&) = delete;

    private:
        static int s_refCount;
    };

    // Link setup of every connection (both ends). ENet's unreliable-packet
    // throttle drops a share of unreliable packets whenever an RTT sample is
    // above the previous one plus twice the variance: on a quiet link with a
    // 1-2 ms RTT that is plain jitter, and up to 7/8 of the stream (player /
    // entity samples) was discarded with no loss on the wire. The stream rates
    // are bounded by the engine itself (cadences, interest management, per-peer
    // budgets), so the throttle never decelerates: it stays at its maximum.
    inline void ConfigureLink(ENetPeer* peer)
    {
        if (peer != nullptr)
            enet_peer_throttle_configure(peer, ENET_PEER_PACKET_THROTTLE_INTERVAL,
                                         ENET_PEER_PACKET_THROTTLE_ACCELERATION, 0);
    }
}