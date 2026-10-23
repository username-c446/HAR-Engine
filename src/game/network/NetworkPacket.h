#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <enet/enet.h>
#include <cstdint>
#include <cstring>

// Packet payloads are sent over ENet. ENet provides ordering, reliability and
// connection management, so our wire format only needs a type tag + payload.
namespace net
{
    enum PacketType : uint8_t
    {
        PACKET_HELLO = 0,
        PACKET_WELCOME = 1,
        PACKET_PLAYER_STATE = 2,
        PACKET_DISCONNECT = 3,
        PACKET_PLAYER_LIST = 4, // server-authoritative list of present entities
        PACKET_VEHICLE_STATE = 5, // per-driver vehicle transform (relayed like player state)
        PACKET_PING = 6,          // client heartbeat; server treats it as liveness
        PACKET_PONG = 7,          // server acknowledgement of a ping
        PACKET_AUTH = 8,          // client->server: password for a password-protected host
        PACKET_MISSION_STATE = 9, // per-player mission progress (relayed like player state)
        PACKET_WORLD_SEED = 10,   // server->client: authoritative world seed (u32)
    };

    // A client is considered alive if it has been heard from within this window.
    // The server silently drops (gracefully disconnects) peers that hit the limit.
    static constexpr const unsigned int CLIENT_TIMEOUT_MS = 8000;
    static constexpr const unsigned int PING_INTERVAL_MS = 2000;

    // ENet channel used for gameplay/reliable game packets.
    static constexpr const enet_uint8 GAME_CHANNEL = 0;

    static constexpr const size_t MAX_PACKET_SIZE = 512;

    // ---- Little-endian safe writers / readers (memcpy-free, no padding bugs) ----

    inline void WriteU16(char*& out, uint16_t v)
    {
        unsigned char* p = reinterpret_cast<unsigned char*>(out);
        p[0] = static_cast<unsigned char>(v & 0xFF);
        p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
        out += 2;
    }

    inline void WriteU32(char*& out, uint32_t v)
    {
        unsigned char* p = reinterpret_cast<unsigned char*>(out);
        p[0] = static_cast<unsigned char>(v & 0xFF);
        p[1] = static_cast<unsigned char>((v >> 8) & 0xFF);
        p[2] = static_cast<unsigned char>((v >> 16) & 0xFF);
        p[3] = static_cast<unsigned char>((v >> 24) & 0xFF);
        out += 4;
    }

    inline void WriteFloat(char*& out, float f)
    {
        uint32_t bits;
        memcpy(&bits, &f, sizeof(uint32_t));
        WriteU32(out, bits);
    }

    inline uint16_t ReadU16(const char*& in)
    {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(in);
        uint16_t v = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
        in += 2;
        return v;
    }

    inline uint32_t ReadU32(const char*& in)
    {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(in);
        uint32_t v = static_cast<uint32_t>(p[0])
                   | (static_cast<uint32_t>(p[1]) << 8)
                   | (static_cast<uint32_t>(p[2]) << 16)
                   | (static_cast<uint32_t>(p[3]) << 24);
        in += 4;
        return v;
    }

    inline float ReadFloat(const char*& in)
    {
        uint32_t bits = ReadU32(in);
        float f;
        memcpy(&f, &bits, sizeof(uint32_t));
        return f;
    }

    // Server-authoritative player list payload format:
    //   uint16 count, then [uint16 clientId] repeated count times.
    // Writes the list into out; returns bytes written.
    inline size_t WritePlayerList(char* out, const uint16_t* ids, size_t count)
    {
        char* p = out;
        WriteU16(p, static_cast<uint16_t>(count));
        for (size_t i = 0; i < count; ++i)
        {
            WriteU16(p, ids[i]);
        }
        return static_cast<size_t>(p - out);
    }
}