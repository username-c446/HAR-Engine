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
}