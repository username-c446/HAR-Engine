#include "NetworkServer.h"
#include "NetworkPacket.h"
#include "ServerModManager.h"

namespace
{
    const int kMaxPlayers = 8;
}

NetworkServer::NetworkServer()
    : m_host(nullptr)
    , m_port(0)
    , m_nextClientId(1000)
    , m_sequence(0)
    , m_clientTimeoutMs(net::CLIENT_TIMEOUT_MS)
    , m_maxPlayers(kMaxPlayers)
    , m_requiresAuth(false)
{
    memset(m_password, 0, sizeof(m_password));
}

NetworkServer::~NetworkServer() { Stop(); }

bool NetworkServer::Start(unsigned short port)
{
    if (m_host) return false;
    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;
    m_host = enet_host_create(&address, m_maxPlayers, 1, 0, 0);
    if (!m_host) return false;
    m_port = port;
    return true;
}

void NetworkServer::SetPassword(const char* password)
{
    memset(m_password, 0, sizeof(m_password));
    if (password != nullptr)
    {
        strncpy(m_password, password, sizeof(m_password) - 1);
        m_requiresAuth = m_password[0] != '\0';
    }
    else
    {
        m_password[0] = '\0';
        m_requiresAuth = false;
    }
}

void NetworkServer::Stop()
{
    if (m_host) {
        enet_host_destroy(m_host);
        m_host = nullptr;
    }
    m_port = 0;
    m_peerIds.clear();
    m_peersById.clear();
    m_players.Clear();
}

void NetworkServer::Update()
{
    if (!m_host) return;
    ENetEvent event;
    while (enet_host_service(m_host, &event, 0) > 0) {
        switch (event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
        {
            unsigned short clientId = m_nextClientId++;
            m_peerIds[event.peer] = clientId;
            m_peersById[clientId] = event.peer;

            // Server-authoritative admission: a Lua mod may reject the join.
            if (m_mods.HasScript() && !m_mods.OnPlayerJoin(static_cast<int>(clientId)))
            {
                // Reject: drop session and disconnect the peer.
                m_peerIds.erase(event.peer);
                m_peersById.erase(clientId);
                enet_peer_disconnect(event.peer, 1);
                break;
            }

            // Server-authoritative session record.
            ClientSession session;
            session.peer = event.peer;
            session.lastStateTimeMs = enet_time_get();
            session.connectedAtMs = session.lastStateTimeMs;
            session.lastSeenMs = session.lastStateTimeMs;
            session.validated = false;
            session.authenticated = !m_requiresAuth;
            m_sessions[clientId] = session;

            // A password-protected host waits for PACKET_AUTH before welcoming.
            // Without a password the client is welcomed immediately.
            if (session.authenticated)
            {
                WelcomeClient(event.peer, clientId);
                // Broadcast the authoritative entity list (servers decide who is in the world).
                SendPlayerListToAll();
            }
            break;
        }
        case ENET_EVENT_TYPE_RECEIVE:
            HandlePacket(event.peer, reinterpret_cast<const char*>(event.packet->data), event.packet->dataLength);
            enet_packet_destroy(event.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
        {
            auto it = m_peerIds.find(event.peer);
            if (it != m_peerIds.end()) {
                if (m_mods.HasScript()) m_mods.OnPlayerLeave(static_cast<int>(it->second));
                m_players.RemovePlayer(static_cast<int>(it->second));
                m_sessions.erase(it->second);
                m_peersById.erase(it->second);
                m_peerIds.erase(it);
                // Reflect the authoritative entity change for everyone.
                SendPlayerListToAll();
            }
            break;
        }
        default:
            break;
        }
    }

    // Sweep for silent peers: a client that has not been heard from within the
    // timeout window is considered gone. Drop it gracefully and update the
    // authoritative entity list, so dead peers do not linger forever.
    if (!m_sessions.empty())
    {
        enet_uint32 nowMs = enet_time_get();
        for (auto it = m_sessions.begin(); it != m_sessions.end();)
        {
            ClientSession& sess = it->second;
            if (nowMs - sess.lastSeenMs > m_clientTimeoutMs)
            {
                unsigned short deadId = it->first;
                ENetPeer* deadPeer = sess.peer;
                if (m_mods.HasScript()) m_mods.OnPlayerLeave(static_cast<int>(deadId));
                m_players.RemovePlayer(static_cast<int>(deadId));
                m_peersById.erase(deadId);
                it = m_sessions.erase(it);
                if (deadPeer != nullptr)
                {
                    m_peerIds.erase(deadPeer);
                    enet_peer_disconnect(deadPeer, 0);
                }
                SendPlayerListToAll();
            }
            else
            {
                ++it;
            }
        }
    }
}

void NetworkServer::HandlePacket(ENetPeer* peer, const char* data, size_t size)
{
    if (size < 1) return;
    unsigned char type = static_cast<unsigned char>(data[0]);
    const char* payload = data + 1;
    size_t payloadSize = size - 1;

    auto it = m_peerIds.find(peer);
    if (it == m_peerIds.end()) return;
    unsigned short clientId = it->second;

    // Any packet from a client proves it is still alive. Refresh its liveness
    // timestamp so the timeout sweep does not drop a responsive peer.
    auto sit = m_sessions.find(clientId);
    if (sit != m_sessions.end())
    {
        sit->second.lastSeenMs = enet_time_get();
    }

    switch (type)
    {
    case net::PACKET_HELLO:
        // ENet connection is already established; nothing more needed server-side.
        break;
    case net::PACKET_PLAYER_STATE:
    {
        // Payload from client: [senderClientId u16][PlayerNetworkState].
        if (payloadSize < sizeof(unsigned short) + static_cast<size_t>(PlayerNetworkState::SERIALIZED_SIZE)) break;
        const char* p = payload;
        net::ReadU16(p); // client-sent clientId; we trust our authoritative mapping.
        PlayerNetworkState state;
        state.Deserialize(p);

        // Server-authoritative gate: only relay states that pass plausibility
        // checks (no teleporting, sane speeds). Invalid states are dropped.
        ValidateAndRelay(clientId, state);
        break;
    }
    case net::PACKET_PING:
        // Heartbeat acknowledgement. The liveness timestamp was refreshed above.
        SendTo(peer, net::PACKET_PONG, nullptr, 0, clientId);
        break;
    case net::PACKET_AUTH:
    {
        // Client presents a password: payload is a null-terminated string.
        // On success we welcome the client; on failure we drop it.
        if (sit == m_sessions.end()) break;
        ClientSession& sess = sit->second;
        if (sess.authenticated) break; // already in

        char candidate[65];
        memset(candidate, 0, sizeof(candidate));
        size_t copyLen = payloadSize < sizeof(candidate) - 1 ? payloadSize : sizeof(candidate) - 1;
        if (copyLen > 0) memcpy(candidate, payload, copyLen);
        candidate[copyLen] = '\0';

        if (m_requiresAuth && strcmp(candidate, m_password) == 0)
        {
            sess.authenticated = true;
            WelcomeClient(peer, clientId);
            SendPlayerListToAll();
        }
        else
        {
            // Wrong or missing password: reject.
            m_peerIds.erase(peer);
            m_peersById.erase(clientId);
            m_sessions.erase(clientId);
            enet_peer_disconnect(peer, 1);
        }
        break;
    }
    case net::PACKET_DISCONNECT:
        enet_peer_disconnect(peer, 0);
        break;
    default:
        break;
    }
}

void NetworkServer::WelcomeClient(ENetPeer* peer, unsigned short clientId)
{
    char out[sizeof(clientId)];
    char* p = out;
    net::WriteU16(p, clientId);
    SendTo(peer, net::PACKET_WELCOME, out, sizeof(clientId), clientId);
}

// Server-authoritative plausibility gate. Returns true if the state can be
// relayed, false if it looks like a teleport or impossibly fast movement.
// The server is the arbiter: last accepted state is kept per session, and a
// fresh state only passes if, in the elapsed time, the movement is physically
// plausible for the vehicle/on foot.
bool NetworkServer::ValidateAndRelay(unsigned short clientId, const PlayerNetworkState& proposed)
{
    auto sit = m_sessions.find(clientId);
    if (sit == m_sessions.end()) return false;

    ClientSession& session = sit->second;
    const PlayerNetworkState* lastState = m_players.GetPlayer(static_cast<int>(clientId));

    enet_uint32 nowMs = enet_time_get();
    enet_uint32 dtMs = nowMs - session.lastStateTimeMs;
    // Guard against huge clock jumps; if dt is absurd, accept and reset clock.
    if (dtMs > 5000) dtMs = 0;

    static const float kMaxSpeedOnFoot = 15.0f;      // ~54 km/h, generous for walking/running
    static const float kMaxSpeedInVehicle = 120.0f;  // ~430 km/h, well above the fastest car
    static const float kMaxTeleportMeters = 5.0f;    // below this we allow any move within dt

    if (lastState != NULL && dtMs > 0)
    {
        float dx = proposed.x - lastState->x;
        float dy = proposed.y - lastState->y;
        float dz = proposed.z - lastState->z;
        float dist = sqrtf(dx * dx + dy * dy + dz * dz);

        float seconds = static_cast<float>(dtMs) / 1000.0f;
        float speed = dist / (seconds > 0.0001f ? seconds : 0.0001f);

        float maxSpeed = (proposed.inVehicle != 0) ? kMaxSpeedInVehicle : kMaxSpeedOnFoot;

        // Teleport check: a large single jump without a plausible travel time.
        if (dist > kMaxTeleportMeters + maxSpeed * seconds)
        {
            // Drop this state; keep the previous hallucination-free position.
            return false;
        }
        if (speed > maxSpeed * 1.5f)
        {
            // Too fast for the purported mode of travel.
            return false;
        }
    }

    // Server-side Lua hook: a script may tighten validation further.
    if (m_mods.HasScript() && !m_mods.OnPlayerValidate(static_cast<int>(clientId), proposed))
    {
        return false;
    }

    // Accept: store as the trusted latest state, then relay to everyone else.
    m_players.UpdatePlayer(static_cast<int>(clientId), proposed);
    session.lastStateTimeMs = nowMs;
    session.validated = true;

    char relay[sizeof(unsigned short) + PlayerNetworkState::SERIALIZED_SIZE];
    char* rp = relay;
    net::WriteU16(rp, clientId);
    proposed.Serialize(rp);
    for (auto& kv : m_peerIds) {
        if (kv.first == session.peer) continue;
        SendTo(kv.first, net::PACKET_PLAYER_STATE, relay, sizeof(relay), clientId);
    }
    return true;
}

void NetworkServer::SendTo(ENetPeer* peer, unsigned char type, const void* payload, size_t payloadSize, unsigned short clientId)
{
    (void)clientId; // ENet peers are the identity; clientId rides in state payload.
    if (!m_host) return;
    ENetPacket* packet = enet_packet_create(nullptr, payloadSize + 1, ENET_PACKET_FLAG_RELIABLE);
    if (!packet) return;
    unsigned char* dst = packet->data;
    dst[0] = type;
    if (payloadSize > 0) memcpy(dst + 1, payload, payloadSize);
    enet_peer_send(peer, static_cast<enet_uint8>(net::GAME_CHANNEL), packet);
    enet_host_flush(m_host);
}

void NetworkServer::SendPlayerListToAll()
{
    if (!m_host) return;

    uint16_t ids[net::MAX_PACKET_SIZE / 2];
    size_t count = 0;
    for (auto& kv : m_peerIds) {
        if (count < (net::MAX_PACKET_SIZE / 2) - 1) {
            ids[count++] = kv.second;
        }
    }

    char buffer[1 + net::MAX_PACKET_SIZE];
    buffer[0] = net::PACKET_PLAYER_LIST;
    size_t payloadSize = net::WritePlayerList(buffer + 1, ids, count);

    for (auto& kv : m_peerIds) {
        ENetPacket* packet = enet_packet_create(nullptr, 1 + payloadSize, ENET_PACKET_FLAG_RELIABLE);
        if (!packet) return;
        packet->data[0] = net::PACKET_PLAYER_LIST;
        memcpy(packet->data + 1, buffer + 1, payloadSize);
        enet_peer_send(kv.first, static_cast<enet_uint8>(net::GAME_CHANNEL), packet);
    }
    enet_host_flush(m_host);
}