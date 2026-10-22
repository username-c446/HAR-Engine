#include "NetworkClient.h"
#include "NetworkPacket.h"
#include <cstring>

namespace
{
    const int kMaxHelloRetries = 5;
    const int kStateEveryFrames = 6; // ~10Hz at 60fps
}

namespace net
{
    int ENetInit::s_refCount = 0;
}

NetworkClient::NetworkClient()
    : m_host(nullptr)
    , m_server(nullptr)
    , m_serverPort(0)
    , m_connected(false)
    , m_serverReady(false)
    , m_clientId(0)
    , m_helloRetries(kMaxHelloRetries)
    , m_sequence(0)
    , m_lastPingMs(0)
    , m_pingIntervalMs(net::PING_INTERVAL_MS)
    , m_pingSentMs(0)
    , m_pingMs(0)
    , m_pingMeasured(false)
    , m_hasPassword(false)
{
    m_serverIp[0] = '\0';
    memset(m_password, 0, sizeof(m_password));
}

NetworkClient::~NetworkClient() { Disconnect(); }

bool NetworkClient::Connect(const char* serverIp, unsigned short serverPort)
{
    if (m_connected) return false;

    m_host = enet_host_create(nullptr, 1, 1, 0, 0);
    if (!m_host) return false;

    strncpy_s(m_serverIp, serverIp, _TRUNCATE);
    m_serverPort = serverPort;

    ENetAddress address;
    enet_address_set_host(&address, serverIp);
    address.port = serverPort;

    m_server = enet_host_connect(m_host, &address, 1, 0);
    if (!m_server) {
        enet_host_destroy(m_host);
        m_host = nullptr;
        return false;
    }

    m_connected = true;
    m_serverReady = false;
    m_helloRetries = kMaxHelloRetries;
    m_sequence = 0;
    m_lastPingMs = 0;
    return true;
}

void NetworkClient::Disconnect()
{
    if (m_host) {
        if (m_server) enet_peer_disconnect(m_server, 0);
        enet_host_destroy(m_host);
        m_host = nullptr;
        m_server = nullptr;
    }
    m_connected = false;
    m_serverReady = false;
    m_clientId = 0;
    m_remotePlayers.Clear();
    m_remoteMissionStates.clear();
}

void NetworkClient::Update()
{
    if (!m_connected || !m_host) return;

    ENetEvent event;
    while (enet_host_service(m_host, &event, 0) > 0) {
        switch (event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            SendHello();
            if (m_hasPassword) SendAuth(m_password);
            break;
        case ENET_EVENT_TYPE_RECEIVE:
            HandlePacket(reinterpret_cast<const char*>(event.packet->data), event.packet->dataLength);
            enet_packet_destroy(event.packet);
            break;
        case ENET_EVENT_TYPE_DISCONNECT:
            m_connected = false;
            m_serverReady = false;
            break;
        default:
            break;
        }
    }

    // Periodic heartbeat so the server knows we are still alive even between
    // state packets. Reliable but tiny; PONG confirms the link and lets us
    // measure round-trip latency.
    if (m_serverReady)
    {
        enet_uint32 nowMs = enet_time_get();
        if (nowMs - m_lastPingMs >= m_pingIntervalMs)
        {
            ENetPacket* packet = enet_packet_create(nullptr, 1, ENET_PACKET_FLAG_RELIABLE);
            if (packet)
            {
                packet->data[0] = net::PACKET_PING;
                enet_peer_send(m_server, static_cast<enet_uint8>(net::GAME_CHANNEL), packet);
                m_pingSentMs = nowMs;
            }
            m_lastPingMs = nowMs;
        }
    }
}

void NetworkClient::SendHello()
{
    if (!m_host || !m_server) return;
    if (m_helloRetries <= 0) return;
    --m_helloRetries;

    ENetPacket* packet = enet_packet_create(nullptr, 1, ENET_PACKET_FLAG_RELIABLE);
    if (!packet) return;
    packet->data[0] = net::PACKET_HELLO;
    enet_peer_send(m_server, static_cast<enet_uint8>(net::GAME_CHANNEL), packet);
    enet_host_flush(m_host);
}

void NetworkClient::SendAuth(const char* password)
{
    if (!m_host || !m_server) return;
    if (password == nullptr) return;
    size_t len = strnlen(password, 64);
    ENetPacket* packet = enet_packet_create(nullptr, 1 + len, ENET_PACKET_FLAG_RELIABLE);
    if (!packet) return;
    packet->data[0] = net::PACKET_AUTH;
    if (len > 0) memcpy(packet->data + 1, password, len);
    enet_peer_send(m_server, static_cast<enet_uint8>(net::GAME_CHANNEL), packet);
    enet_host_flush(m_host);
}

void NetworkClient::SetPassword(const char* password)
{
    memset(m_password, 0, sizeof(m_password));
    if (password != nullptr && password[0] != '\0')
    {
        strncpy(m_password, password, sizeof(m_password) - 1);
        m_hasPassword = true;
    }
    else
    {
        m_hasPassword = false;
    }
}

void NetworkClient::HandlePacket(const char* data, size_t size)
{
    if (size < 1) return;
    unsigned char type = static_cast<unsigned char>(data[0]);
    const char* payload = data + 1;
    size_t payloadSize = size - 1;

    switch (type)
    {
    case net::PACKET_WELCOME:
    {
        if (payloadSize < sizeof(unsigned short)) return;
        const char* p = payload;
        m_clientId = net::ReadU16(p);
        m_serverReady = true;
        m_helloRetries = 0;
        m_remotePlayers.Clear();
        m_authoritativePlayers.clear();
        m_prevAuthoritativePlayers.clear();
        m_pendingEvents.clear();
        m_remoteMissionStates.clear();
        break;
    }
    case net::PACKET_PLAYER_LIST:
    {
        // Server-authoritative entity list: rebuild the known-players set from
        // the server's list rather than inferring it from scattered state packets.
        if (payloadSize < sizeof(unsigned short)) return;
        const char* p = payload;
        unsigned short count = net::ReadU16(p);
        // Payload is [count u16][ids...]: 2 bytes for count + count*2 for ids.
        if (payloadSize < sizeof(unsigned short) + count * 2) return;

        std::vector<unsigned short> serverList;
        serverList.reserve(count);
        for (unsigned short i = 0; i < count; ++i) {
            unsigned short id = net::ReadU16(p);
            if (id == m_clientId) continue; // we are not a "remote" player to ourselves
            serverList.push_back(id);
        }

        // Removes players no longer present on the server, keep the rest.
        m_remotePlayers.KeepOnly(serverList);
        m_authoritativePlayers = serverList;

        // Diff against the previous authoritative set to detect joins/leaves.
        // A leave is any previously-known id missing now; a join is any new id.
        m_pendingEvents.reserve(m_prevAuthoritativePlayers.size() + serverList.size());
        for (unsigned short prevId : m_prevAuthoritativePlayers)
        {
            bool stillHere = false;
            for (unsigned short curId : serverList)
            {
                if (curId == prevId) { stillHere = true; break; }
            }
            if (!stillHere)
            {
                JoinLeaveEvent ev;
                ev.clientId = prevId;
                ev.joined = false;
                m_pendingEvents.push_back(ev);
            }
        }
        for (unsigned short curId : serverList)
        {
            bool wasHere = false;
            for (unsigned short prevId : m_prevAuthoritativePlayers)
            {
                if (prevId == curId) { wasHere = true; break; }
            }
            if (!wasHere)
            {
                JoinLeaveEvent ev;
                ev.clientId = curId;
                ev.joined = true;
                m_pendingEvents.push_back(ev);
            }
        }
        m_prevAuthoritativePlayers = serverList;
        break;
    }
    case net::PACKET_PONG:
    {
        // Round-trip = now - when the last ping was sent. Guard the wrap/edge.
        enet_uint32 nowMs = enet_time_get();
        if (m_pingSentMs != 0)
        {
            m_pingMs = nowMs - m_pingSentMs;
            m_pingMeasured = true;
        }
        m_pingSentMs = 0;
        break;
    }
    case net::PACKET_MISSION_STATE:
    {
        // Payload: clientId(u16) + MissionSyncState
        if (payloadSize < sizeof(unsigned short) + static_cast<size_t>(MissionSyncState::SERIALIZED_SIZE)) return;
        const char* p = payload;
        unsigned short sender = net::ReadU16(p);
        if (sender == m_clientId) break; // ignore our own echoed state
        MissionSyncState mstate;
        mstate.Deserialize(p);
        m_remoteMissionStates[sender] = mstate;
        break;
    }
    case net::PACKET_PLAYER_STATE:
    {
        // Payload: clientId(u16) + PlayerNetworkState
        if (payloadSize < sizeof(unsigned short) + static_cast<size_t>(PlayerNetworkState::SERIALIZED_SIZE)) return;
        const char* p = payload;
        unsigned short sender = net::ReadU16(p);
        if (sender == m_clientId) return; // ignore our own echoed state
        PlayerNetworkState state;
        state.Deserialize(p);
        m_remotePlayers.UpdatePlayer(static_cast<int>(sender), state);
        break;
    }
    default:
        break;
    }
}

void NetworkClient::SendPlayerState(const PlayerNetworkState& state)
{
    if (!m_connected || !m_serverReady || !m_host || !m_server) return;
    if ((m_sequence++ % kStateEveryFrames) != 0) return;

    char buffer[1 + sizeof(unsigned short) + PlayerNetworkState::SERIALIZED_SIZE];
    buffer[0] = net::PACKET_PLAYER_STATE;
    char* p = buffer + 1;
    net::WriteU16(p, m_clientId);
    state.Serialize(p);

    ENetPacket* packet = enet_packet_create(buffer, sizeof(buffer), 0);
    if (!packet) return;
    enet_peer_send(m_server, static_cast<enet_uint8>(net::GAME_CHANNEL), packet);
}

void NetworkClient::SendMissionState(const MissionSyncState& state)
{
    if (!m_connected || !m_serverReady || !m_host || !m_server) return;

    char buffer[1 + sizeof(unsigned short) + MissionSyncState::SERIALIZED_SIZE];
    buffer[0] = net::PACKET_MISSION_STATE;
    char* p = buffer + 1;
    net::WriteU16(p, m_clientId);
    state.Serialize(p);

    ENetPacket* packet = enet_packet_create(buffer, sizeof(buffer), 0);
    if (!packet) return;
    enet_peer_send(m_server, static_cast<enet_uint8>(net::GAME_CHANNEL), packet);
}

void NetworkClient::DrainJoinLeaveEvents(std::vector<JoinLeaveEvent>& out)
{
    out.insert(out.end(), m_pendingEvents.begin(), m_pendingEvents.end());
    m_pendingEvents.clear();
}