#include "PacketRegistry.h"

namespace net
{
    bool PacketRegistry::Register(PacketId id, IPacketHandler* handler)
    {
        if (handler == nullptr) return false;
        m_handlers[id] = [handler](const InboundPacket& p) { handler->Handle(p); };
        return true;
    }

    bool PacketRegistry::Register(PacketId id, PacketHandlerFn fn)
    {
        if (!fn) return false;
        m_handlers[id] = std::move(fn);
        return true;
    }

    bool PacketRegistry::Dispatch(const InboundPacket& packet)
    {
        auto it = m_handlers.find(packet.id);
        if (it == m_handlers.end())
        {
            ++m_unknownDropped;
            return false;
        }
        it->second(packet);
        return true;
    }
}