#include "PlayerInterestPolicy.h"
#include "PlayerNetworkState.h"
#include "PlayerPresence.h"
#include "NetEntityManager.h"

void PlayerInterestPolicy::OnPlayerState(net::PeerId peer, const uint8_t* data, uint16_t size, uint32_t serverNowMs)
{
    (void)serverNowMs;
    PlayerNetworkState st;
    if (!st.Deserialize(reinterpret_cast<const char*>(data), size)) return;
    Position& p = m_positions[peer];
    p.x = st.x;
    p.z = st.z;
}

const PlayerInterestPolicy::Position* PlayerInterestPolicy::PositionOf(net::PeerId peer) const
{
    auto it = m_positions.find(peer);
    return it == m_positions.end() ? nullptr : &it->second;
}

bool PlayerInterestPolicy::SameContext(net::PeerId a, net::PeerId b) const
{
    if (m_world == nullptr) return true;
    net::PlayerContext ctx[2];
    const net::PeerId peers[2] = { a, b };
    for (int i = 0; i < 2; ++i)
    {
        net::WorldStateKey key;
        key.object = net::MakePlayerObjectId(peers[i]);
        key.channel = net::WOC_PLAYER_CONTEXT;
        net::WorldStateEntry e;
        if (!m_world->Find(key, e) || !ctx[i].Decode(e.value) || ctx[i].level == 0xFF) return true; // unknown
    }
    return ctx[0] == ctx[1];
}

uint32_t PlayerInterestPolicy::PlayerStateIntervalMs(net::PeerId receiver, net::PeerId sender)
{
    if (!m_config.enabled) return 0;
    if (!SameContext(receiver, sender)) return kNotRelevant;
    const Position* r = PositionOf(receiver);
    const Position* s = PositionOf(sender);
    if (r == nullptr || s == nullptr) return 0;

    float dx = s->x - r->x, dz = s->z - r->z;
    float d2 = dx * dx + dz * dz;
    bool& nearTier = m_near.emplace(std::make_pair(receiver, sender), true).first->second;
    float limit = nearTier ? m_config.nearRadius + m_config.nearHysteresis : m_config.nearRadius;
    nearTier = d2 <= limit * limit;
    return nearTier ? 0 : m_config.farIntervalMs;
}

bool PlayerInterestPolicy::EntityRelevant(net::PeerId receiver, net::PeerId owner, const net::EntityStateRecord& rec)
{
    if (!m_config.enabled) return true;
    if (!SameContext(receiver, owner)) return false;
    const Position* r = PositionOf(receiver);
    net::NetEntityMotion m;
    if (r == nullptr || !m.Decode(rec)) return true;
    float dx = m.x - r->x, dz = m.z - r->z;
    return dx * dx + dz * dz <= m_config.entityRadius * m_config.entityRadius;
}

void PlayerInterestPolicy::OnPeerLeft(net::PeerId peer)
{
    m_positions.erase(peer);
    for (auto it = m_near.begin(); it != m_near.end();)
    {
        if (it->first.first == peer || it->first.second == peer) it = m_near.erase(it);
        else ++it;
    }
}

void PlayerInterestPolicy::Clear()
{
    m_positions.clear();
    m_near.clear();
}
