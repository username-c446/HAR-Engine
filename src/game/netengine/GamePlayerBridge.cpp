#include "GamePlayerBridge.h"
#include "Session.h"

#include "../network/PlayerNetworkState.h"
#include "../network/RemotePlayerManager.h"

namespace net
{
    GamePlayerBridge::GamePlayerBridge() = default;
    GamePlayerBridge::~GamePlayerBridge() = default;

    void GamePlayerBridge::AttachToSession(Session* session)
    {
        if (!session) return;
        session->SetLocalStateProvider(this);   // engine pulls local blob from us
        session->SetRemoteStateSink(this);      // remote blobs + departures
    }

    bool GamePlayerBridge::GetLocalState(uint8_t* out, uint16_t capacity, uint16_t& size)
    {
        if (m_localSource)
            return m_localSource->GetLocalState(out, capacity, size);

        if (m_typedLocal)
        {
            if (capacity < static_cast<uint16_t>(PlayerNetworkState::SERIALIZED_SIZE))
                return false;
            PlayerNetworkState st;
            if (!m_typedLocal(st)) return false;
            st.Serialize(reinterpret_cast<char*>(out));
            size = static_cast<uint16_t>(PlayerNetworkState::SERIALIZED_SIZE);
            return true;
        }
        return false;
    }

    void GamePlayerBridge::ApplyRemoteState(PeerId from, const uint8_t* data, uint16_t size, uint32_t serverTimeMs)
    {
        if (from == m_selfId) return;                    // skip our own echo
        if (size < static_cast<uint16_t>(PlayerNetworkState::SERIALIZED_SIZE)) return;

        PlayerNetworkState st;
        st.Deserialize(reinterpret_cast<const char*>(data));

        if (m_remotePlayers)
            m_remotePlayers->UpdatePlayer(static_cast<int>(from), st, serverTimeMs);
    }

    void GamePlayerBridge::OnPeerLeft(PeerId peer)
    {
        if (peer == m_selfId) return;
        if (m_remotePlayers)
            m_remotePlayers->RemovePlayer(static_cast<int>(peer));
    }
}