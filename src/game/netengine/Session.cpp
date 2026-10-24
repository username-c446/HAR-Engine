#include "Session.h"

namespace net
{
    // One process-level Session instance for the game (see Session.h).
    static Session* s_session = nullptr;

    Session* Session::Create()
    {
        if (s_session == nullptr)
            s_session = new Session();
        return s_session;
    }
    Session* Session::Get()
    {
        return s_session;
    }
    void Session::Destroy()
    {
        delete s_session;
        s_session = nullptr;
    }

    Session::Session() = default;
    Session::~Session() { Leave(); }

    // Inbound processing for a CLIENT-side session: everything the server
    // pushes to us (WELCOME, WORLD_SEED, PLAYER_LIST) updates local state.
    namespace
    {
        // Pack/unpack helpers use the reserved-range ids defined in Packet.h.
        constexpr const PacketId kIdWelcome    = PID_WELCOME;
        constexpr const PacketId kIdSeed       = PID_WORLD_SEED;
        constexpr const PacketId kIdPlayerList = PID_PLAYER_LIST;
        constexpr const PacketId kIdState      = PID_PLAYER_STATE;
        constexpr const PacketId kIdAuth       = PID_AUTH;
        constexpr const PacketId kIdPing       = PID_PING;
        constexpr const PacketId kIdPong       = PID_PONG;
        constexpr const PacketId kIdUser       = PID_USER;

        inline void WriteU16Le(uint8_t* d, uint16_t v)
        {
            d[0] = static_cast<uint8_t>(v & 0xFF);
            d[1] = static_cast<uint8_t>((v >> 8) & 0xFF);
        }
        inline uint16_t ReadU16Le(const uint8_t* d)
        {
            return static_cast<uint16_t>(d[0]) | static_cast<uint16_t>(d[1]) << 8;
        }
    }

    bool Session::StartHost(uint16_t port, uint32_t worldSeed)
    {
        if (IsInMatch()) return false;

        auto server = std::make_unique<ServerEndpoint>();
        if (!server->Start(port, 32)) return false;

        auto client = std::make_unique<ClientEndpoint>();
        if (!client->Connect("127.0.0.1", port))
        {
            server->Stop();
            return false;
        }

        m_server    = std::move(server);
        m_client    = std::move(client);
        m_worldSeed = worldSeed;
        m_port      = port;
        m_selfId    = 0; // resolved on WELCOME
        m_peerRoster.clear();
        m_authenticated.clear();

        // A password-protected host's own loopback client must authenticate to
        // its own server like any other peer. Seed the client-side password
        // with the host password so the host can actually participate; the
        // actual PID_AUTH is delivered by SendAuthIfDue() once the loopback
        // connection is up (sending it here, before the handshake, could drop
        // the payload and strand the host).
        m_authSent = false;
        if (m_hostPassword[0] != '\0')
            SetClientPassword(m_hostPassword);

        // Server relay: any inbound state from a peer is validated (if a
        // validator is attached) and re-broadcast to all OTHER peers, prefixed
        // with the sender id so receivers know who it is.
        m_server->SetPacketSink([this](const ServerEndpoint::RawPacket& rp)
        {
            if (rp.id == kIdAuth)
            {
                // Client presents a password. A host without a password accepts
                // everyone implicitly; a protected host only welcomes a peer
                // whose presented password matches. A bad/missing password
                // drops the peer (disconnect), so an unauthenticated client is
                // never admitted to the world.
                char candidate[65];
                memset(candidate, 0, sizeof(candidate));
                size_t copyLen = rp.size < sizeof(candidate) - 1 ? rp.size
                                                                  : sizeof(candidate) - 1;
                if (copyLen) memcpy(candidate, rp.data, copyLen);
                candidate[copyLen] = 0;

                if (m_hostPassword[0] == '\0'
                    || (copyLen <= strlen(m_hostPassword)
                        && strcmp(candidate, m_hostPassword) == 0))
                {
                    m_authenticated.insert(rp.from);
                    // Now the peer is allowed in: welcome + seed + roster.
                    uint8_t welcome[2];
                    WriteU16Le(welcome, rp.from);
                    m_server->SendTo(rp.from, kIdWelcome, welcome, sizeof(welcome));

                    uint8_t seed[4];
                    seed[0] = static_cast<uint8_t>(m_worldSeed & 0xFF);
                    seed[1] = static_cast<uint8_t>((m_worldSeed >> 8) & 0xFF);
                    seed[2] = static_cast<uint8_t>((m_worldSeed >> 16) & 0xFF);
                    seed[3] = static_cast<uint8_t>((m_worldSeed >> 24) & 0xFF);
                    m_server->SendTo(rp.from, kIdSeed, seed, sizeof(seed));

                    m_peerRoster.insert(rp.from);
                    if (m_joinLeave) m_joinLeave(rp.from, true);

                    std::vector<uint8_t> list(2 + m_peerRoster.size() * 2, 0);
                    WriteU16Le(list.data(), static_cast<uint16_t>(m_peerRoster.size()));
                    uint16_t idx = 0;
                    for (PeerId id : m_peerRoster)
                    {
                        WriteU16Le(list.data() + 2 + idx * 2, id);
                        ++idx;
                    }
                    m_server->Broadcast(kIdPlayerList, list.data(),
                                        static_cast<uint16_t>(list.size()), 0);
                }
                else
                {
                    // Wrong/missing password: reject outright by kicking the
                    // peer. It was never added to the roster, so it will never
                    // receive a WELCOME/seed/list; forcing the disconnect makes
                    // the rejection decisive and immediate.
                    m_server->KickPeer(rp.from);
                }
                return;
            }
            if (rp.id == kIdUser)
            {
                // Open user-data relay: forward the opaque payload to every
                // OTHER peer (server-authoritative, mirrors the state relay).
                // The server does not interpret the bytes.
                uint8_t framed[2 + MAX_PACKET_PAYLOAD];
                if (rp.size + 2 > sizeof(framed)) return;
                WriteU16Le(framed, rp.from);
                std::memcpy(framed + 2, rp.data, rp.size);
                m_server->Broadcast(kIdUser, framed, static_cast<uint16_t>(rp.size + 2), rp.from);
                return;
            }
            if (rp.id == kIdState)
            {
                // Server-authoritative gate: a validator may reject the state
                // (e.g. teleport detection). Unvalidated/simple relays pass all.
                if (m_validator && !m_validator->ValidatePeerState(rp.from, rp.data, rp.size))
                    return; // drop this state; keep last accepted authoritative

                // Build [sender u16][payload ...] and broadcast to every peer
                // except the sender.
                uint8_t framed[2 + MAX_PACKET_PAYLOAD];
                if (rp.size + 2 > sizeof(framed)) return;
                WriteU16Le(framed, rp.from);
                std::memcpy(framed + 2, rp.data, rp.size);
                m_server->Broadcast(kIdState, framed, static_cast<uint16_t>(rp.size + 2), rp.from);
                // Also deliver to our own world sink? The host sees other
                // players through its loopback client — it will receive the
                // relayed copy there. So we do NOT feed the sink directly.
            }
        });

        // Server side: on connect, admit the peer only if the session is open
        // (no password). A password-protected host defers the WELCOME/seed/list
        // until the client presents a matching PID_AUTH (handled in the packet
        // sink above). This keeps the admission server-authoritative: an
        // unauthenticated peer is never told who else is in the match.
        m_server->SetPeerEventFn([this](const PeerEvent& e)
        {
            if (!e.connected)
            {
                m_peerRoster.erase(e.peer);
                m_authenticated.erase(e.peer);
                if (m_joinLeave) m_joinLeave(e.peer, false);
                return;
            }
            // Open host: welcome immediately. Protected host: wait for PID_AUTH.
            if (m_hostPassword[0] != '\0')
            {
                // Peer connected but must authenticate. Do NOT welcome yet.
                return;
            }

            uint8_t welcome[2];
            WriteU16Le(welcome, e.peer);
            m_server->SendTo(e.peer, kIdWelcome, welcome, sizeof(welcome));

            uint8_t seed[4];
            seed[0] = static_cast<uint8_t>(m_worldSeed & 0xFF);
            seed[1] = static_cast<uint8_t>((m_worldSeed >> 8) & 0xFF);
            seed[2] = static_cast<uint8_t>((m_worldSeed >> 16) & 0xFF);
            seed[3] = static_cast<uint8_t>((m_worldSeed >> 24) & 0xFF);
            m_server->SendTo(e.peer, kIdSeed, seed, sizeof(seed));

            m_peerRoster.insert(e.peer);
            if (m_joinLeave) m_joinLeave(e.peer, true);

            std::vector<uint8_t> list(2 + m_peerRoster.size() * 2, 0);
            WriteU16Le(list.data(), static_cast<uint16_t>(m_peerRoster.size()));
            uint16_t idx = 0;
            for (PeerId id : m_peerRoster)
            {
                WriteU16Le(list.data() + 2 + idx * 2, id);
                ++idx;
            }
            m_server->Broadcast(kIdPlayerList, list.data(), static_cast<uint16_t>(list.size()), 0);
        });

        // Client side: handle WELCOME/SEED/PLAYER_LIST/STATE from the server.
        m_client->SetPacketSink([this](const ClientEndpoint::RawPacket& rp)
        {
            if (rp.id == kIdWelcome && rp.size >= 2)
            {
                m_selfId = ReadU16Le(rp.data);
            }
            else if (rp.id == kIdSeed && rp.size >= 4)
            {
                m_worldSeed = static_cast<uint32_t>(rp.data[0])
                            | static_cast<uint32_t>(rp.data[1]) << 8
                            | static_cast<uint32_t>(rp.data[2]) << 16
                            | static_cast<uint32_t>(rp.data[3]) << 24;
            }
            else if (rp.id == kIdPlayerList && rp.size >= 2)
            {
                uint16_t count = ReadU16Le(rp.data);
                if (rp.size >= 2 + count * 2)
                {
                    m_peerRoster.clear();
                    for (uint16_t k = 0; k < count; ++k)
                    {
                        PeerId id = ReadU16Le(rp.data + 2 + k * 2);
                        if (id != m_selfId) m_peerRoster.insert(id);
                    }
                }
            }
            if (rp.id == kIdUser && rp.size >= 2)
            {
                // Relayed user event: [sender u16][payload]. Forward to the
                // game's custom-event sink (self-echo already omitted by the
                // server's omit=sender broadcast).
                PeerId from = ReadU16Le(rp.data);
                if (from != m_selfId && m_customEvent)
                    m_customEvent(from, rp.data + 2, static_cast<uint16_t>(rp.size - 2));
            }
            else if (rp.id == kIdState && rp.size >= 2 + 1)
            {
                PeerId from = ReadU16Le(rp.data);
                if (from != m_selfId)               // ignore echo of ourselves
                    HandleRemoteState(from, rp.data + 2, static_cast<uint16_t>(rp.size - 2));
            }
        });

        return true;
    }

    bool Session::Join(const char* serverIp, uint16_t port)
    {
        if (IsInMatch()) return false;

        auto client = std::make_unique<ClientEndpoint>();
        if (!client->Connect(serverIp, port)) return false;

        m_client = std::move(client);
        m_port = port;
        m_worldSeed = 0;
        m_selfId = 0;
        m_peerRoster.clear();
        m_authSent = false;

        // The client-side password (SetPassword) is presented the moment the
        // ENet connection is up. We do NOT send it here: SendTo before the
        // handshake completes can drop the payload, leaving a protected host
        // unable to admit us. Delivery is deferred to Update() via
        // SendAuthIfDue(), which fires once IsPeerReady() is true.

        m_client->SetPacketSink([this](const ClientEndpoint::RawPacket& rp)
        {
            if (rp.id == kIdWelcome && rp.size >= 2)
                m_selfId = ReadU16Le(rp.data);
            else if (rp.id == kIdSeed && rp.size >= 4)
                m_worldSeed = static_cast<uint32_t>(rp.data[0])
                            | static_cast<uint32_t>(rp.data[1]) << 8
                            | static_cast<uint32_t>(rp.data[2]) << 16
                            | static_cast<uint32_t>(rp.data[3]) << 24;
            else if (rp.id == kIdPlayerList && rp.size >= 2)
            {
                uint16_t count = ReadU16Le(rp.data);
                if (rp.size >= 2 + count * 2)
                {
                    m_peerRoster.clear();
                    for (uint16_t k = 0; k < count; ++k)
                    {
                        PeerId id = ReadU16Le(rp.data + 2 + k * 2);
                        if (id != m_selfId) m_peerRoster.insert(id);
                    }
                }
            }
            else if (rp.id == kIdUser && rp.size >= 2)
            {
                PeerId from = ReadU16Le(rp.data);
                if (from != m_selfId && m_customEvent)
                    m_customEvent(from, rp.data + 2, static_cast<uint16_t>(rp.size - 2));
            }
            else if (rp.id == kIdState && rp.size >= 2 + 1)
            {
                PeerId from = ReadU16Le(rp.data);
                if (from != m_selfId)
                    HandleRemoteState(from, rp.data + 2, static_cast<uint16_t>(rp.size - 2));
            }
        });

        return true;
    }

    void Session::Leave()
    {
        if (m_client) m_client->Disconnect();
        if (m_server) m_server->Stop();
        m_client.reset();
        m_server.reset();
        m_selfId = 0;
        m_worldSeed = 0;
        m_port = 0;
        m_peerRoster.clear();
        m_joinLeave = nullptr;
    }

    void Session::Update()
    {
        if (m_server) m_server->Update();
        if (m_client) m_client->Update();

        SendAuthIfDue();
        SendLocalStateIfDue();
    }

    void Session::SendAuthIfDue()
    {
        if (m_authSent) return;
        if (!m_client || !m_client->IsPeerReady()) return; // connection not up yet
        if (m_clientPassword[0] == '\0') { m_authSent = true; return; } // nothing to present

        uint8_t payload[65];
        size_t n = strlen(m_clientPassword);
        if (n > sizeof(payload)) n = sizeof(payload);
        memcpy(payload, m_clientPassword, n);
        m_client->SendTo(kIdAuth, payload, static_cast<uint16_t>(n));
        m_authSent = true;
    }

    void Session::SendLocalStateIfDue()
    {
        if (!m_localState) return;
        if (!IsJoined()) return;

        if (++m_stateTick < m_stateSendEvery) return;
        m_stateTick = 0;

        uint16_t size = 0;
        if (!m_localState->GetLocalState(m_stateScratch, sizeof(m_stateScratch), size))
            return;
        if (size == 0) return;

        // Ship our state whenever we have a peer. Broadcast excludes nobody for
        // simplicity (self-echo filtered on the sink by peer id).
        if (m_client && m_client->SendTo(kIdState, m_stateScratch, size))
            return; // client-session: sent to the server (which relays)
        if (m_server) m_server->Broadcast(kIdState, m_stateScratch, size, 0);
    }

    bool Session::SendCustomEvent(const uint8_t* data, uint16_t size)
    {
        if (size > MAX_PACKET_PAYLOAD) return false;
        // Ship to the server (which relays to everyone else), or directly to
        // all peers if we are the host.
        if (m_client && m_client->SendTo(kIdUser, data, size))
            return true;
        if (m_server) { m_server->Broadcast(kIdUser, data, size, 0); return true; }
        return false;
    }

    void Session::HandleRemoteState(PeerId from, const uint8_t* data, uint16_t size)
    {
        if (m_remoteSink) m_remoteSink->ApplyRemoteState(from, data, size);
    }

    void Session::SetHostPassword(const char* password)
    {
        memset(m_hostPassword, 0, sizeof(m_hostPassword));
        if (password != nullptr)
        {
            size_t n = strlen(password);
            if (n > sizeof(m_hostPassword) - 1) n = sizeof(m_hostPassword) - 1;
            memcpy(m_hostPassword, password, n);
            m_hostPassword[n] = '\0';
        }
        // A freshly configured password only applies to future matches; an
        // active host keeps its current admission state (the roster is already
        // built). Callers set this before StartHost().
    }

    void Session::SetClientPassword(const char* password)
    {
        memset(m_clientPassword, 0, sizeof(m_clientPassword));
        if (password != nullptr)
        {
            size_t n = strlen(password);
            if (n > sizeof(m_clientPassword) - 1) n = sizeof(m_clientPassword) - 1;
            memcpy(m_clientPassword, password, n);
            m_clientPassword[n] = '\0';
        }
    }
}