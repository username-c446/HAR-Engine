#pragma once

#include "../netengine/Packet.h"

#include <cstdint>
#include <functional>

// WorldEvent
// ----------
// A typed, lightweight world-sync event carried over the open PID_USER channel
// (net::Session::SendCustomEvent). The engine net:: layer treats the payload as
// opaque bytes; this wrapper adds a stable type tag + origin so game systems
// (and host-authoritative logic) can broadcast shared-world changes — broken
// props, vehicle destruction, door/stateprop transitions, coin collection —
// without coupling the transport to game types.
//
// Wire format (little-endian):
//   [ u8  eventType ]
//   [ u8  reserved  ]
//   [ u16 originPeerId ]
//   [ u16 payloadSize ]
//   [ payloadSize bytes ]
//
// Max payload such that the whole event fits MAX_PACKET_PAYLOAD.
namespace net
{
    // Game/world event type tags. Extend as new systems are wired up; keep
    // values stable once released so older clients can skip unknown events.
    enum WorldEventType : uint8_t
    {
        WE_NONE        = 0x00,
        WE_STATEPROP   = 0x01, // StatePropDSG m_CurrentState changed (door/gate/mechanism)
        WE_BREAKABLE   = 0x02, // BreakablesManager / breakable prop destroyed
        WE_VEHICLE_DESTROYED = 0x03, // a vehicle was destroyed (explosion/husk)
        WE_COIN_COLLECTED    = 0x04, // a world coin was picked up
        WE_PLATFORM_DIR      = 0x05, // animcollisionentity mfDirection change (platform/door)
    };

    // Bounds for serialized WorldEvent body (type+reserved+origin+size+payload).
    static constexpr const uint16_t WORLD_EVENT_HEADER_SIZE = 1 + 1 + 2 + 2;
    static_assert(WORLD_EVENT_HEADER_SIZE < MAX_PACKET_PAYLOAD, "header must fit a packet");

    class WorldEventCodec
    {
    public:
        // Serializes a world event into `out`; returns bytes written, or 0 if
        // the payload does not fit.
        static uint16_t Encode(uint8_t* out, uint16_t capacity,
                               WorldEventType type, uint16_t originPeer,
                               const uint8_t* payload, uint16_t payloadSize)
        {
            uint16_t total = WORLD_EVENT_HEADER_SIZE + payloadSize;
            if (total > capacity || total > MAX_PACKET_PAYLOAD) return 0;

            out[0] = static_cast<uint8_t>(type);
            out[1] = 0; // reserved
            out[2] = static_cast<uint8_t>(originPeer & 0xFF);
            out[3] = static_cast<uint8_t>((originPeer >> 8) & 0xFF);
            out[4] = static_cast<uint8_t>(payloadSize & 0xFF);
            out[5] = static_cast<uint8_t>((payloadSize >> 8) & 0xFF);
            if (payloadSize) {/* payload copied below */} 
            for (uint16_t i = 0; i < payloadSize; ++i)
                out[WORLD_EVENT_HEADER_SIZE + i] = payload[i];
            return total;
        }

        // Result of decoding an inbound world event.
        struct Decoded
        {
            WorldEventType type = WE_NONE;
            uint16_t       originPeer = 0;
            const uint8_t* payload = nullptr;
            uint16_t       payloadSize = 0;
            bool           valid = false;
        };

        // Callback type for inbound world events (NetworkMatchManager surface).
        using InboundFn = std::function<void(const Decoded*)>;

        static Decoded Decode(const uint8_t* data, uint16_t size)
        {
            Decoded d;
            if (size < WORLD_EVENT_HEADER_SIZE) return d;
            d.type        = static_cast<WorldEventType>(data[0]);
            d.originPeer  = static_cast<uint16_t>(data[2]) | static_cast<uint16_t>(data[3]) << 8;
            d.payloadSize = static_cast<uint16_t>(data[4]) | static_cast<uint16_t>(data[5]) << 8;
            if (WORLD_EVENT_HEADER_SIZE + d.payloadSize > size) return d;
            d.payload = data + WORLD_EVENT_HEADER_SIZE;
            d.valid   = true;
            return d;
        }
    };
}