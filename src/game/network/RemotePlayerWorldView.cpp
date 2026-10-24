#include "RemotePlayerWorldView.h"

RemotePlayerWorldView::RemotePlayerWorldView()
    : m_smoothingSeconds(0.12f)
{
    memset(&m_sink, 0, sizeof(m_sink));
}

RemotePlayerWorldView::~RemotePlayerWorldView()
{
    Clear();
}

void RemotePlayerWorldView::Sync(const RemotePlayerManager& players)
{
    players.ForEachPlayer(
        [](int playerId, const PlayerNetworkState& state, void* userData) -> bool
        {
            RemotePlayerWorldView* self = static_cast<RemotePlayerWorldView*>(userData);
            self->SyncOne(playerId, state);
            return true;
        },
        this);

    // Despawn any remote player that is no longer reported by the network.
    for (auto it = m_entries.begin(); it != m_entries.end();)
    {
        if (!players.HasPlayer(it->first))
        {
            if (m_sink.despawn) m_sink.despawn(it->first, it->second.handle);
            it = m_entries.erase(it);
        }
        else
        {
            ++it;
        }
    }
}

void RemotePlayerWorldView::SyncOne(int playerId, const PlayerNetworkState& state)
{
    auto it = m_entries.find(playerId);
    if (it == m_entries.end())
    {
        // New remote player: spawn if we can, otherwise skip. If spawned, the
        // visual position starts exactly at the reported position so there is
        // no initial fly-in.
        void* handle = m_sink.spawn ? m_sink.spawn(playerId) : nullptr;
        if (handle == nullptr) return;
        Entry entry;
        entry.handle = handle;
        entry.targetX = entry.visualX = state.x;
        entry.targetY = entry.visualY = state.y;
        entry.targetZ = entry.visualZ = state.z;
        entry.targetFacing = entry.visualFacing = state.rotY;
        entry.targetMotionAngle = state.motionAngle;
        entry.targetFacingAngle = state.facingAngle;
        entry.targetSpeedFactor = state.speedFactor;
        entry.targetCharacterState = state.characterState;
        entry.targetActionEvent = state.actionEvent;
        entry.targetFlags = state.flags;
        entry.smoothingApplied = false;
        entry.jumping = ( state.flags & ( PlayerNetworkState::FLAG_JUMPING | PlayerNetworkState::FLAG_IN_AIR ) ) != 0;
        m_entries[playerId] = entry;
        if (m_sink.move) m_sink.move(playerId, handle, state);
        return;
    }

    // Existing remote player: just update the target. The actual movement of
    // the visual representation is driven by Update() so a packet with the same
    // position does not produce stutter.
    Entry& entry = it->second;
    bool jumpingNow = ( state.flags & ( PlayerNetworkState::FLAG_JUMPING | PlayerNetworkState::FLAG_IN_AIR ) ) != 0;
    // Rising edge of a jump: fire the animation callback once so the receiver
    // can play the skeletal jump (not merely snap the height).
    if ( jumpingNow && !entry.jumping && m_sink.jump )
    {
        m_sink.jump(playerId, entry.handle);
    }
    // Rising edge of a non-zero one-shot action: fire the general action
    // channel once (mirrors the jump-latch pattern above). Continuous non-zero
    // actionEvent must not re-fire.
    if ( state.actionEvent != 0 && state.actionEvent != entry.targetActionEvent && m_sink.action )
    {
        m_sink.action(playerId, entry.handle, state.actionEvent);
    }
    entry.targetX = state.x;
    entry.targetY = state.y;
    entry.targetZ = state.z;
    entry.targetFacing = state.rotY;
    entry.targetMotionAngle = state.motionAngle;
    entry.targetFacingAngle = state.facingAngle;
    entry.targetSpeedFactor = state.speedFactor;
    entry.targetCharacterState = state.characterState;
    entry.targetActionEvent = state.actionEvent;
    entry.targetFlags = state.flags;
    entry.smoothingApplied = false;
    entry.jumping = jumpingNow;
}

void RemotePlayerWorldView::Update(float dt)
{
    if (dt <= 0.0f) return;

    // Frame-rate independent exponential smoothing. lambda is chosen so that
    // m_smoothingSeconds reaches ~95% of the remaining distance.
    const float lambda = 3.0f / (m_smoothingSeconds > 0.0f ? m_smoothingSeconds : 0.12f);
    const float alpha = 1.0f - expf(-lambda * dt);

    for (auto& kv : m_entries)
    {
        Entry& entry = kv.second;
        if (entry.smoothingApplied) continue;

        // While the remote player is jumping / airborne, do NOT smooth the
        // vertical axis: interpolation through the arc looks like rubber-banding.
        // Snap the height instantly and only smooth the horizontal plane.
        if (entry.jumping)
        {
            entry.visualY = entry.targetY;
            entry.visualX += (entry.targetX - entry.visualX) * alpha;
            entry.visualZ += (entry.targetZ - entry.visualZ) * alpha;
        }
        else
        {
            entry.visualX += (entry.targetX - entry.visualX) * alpha;
            entry.visualY += (entry.targetY - entry.visualY) * alpha;
            entry.visualZ += (entry.targetZ - entry.visualZ) * alpha;
        }
        entry.smoothingApplied = true;

        // Deferred facing: rotate decrementally toward the target yaw, taking
        // the shortest arc around the circle so a 350deg -> 10deg turn moves
        // forward through 0 rather than backwards through 180.
        static const float kPi = 3.14159265f;
        float diff = entry.targetFacing - entry.visualFacing;
        while (diff >  kPi) diff -= 2.0f * kPi;
        while (diff < -kPi) diff += 2.0f * kPi;
        if (diff != 0.0f) entry.visualFacing += diff * alpha;
        // Normalize to [-PI, PI].
        if (entry.visualFacing >  kPi) entry.visualFacing -= 2.0f * kPi;
        if (entry.visualFacing < -kPi) entry.visualFacing += 2.0f * kPi;

        if (m_sink.move)
        {
            PlayerNetworkState state;
            state.x = entry.visualX;
            state.y = entry.visualY;
            state.z = entry.visualZ;
            state.rotY = entry.visualFacing;
            // Locomotion channels: pass the raw reported targets through — the
            // LocomotionDriver on the game side does its own facing/speed
            // smoothing, so we must not pre-smooth the angles here.
            state.motionAngle = entry.targetMotionAngle;
            state.facingAngle = entry.targetFacingAngle;
            state.speedFactor = entry.targetSpeedFactor;
            state.characterState = entry.targetCharacterState;
            state.flags = entry.targetFlags;
            m_sink.move(kv.first, entry.handle, state);
        }
    }
}

void RemotePlayerWorldView::Clear()
{
    for (auto& kv : m_entries)
    {
        if (m_sink.despawn) m_sink.despawn(kv.first, kv.second.handle);
    }
    m_entries.clear();
}