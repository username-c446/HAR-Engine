#include "Packet.h"

namespace net
{
    // This translation unit hosts Packet-level helpers that need a .cpp (as
    // opposed to the inline Writer/Reader in Packet.h). The on-wire framing is:
    //
    //   [ PacketId u16 ][ payloadSize u16 ][ payload payloadSize bytes ]
    //
    // (all little-endian). Serializing a packet is therefore: write the id,
    // write the size, then the body. Keeping the framing here in one place
    // means a wire-format change touches exactly this file, not every sender.

    static constexpr const uint16_t FRAME_HEADER_BYTES = sizeof(PacketId) + sizeof(uint16_t);

    // Returns true if a datagram of `totalSize` bytes could hold a valid frame
    // with the given payload length, and (optionally) reports the payload start.
    bool FrameFits(uint16_t totalSize, uint16_t payloadSize)
    {
        return payloadSize <= MAX_PACKET_PAYLOAD
            && static_cast<uint32_t>(FRAME_HEADER_BYTES) + payloadSize <= totalSize;
    }

    uint16_t FrameOverhead() { return FRAME_HEADER_BYTES; }
}