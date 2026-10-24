#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>
#include <cstring>

namespace net
{
    // Version of the on-wire protocol. Bumped on every incompatible change to a
    // packet layout. A client presents it in PID_HELLO; a server refuses any
    // other version (PID_REFUSE, REFUSE_PROTOCOL) instead of silently
    // misreading its packets.
    static constexpr const uint16_t PROTOCOL_VERSION = 2;

    // First field of PID_HELLO ('HARN'): tells an engine client from random
    // UDP traffic that happens to reach the port.
    static constexpr const uint32_t PROTOCOL_MAGIC = 0x4E524148u;

    // Hard cap for any single packet payload (one UDP datagram body). Kept small
    // so a packet fits one ENet packet without fragmentation concerns.
    static constexpr const uint16_t MAX_PACKET_PAYLOAD = 512;

    // Reserved packet-id ranges:
    //   0x0000-0x00FF  core transport (hello, ack, ping, auth, world sync)
    //   0x0100-0x01FF  gameplay relay (player/vehicle/mission state)
    //   0x0200-0x7FFF  server Lua events / mod-defined
    //   0x8000-0xFFFF  user/developer-defined custom packets
    using PacketId = uint16_t;

    enum ReservedPacketId : PacketId
    {
        PID_HELLO        = 0x0000, // C->S: {magic, protocol, game version, password}
        PID_WELCOME      = 0x0001, // S->C: {peer id, server time, session options}
        PID_REFUSE       = 0x0003, // S->C: {reason, server protocol} then disconnect
        PID_PLAYER_LIST  = 0x0004, // S->C: authoritative present-peer list
        PID_PING         = 0x0005, // C->S: {client time} heartbeat / RTT probe
        PID_PONG         = 0x0006, // S->C: {client time echo, server time}
        PID_WORLD_SEED   = 0x0007, // S->C: authoritative world seed (u32)
        PID_SESSION_INFO = 0x0008, // S->C: session options changed {u32 options}
        PID_STATE_CORRECTION = 0x0009, // S->C: validator refused our state; snap to this blob

        // Server-authoritative replicated world state (WorldState.h).
        PID_WS_REQUEST      = 0x0010, // C->S: predicted set/erase {seq, key, op}
        PID_WS_UPDATE       = 0x0011, // S->all: accepted change {origin, seq, entry}
        PID_WS_REJECT       = 0x0012, // S->C: refused prediction + current entry
        PID_WS_ERASE        = 0x0013, // S->all: entry removed {origin, seq, key}
        PID_WS_SNAPSHOT     = 0x0014, // S->C: late-join snapshot chunk
        PID_WS_SNAPSHOT_END = 0x0015, // S->C: snapshot complete {total}

        // Server-authoritative counters / economy (Ledger.h).
        PID_LEDGER_REQUEST  = 0x0020, // C->S: {seq, counter, reason, delta, claim}
        PID_LEDGER_RESULT   = 0x0021, // S->C: {seq, counter, accepted, balance}

        PID_PLAYER_STATE = 0x0100, // relayed per-player transform
        PID_VEHICLE_STATE= 0x0101, // relayed remote driver's machine
        PID_MISSION_STATE= 0x0102, // relayed mission progress
        PID_ENTITY_STATE = 0x0103, // owner-streamed dynamic entity state (owner-checked relay)
        PID_ENTITY_EVENT = 0x0104, // C->S->owner: hit/impulse/damage aimed at an entity
        PID_PLAYER_EVENT = 0x0105, // C->S->victim: hit aimed at another player (PvP)

        // Open user-data channel in the reserved 0x8000+ range. The engine does
        // not interpret the payload: the session relays it to all other peers
        // (server-authoritative, like player state) and delivers a copy to the
        // game's custom-event sink. Games/mods use it to ship their own packets
        // without touching the core protocol.
        PID_USER         = 0x8000,
        // Same, unreliable-sequenced (stream channel): for frequent mod data
        // where only the newest packet matters.
        PID_USER_STREAM  = 0x8001,
    };

    // Why a server refused a client (PID_REFUSE). The client stays
    // disconnected and reports the reason (Session::GetRefusal).
    enum RefuseReason : uint8_t
    {
        REFUSE_NONE         = 0,
        REFUSE_PROTOCOL     = 1, // different PROTOCOL_VERSION / not an engine client
        REFUSE_GAME_VERSION = 2, // different game content version
        REFUSE_PASSWORD     = 3, // protected session, wrong or missing password
        REFUSE_FULL         = 4, // no free slot
        REFUSE_HANDSHAKE    = 5, // no HELLO within the handshake window
        REFUSE_FLOOD        = 6, // kicked for exceeding the rate limits
        REFUSE_DENIED       = 7, // the server's admission filter (mod) said no
    };

    // ENet channels. Reliable traffic (handshake, world state, ledger, events)
    // is ordered on its own channel; frequent latest-state streams ride an
    // unreliable-sequenced channel, so a lost stream packet is simply replaced
    // by the next one instead of stalling everything behind a retransmit.
    // Ping/pong get their own unreliable channel: a reliable retransmit would
    // inflate the RTT sample, and a shared sequenced channel would drop pings.
    enum Channel : uint8_t
    {
        CH_RELIABLE = 0,
        CH_STREAM   = 1,
        CH_TIME     = 2,
        CH_COUNT    = 3,
    };

    // Single delivery policy for every packet id, used by both endpoints.
    inline Channel ChannelOf(PacketId id)
    {
        switch (id)
        {
        case PID_PING:
        case PID_PONG:
            return CH_TIME;
        case PID_PLAYER_STATE:
        case PID_VEHICLE_STATE:
        case PID_MISSION_STATE:
        case PID_ENTITY_STATE:
        case PID_USER_STREAM:
            return CH_STREAM;
        default:
            return CH_RELIABLE;
        }
    }

    // Opaque identifier of a connected peer (assigned by the transport).
    using PeerId = uint16_t;

    // PacketId sanity: must fit the ranges above.
    static_assert(PID_HELLO < 0x0100, "core packet ids must stay in reserved range");
    static_assert(PID_MISSION_STATE >= 0x0100 && PID_MISSION_STATE < 0x0200,
                  "gameplay ids must stay in reserved range");
    static_assert(PID_PLAYER_EVENT < 0x0200, "gameplay ids must stay in reserved range");

    // ---------------------------------------------------------------------
    // net::Reader / net::Writer: bounds-checked little-endian serializers.
    //
    // The wire format is always little-endian and positional. Inputs are
    // validated so a malformed or truncated packet can never overrun the buffer
    // (a hard requirement for a reusable engine layer).
    // ---------------------------------------------------------------------

    class Writer
    {
    public:
        Writer(uint8_t* dst, uint16_t capacity)
            : m_dst(dst), m_capacity(capacity), m_pos(0), m_failed(false) {}

        bool WriteU8(uint8_t v)
        {
            if (m_pos + 1 > m_capacity) { m_failed = true; return false; }
            m_dst[m_pos++] = v;
            return true;
        }
        bool WriteU16(uint16_t v)
        {
            if (m_pos + 2 > m_capacity) { m_failed = true; return false; }
            m_dst[m_pos]     = static_cast<uint8_t>(v & 0xFF);
            m_dst[m_pos + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            m_pos += 2;
            return true;
        }
        bool WriteU32(uint32_t v)
        {
            if (m_pos + 4 > m_capacity) { m_failed = true; return false; }
            m_dst[m_pos]     = static_cast<uint8_t>(v & 0xFF);
            m_dst[m_pos + 1] = static_cast<uint8_t>((v >> 8) & 0xFF);
            m_dst[m_pos + 2] = static_cast<uint8_t>((v >> 16) & 0xFF);
            m_dst[m_pos + 3] = static_cast<uint8_t>((v >> 24) & 0xFF);
            m_pos += 4;
            return true;
        }
        bool WriteFloat(float f)
        {
            uint32_t bits;
            std::memcpy(&bits, &f, sizeof(bits));
            return WriteU32(bits);
        }
        bool WriteBytes(const void* src, uint16_t count)
        {
            if (m_pos + count > m_capacity) { m_failed = true; return false; }
            std::memcpy(m_dst + m_pos, src, count);
            m_pos += count;
            return true;
        }

        uint16_t Position() const { return m_pos; }
        // False once ANY write has overrun the buffer. A writer that hit the
        // limit is unusable for a well-formed packet, so callers must treat
        // !Ok() as "do not send, buffer corrupted".
        bool     Ok() const      { return !m_failed && m_pos <= m_capacity; }

    private:
        uint8_t* m_dst;
        uint16_t m_capacity;
        uint16_t m_pos;
        bool     m_failed;
    };

    class Reader
    {
    public:
        Reader(const uint8_t* src, uint16_t size)
            : m_src(src), m_size(size), m_pos(0) {}

        bool ReadU8(uint8_t& v)
        {
            if (m_pos + 1 > m_size) return false;
            v = m_src[m_pos++];
            return true;
        }
        bool ReadU16(uint16_t& v)
        {
            if (m_pos + 2 > m_size) return false;
            v = static_cast<uint16_t>(m_src[m_pos])
              | static_cast<uint16_t>(m_src[m_pos + 1]) << 8;
            m_pos += 2;
            return true;
        }
        bool ReadU32(uint32_t& v)
        {
            if (m_pos + 4 > m_size) return false;
            v = static_cast<uint32_t>(m_src[m_pos])
              | static_cast<uint32_t>(m_src[m_pos + 1]) << 8
              | static_cast<uint32_t>(m_src[m_pos + 2]) << 16
              | static_cast<uint32_t>(m_src[m_pos + 3]) << 24;
            m_pos += 4;
            return true;
        }
        bool ReadFloat(float& f)
        {
            uint32_t bits;
            if (!ReadU32(bits)) return false;
            std::memcpy(&f, &bits, sizeof(f));
            return true;
        }
        bool ReadBytes(void* dst, uint16_t count)
        {
            if (m_pos + count > m_size) return false;
            std::memcpy(dst, m_src + m_pos, count);
            m_pos += count;
            return true;
        }

        uint16_t Position() const { return m_pos; }
        uint16_t Remaining() const { return m_size - m_pos; }

    private:
        const uint8_t* m_src;
        uint16_t m_size;
        uint16_t m_pos;
    };
}