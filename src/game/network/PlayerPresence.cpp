#include "PlayerPresence.h"

#include <cstring>

namespace net
{
    bool PlayerContext::Encode(WorldStateValue& out) const
    {
        Writer w(out.bytes, WS_MAX_VALUE);
        w.WriteU8(level);
        w.WriteU32(static_cast<uint32_t>(zone & 0xFFFFFFFFu));
        w.WriteU32(static_cast<uint32_t>(zone >> 32));
        if (!w.Ok()) return false;
        out.len = static_cast<uint8_t>(w.Position());
        return true;
    }

    bool PlayerContext::Decode(const WorldStateValue& in)
    {
        Reader r(in.bytes, in.len);
        uint32_t lo = 0, hi = 0;
        if (!(r.ReadU8(level) && r.ReadU32(lo) && r.ReadU32(hi))) return false;
        zone = static_cast<uint64_t>(lo) | static_cast<uint64_t>(hi) << 32;
        return true;
    }

    bool PlayerAppearance::operator==(const PlayerAppearance& o) const
    {
        return std::strncmp(model, o.model, sizeof(model)) == 0 && std::strncmp(anim, o.anim, sizeof(anim)) == 0;
    }

    bool PlayerAppearance::Set(const char* modelName, const char* animBank)
    {
        std::memset(model, 0, sizeof(model));
        std::memset(anim, 0, sizeof(anim));
        if (modelName == nullptr || modelName[0] == '\0') return false;
        std::strncpy(model, modelName, sizeof(model) - 1);
        if (animBank != nullptr) std::strncpy(anim, animBank, sizeof(anim) - 1);
        return true;
    }

    bool PlayerAppearance::Encode(WorldStateValue& out) const
    {
        static_assert(sizeof(model) + sizeof(anim) <= WS_MAX_VALUE, "appearance must fit a world-state value");
        std::memcpy(out.bytes, model, sizeof(model));
        std::memcpy(out.bytes + sizeof(model), anim, sizeof(anim));
        out.len = static_cast<uint8_t>(sizeof(model) + sizeof(anim));
        return true;
    }

    bool PlayerAppearance::Decode(const WorldStateValue& in)
    {
        if (in.len != sizeof(model) + sizeof(anim)) return false;
        std::memcpy(model, in.bytes, sizeof(model));
        std::memcpy(anim, in.bytes + sizeof(model), sizeof(anim));
        model[sizeof(model) - 1] = '\0'; // never trust a peer's terminator
        anim[sizeof(anim) - 1] = '\0';
        return model[0] != '\0';
    }
}

void PlayerPresence::OnWelcomed(net::PeerId self)
{
    m_self = self;
    m_welcomed = true;
    if (m_localContext.level != 0xFF) Publish(net::WOC_PLAYER_CONTEXT);
    if (m_hasLocalAppearance) Publish(net::WOC_APPEARANCE);
}

void PlayerPresence::OnMatchEnd()
{
    m_welcomed = false;
    m_self = 0;
    m_remote.clear();
}

void PlayerPresence::SetLocalContext(const net::PlayerContext& ctx)
{
    if (ctx == m_localContext) return;
    m_localContext = ctx;
    Publish(net::WOC_PLAYER_CONTEXT);
}

void PlayerPresence::SetLocalAppearance(const net::PlayerAppearance& app)
{
    if (m_hasLocalAppearance && app == m_localAppearance) return;
    m_localAppearance = app;
    m_hasLocalAppearance = true;
    Publish(net::WOC_APPEARANCE);
}

void PlayerPresence::Publish(uint8_t channel)
{
    if (!m_welcomed || !m_send) return;
    net::WorldStateOp op;
    op.key.object = net::MakePlayerObjectId(m_self);
    op.key.channel = channel;
    op.kind = net::WSO_SET;
    op.policy = net::WSP_OWNER;
    op.flags = net::WSF_ERASE_ON_OWNER_LEAVE;
    bool ok = channel == net::WOC_PLAYER_CONTEXT ? m_localContext.Encode(op.value)
                                                 : m_localAppearance.Encode(op.value);
    if (ok) m_send(op);
}

void PlayerPresence::OnAuthoritative(const net::WorldStateEntry& e, net::WorldStateReason reason)
{
    if (!net::IsPlayerObjectId(e.key.object)) return;
    if (e.key.channel != net::WOC_PLAYER_CONTEXT && e.key.channel != net::WOC_APPEARANCE) return;
    net::PeerId peer = net::PlayerOfObjectId(e.key.object);
    if (m_welcomed && peer == m_self) return; // our own records

    if (reason == net::WSR_ERASED)
    {
        auto it = m_remote.find(peer);
        if (it == m_remote.end()) return;
        if (e.key.channel == net::WOC_PLAYER_CONTEXT) it->second.hasContext = false;
        else it->second.hasAppearance = false;
        if (!it->second.hasContext && !it->second.hasAppearance) m_remote.erase(it);
        return;
    }

    Remote& r = m_remote[peer];
    if (e.key.channel == net::WOC_PLAYER_CONTEXT)
    {
        net::PlayerContext c;
        if (c.Decode(e.value)) { r.context = c; r.hasContext = true; }
    }
    else
    {
        net::PlayerAppearance a;
        if (a.Decode(e.value) && (!r.hasAppearance || a != r.appearance))
        {
            r.appearance = a;
            r.hasAppearance = true;
            ++r.appearanceVersion;
        }
    }
}

const net::PlayerContext* PlayerPresence::Context(net::PeerId peer) const
{
    auto it = m_remote.find(peer);
    return (it != m_remote.end() && it->second.hasContext) ? &it->second.context : nullptr;
}

const net::PlayerAppearance* PlayerPresence::Appearance(net::PeerId peer) const
{
    auto it = m_remote.find(peer);
    return (it != m_remote.end() && it->second.hasAppearance) ? &it->second.appearance : nullptr;
}

uint32_t PlayerPresence::AppearanceVersion(net::PeerId peer) const
{
    auto it = m_remote.find(peer);
    return it == m_remote.end() ? 0 : it->second.appearanceVersion;
}

bool PlayerPresence::SharesContext(net::PeerId peer) const
{
    const net::PlayerContext* c = Context(peer);
    if (c == nullptr || c->level == 0xFF || m_localContext.level == 0xFF) return true;
    return *c == m_localContext;
}
