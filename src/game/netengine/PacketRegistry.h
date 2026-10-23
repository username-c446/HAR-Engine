#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "Packet.h"

#include <cstdint>
#include <map>
#include <functional>

namespace net
{
    // Opaque identifier of a connected peer (assigned by the transport).
// (Defined in Packet.h.)

// A fully decoded inbound packet. The reader view is already positioned at
    // the payload start; handlers read fields out of it.
    struct InboundPacket
    {
        PacketId      id;
        PeerId        from;      // sending peer
        Reader        body;      // bounds-checked payload reader

        InboundPacket(PacketId id_, PeerId from_, const uint8_t* data, uint16_t size)
            : id(id_), from(from_), body(data, size) {}
    };

    // Handler interface for one packet id. Implementations are registered in the
    // registry; the transport delivers packets to it. Because registration is
    // open, a developer (or a mod) can extend the protocol without touching the
    // core switch — this is the main extensibility point of the engine API.
    class IPacketHandler
    {
    public:
        virtual ~IPacketHandler() = default;
        virtual void Handle(const InboundPacket& packet) = 0;
    };

    // Simple functor-based handler for convenience / quick tests.
    using PacketHandlerFn = std::function<void(const InboundPacket&)>;

    // Routes inbound packets by id to their registered handler. Replaces the
    // monolithic switch historically used by NetworkServer/NetworkClient: adding
    // a packet is now "register a handler for id X", not "edit the switch".
    class PacketRegistry
    {
    public:
        PacketRegistry() = default;

        // Registers (or replaces) the handler for id. Returns false if the id is
        // outside the reserved range below (guard against accidental clash with
        // the protocol's own ids).
        bool Register(PacketId id, IPacketHandler* handler);
        bool Register(PacketId id, PacketHandlerFn fn);

        // Returns true if a packet with this id was delivered to a handler.
        // Unknown ids are dropped silently (counted) — the protocol is forward
        // tolerant: a newer peer may send an id we do not know yet.
        bool Dispatch(const InboundPacket& packet);

        unsigned GetUnknownDropped() const { return m_unknownDropped; }

    private:
        std::map<PacketId, PacketHandlerFn> m_handlers;
        unsigned m_unknownDropped = 0;
    };
}