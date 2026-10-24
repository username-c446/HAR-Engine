#include "RemotePlayerWorldView.h"

RemotePlayerWorldView::RemotePlayerWorldView()
{
    memset(&m_sink, 0, sizeof(m_sink));
}

RemotePlayerWorldView::~RemotePlayerWorldView()
{
    Clear();
}

void RemotePlayerWorldView::Sync(const RemotePlayerManager& players)
{
    struct Ctx { RemotePlayerWorldView* self; const RemotePlayerManager* players; } ctx = { this, &players };
    players.ForEachPlayer(
        [](int playerId, const PlayerNetworkState& state, void* userData) -> bool
        {
            Ctx* c = static_cast<Ctx*>(userData);
            c->self->SyncOne(playerId, state, c->players->GetPlayerTime(playerId));
            return true;
        },
        &ctx);

    // Despawn any remote player that is no longer reported by the network.
    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        if (!players.HasPlayer(it->first))
        {
            if (m_sink.despawn && it->second.handle) m_sink.despawn(it->first, it->second.handle);
            it = m_entries.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void RemotePlayerWorldView::SyncOne(int playerId, const PlayerNetworkState& state, uint32_t serverTimeMs)
{
    const bool jumpingNow = (state.flags & (PlayerNetworkState::FLAG_JUMPING | PlayerNetworkState::FLAG_IN_AIR)) != 0;

    auto it = m_entries.find(playerId);
    if (it == m_entries.end())
    {
        Entry& entry = m_entries[playerId];
        entry.lastActionEvent = state.actionEvent; // a held action is not replayed
        entry.jumping = jumpingNow;
        entry.track.Push(serverTimeMs, state);
        UpdatePresence(playerId, entry, state);
        return;
    }

    Entry& entry = it->second;
    bool fresh = entry.track.Push(serverTimeMs, state);
    UpdatePresence(playerId, entry, state);
    if (!fresh || entry.handle == nullptr) // the same packet again / not shown
    {
        entry.jumping = jumpingNow;
        entry.lastActionEvent = state.actionEvent;
        return;
    }

    // Rising edge of a jump: play the skeletal jump once.
    if (jumpingNow && !entry.jumping && m_sink.jump) m_sink.jump(playerId, entry.handle);
    entry.jumping = jumpingNow;

    // A changed one-shot action byte ([seq|id], held by the sender) is a new
    // action; a repeat of the same action differs by its sequence bits.
    if (state.actionEvent != entry.lastActionEvent && m_sink.action)
    {
        uint8_t id = PlayerNetworkState::ActionId(state.actionEvent);
        if (id != PlayerNetworkState::ACTION_NONE) m_sink.action(playerId, entry.handle, id);
    }
    entry.lastActionEvent = state.actionEvent;
}

void RemotePlayerWorldView::UpdatePresence(int playerId, Entry& entry, const PlayerNetworkState& state)
{
    bool visible = !m_visible || m_visible(playerId);
    if (!visible)
    {
        if (entry.handle != nullptr && m_sink.despawn) m_sink.despawn(playerId, entry.handle);
        entry.handle = nullptr;
        return;
    }
    uint32_t version = m_appearance ? m_appearance(playerId) : 0;
    if (entry.handle == nullptr)
    {
        // (Re)appears: spawn exactly at the reported pose (no fly-in), in its
        // current appearance.
        entry.handle = m_sink.spawn ? m_sink.spawn(playerId) : nullptr;
        entry.appearanceVersion = version;
        if (entry.handle != nullptr && m_sink.move) m_sink.move(playerId, entry.handle, state);
        return;
    }
    if (version != entry.appearanceVersion)
    {
        entry.appearanceVersion = version;
        if (m_sink.restyle) m_sink.restyle(playerId, entry.handle);
    }
}

void RemotePlayerWorldView::Update(float dt)
{
    (void)dt; // poses come from the server clock, not from frame time
    if (!m_sink.move) return;
    for (auto& kv : m_entries)
    {
        if (kv.second.handle == nullptr) continue;
        PlayerNetworkState s;
        if (kv.second.track.Render(m_timing, s)) m_sink.move(kv.first, kv.second.handle, s);
    }
}

int RemotePlayerWorldView::FindPlayer(const void* handle) const
{
    if (handle == nullptr) return 0;
    for (const auto& kv : m_entries)
        if (kv.second.handle == handle) return kv.first;
    return 0;
}

void RemotePlayerWorldView::Clear()
{
    for (auto& kv : m_entries)
    {
        if (m_sink.despawn && kv.second.handle) m_sink.despawn(kv.first, kv.second.handle);
    }
    m_entries.clear();
}
