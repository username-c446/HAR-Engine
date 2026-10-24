//=============================================================================
// Copyright (C) 2002 Radical Entertainment Ltd.  All rights reserved.
//
// File:        
//
// Description: Implement GameplayContext
//
// History:     21/05/2002 + Created -- NAME
//
//=============================================================================

//========================================
// System Includes
//========================================
// Foundation Tech
#include <raddebug.hpp>
#include <radtime.hpp>
#include <raddebugwatch.hpp>

//========================================
// Project Includes
//========================================

#include <ai/vehicle/vehicleairender.h>

#include <camera/animatedcam.h>
#include <camera/debugcam.h>
#include <camera/followcam.h> 
#include <camera/relativeanimatedcam.h>
#include <camera/chasecam.h>
#include <camera/bumpercam.h>
#include <camera/kullcam.h>
#include <camera/trackercam.h>
#include <camera/walkercam.h>
#include <camera/railcam.h>
#include <camera/wrecklesscam.h>
#include <camera/supercammanager.h>
#include <camera/conversationcam.h>
#include <camera/reversecam.h>
#include <camera/snapshotcam.h>
#ifdef RAD_WIN32
#include <camera/pccam.h>
#endif

//#include <camera/firstpersoncam.h>

#include <contexts/gameplay/gameplaycontext.h>
#include <contexts/contextenum.h>
#include <events/eventmanager.h>
#include <data/persistentworldmanager.h>
#include <gameflow/gameflow.h>

#include <choreo/locomotion.hpp>
#include <ai/sequencer/action.h>
#include <ai/sequencer/actioncontroller.h>
#include <ai/sequencer/sequencer.h>

#include <data/gamedatamanager.h>

#include <interiors/interiormanager.h>
#include <memory/leakdetection.h>
#include <memory/srrmemory.h>

#include <meta/triggervolumetracker.h>

#include <mission/gameplaymanager.h>
#include <mission/mission.h>
#include <mission/charactersheet/charactersheetmanager.h>

#include <presentation/presentation.h>
#include <presentation/gui/guisystem.h>

#include <render/rendermanager/rendermanager.h>
#include <render/RenderManager/RenderLayer.h>

#include <worldsim/character/footprint/footprintmanager.h>
#include <worldsim/worldphysicsmanager.h>
#include <worldsim/avatarmanager.h>
#include <worldsim/coins/coinmanager.h>
#include <worldsim/skidmarks/skidmarkmanager.h>
#include <worldsim/coins/sparkle.h>
#include <worldsim/hitnrunmanager.h>
#include <ai/actionbuttonmanager.h>
#include <ai/actor/actormanager.h>


#include <worldsim/character/charactermanager.h>
#include <worldsim/character/character.h>
#include <worldsim/traffic/trafficmanager.h>
#include <worldsim/ped/pedestrianmanager.h>

#include <network/NetworkMatchManager.h>
#include <network/NetworkConsoleCommands.h>
#include <network/MissionSyncState.h>
#include <network/RemotePlayerWorldView.h>
#include <network/RemoteVehicleWorldView.h>
#include <network/NetWorldHooks.h>
#include <set>
#include <network/EconomyConfig.h>
#include <network/LocalActionLatch.h>
#include <render/DSG/StatePropDSG.h>
#include <render/DSG/animcollisionentitydsg.h>
#include <console/console.h>
#include <worldsim/redbrick/vehicle.h>
#include <worldsim/redbrick/geometryvehicle.h>
#include <worldsim/vehiclecentral.h>

#include <render/breakables/breakablesmanager.h>
#include <render/particles/particlemanager.h>
#include <render/animentitydsgmanager/animentitydsgmanager.h>
#include <render/RenderManager/WorldRenderLayer.h>
#include <worldsim/skidmarks/skidmarkgenerator.h>

#include <sound/soundmanager.h>

#include <roads/roadmanager.h>

#include <input/inputmanager.h>
#include <p3d/light.hpp>
#include <p3d/view.hpp>

//******************************************************************************
//
// Global Data, Local Data, Local Classes
//
//******************************************************************************

// Static pointer to instance of singleton.
GameplayContext* GameplayContext::spInstance = NULL;

namespace
{
    // Scratch buffer for the auto-join command-line target (\-join <ip:port>).
    static char s_autoJoinIp[128];

    // Local-state provider for the multiplayer layer: reads the local live Avatar
    // position/heading/vehicle and publishes it to peers over the network.
    //
    // IMPORTANT (world-avatar index vs network peer-id — do not conflate them):
    //  * The game world here is single-avatar per process: AvatarManager is sized
    //    by GameplayManager::GetNumPlayers(), which the mission system fixes at 1
    //    (missionmanager.cpp, `SetNumPlayers(1)`). So GetAvatarForPlayer(0) is the
    //    ONE and only avatar of this process — it is the LOCAL player, whichever
    //    machine this is. It is NOT "the host" by any network meaning.
    //  * The network peer id of this process (net::Session::GetLocalPeerId(),
    //    resolved from the server's WELCOME) is a DIFFERENT axis: it is 0 for the
    //    host (host's loopback client connects first, ServerEndpoint assigns
    //    m_nextPeerId++ in connect order) and 1,2,3... for joiners. It is used by
    //    the bridge (GamePlayerBridge::m_selfId) ONLY to filter our own echo, and
    //    must be pushed via NetworkMatchManager::Update -> SetLocalPeerId so a
    //    client does not wrongly drop the host's state (peer id 0 == world index 0
    //    is a coincidence of connect order, not a semantic link).
    // See docs/MULTIPLAYER_SYNC_MANIFEST.ru.md, "Индекс аватара vs сетевой peer-id".

    // Scales a radian heading to the wire's uint16 (radians * 1000, clamped).
    const float kAngleScale = 1000.0f;
    const uint16_t kAngleMax = 65535;
    // Steer-angle wire scale: byte units per degree. -45..+45 deg ~ -128..127.
    const float kSteerUnitsPerDeg = 255.0f / 90.0f;

    bool GameplayLocalStateProvider( PlayerNetworkState& outState )
    {
        AvatarManager* am = GetAvatarManager();
        if ( am == NULL ) return false;
        Avatar* avatar = am->GetAvatarForPlayer( 0 ); // the local (only) avatar of this process
        if ( avatar == NULL ) return false;

        rmt::Vector pos;
        avatar->GetPosition( pos );
        outState.x = pos.x;
        outState.y = pos.y;
        outState.z = pos.z;

        rmt::Vector heading;
        avatar->GetHeading( heading );
        outState.rotY = heading.y;

        outState.inVehicle = avatar->IsInCar() ? 1 : 0;

        // Carry animation/prediction flags so the receiver knows when NOT to
        // smooth our trajectory (e.g. while airborne / jumping, smoothing the
        // vertical arc produces the "bouncy rubber-band" artifact).
        outState.flags = 0;
        Character* pChar = avatar->GetCharacter();
        if ( pChar != NULL )
        {
            if ( pChar->IsJumping() )
            {
                outState.flags |= PlayerNetworkState::FLAG_JUMPING;
            }
            if ( pChar->GetLocoVelocity().y > 0.01f || pChar->GetLocoVelocity().y < -0.01f )
            {
                outState.flags |= PlayerNetworkState::FLAG_IN_AIR;
            }
            if ( pChar->IsTurbo() )
            {
                outState.flags |= PlayerNetworkState::FLAG_LOCO_TURBO;
            }
        }

        Vehicle* vehicle = avatar->GetVehicle();
        if (outState.inVehicle && vehicle != NULL)
        {
            // Stable network id: derive from the central index so it is identical
            // for the same machine slot; fall back to a hash of the pointer so it
            // is at least stable for the session.
            const char* name = vehicle->GetName();
            size_t i = 0;
            char* dst = outState.vehicleModel;
            while (name != NULL && name[i] != '\0' && i < sizeof(outState.vehicleModel) - 1)
            {
                dst[i] = name[i];
                ++i;
            }
            outState.vehicleModel[i] = '\0';

            int central = vehicle->mVehicleCentralIndex;
            outState.netVehicleId = central >= 0 ? central
                : static_cast<int>( reinterpret_cast<uintptr_t>( vehicle ) & 0x7FFFFFFF );

            outState.vehicleSpeed = vehicle->GetSpeedKmh();

            // Loco/action channels are meaningless while in a machine: zero them.
            outState.motionAngle = 0;
            outState.facingAngle = 0;
            outState.speedFactor = 0;
            outState.characterState = 0;
            outState.actionEvent = 0;

            // Faithful kinematic vehicle mirror fields.
            outState.vehYaw = vehicle->GetFacingInRadians();
            float steerDeg = vehicle->mWheelTurnAngle * (180.0f / rmt::PI);
            outState.vehSteer = static_cast<uint8_t>(
                static_cast<int8_t>( steerDeg * kSteerUnitsPerDeg ) );
            float kmh = vehicle->GetSpeedKmh();
            outState.vehSpeedKmh = kmh < 0.0f ? 0 : ( kmh > 65535.0f ? 65535 : static_cast<uint16_t>( kmh ) );
            outState.vehState = static_cast<uint8_t>( vehicle->mVehicleState );
            outState.vehFlags = 0;
            if ( vehicle->mBrakeLightsOn )    outState.vehFlags |= PlayerNetworkState::VEH_FLAG_BRAKE;
            if ( vehicle->mReverseLightsOn )  outState.vehFlags |= PlayerNetworkState::VEH_FLAG_REVERSE;
            if ( vehicle->IsVehicleDestroyed() ) outState.vehFlags |= PlayerNetworkState::VEH_FLAG_DESTROYED;
        }
        else
        {
            outState.vehicleModel[0] = '\0';
            outState.netVehicleId = 0;
            outState.vehicleSpeed = 0.0f;
            outState.inVehicle = 0;

            // Zero the machine-mirror fields (unused on foot).
            outState.vehYaw = 0.0f;
            outState.vehSteer = 0;
            outState.vehSpeedKmh = 0;
            outState.vehState = 0;
            outState.vehFlags = 0;

            // Continuous locomotion + one-shot action for the on-foot state.
            outState.motionAngle = 0;
            outState.facingAngle = 0;
            outState.speedFactor = 0;
            outState.characterState = 0;
            outState.actionEvent = 0;
            if ( pChar != NULL )
            {
                if ( pChar->GetFacingDir() >= 0.0f && pChar->GetFacingDir() < 2.0f * rmt::PI )
                {
                    int f = static_cast<int>( pChar->GetFacingDir() * kAngleScale );
                    outState.facingAngle = static_cast<uint16_t>( f < 0 ? 0 : ( f > kAngleMax ? kAngleMax : f ) );
                }
                // DesiredDir is the direction of travel. If it is unusable, fall
                // back to the facing angle (the character pivots in place / walks
                // facing wherever it is headed by default).
                float desiredDir = pChar->GetDesiredDir();
                if ( desiredDir < 0.0f || desiredDir >= 2.0f * rmt::PI )
                {
                    desiredDir = pChar->GetFacingDir();
                }
                int m = static_cast<int>( desiredDir * kAngleScale );
                outState.motionAngle = static_cast<uint16_t>( m < 0 ? 0 : ( m > kAngleMax ? kAngleMax : m ) );

                float maxSpeed = pChar->GetMaxSpeed();
                if ( maxSpeed > 0.0f )
                {
                    float frac = pChar->GetDesiredSpeed() / maxSpeed;
                    frac = frac < 0.0f ? 0.0f : ( frac > 1.0f ? 1.0f : frac );
                    outState.speedFactor = static_cast<uint8_t>( frac * 255.0f );
                }
                else
                {
                    outState.speedFactor = 0;
                }

                CharacterAi::StateManager* sm = pChar->GetStateManager();
                if ( sm != NULL )
                {
                    outState.characterState = static_cast<uint8_t>( sm->GetState() );
                }

                // One-shot action: the latest [seq|id] latched by the game's
                // action sites (dodge, attack, celebrate, knock-down, ...). Jumps
                // ride FLAG_JUMPING instead, so they are not double-fired.
                outState.actionEvent = net::LocalActionLatch::Current();
            }
        }
        return true;
    }

    // Local mission-state provider: reports the local player's current mission,
    // stage and objective so co-op partners can track progress together.
    bool GameplayMissionStateProvider( MissionSyncState& outState )
    {
        // GameplayManager owns the current mission. Guarded against absence.
        GameplayManager* gm = GetGameplayManager();
        if ( gm == NULL ) return false;
        Mission* mission = gm->GetCurrentMission();
        MissionSyncState state;
        state.Clear();
        if ( mission != NULL )
        {
            state.missionIndex = gm->GetCurrentMissionIndex();
            state.stageIndex = mission->GetCurrentStageIndex();
            state.inMission = 1;
        }
        outState = state;
        return true;
    }

    // World sink for remote players: spawn/move/despawn lightweight NPCs that
    // mirror what other network players report through their PlayerNetworkState.
    void* RemotePlayerSpawn( int playerId )
    {
        char name[32];
        sprintf( name, "mp_remote_%d", playerId );
        Character* c = GetCharacterManager()->AddCharacter(
            CharacterManager::NPC, name, "homer", "npd", NULL );
        if ( c == NULL )
        {
            rReleasePrintf( "net: could not spawn remote player %d\n", playerId );
            return NULL;
        }
        // AddCharacter does NOT put NPCs into the world scene (only PCs get
        // AddToWorldScene there), so an NPC would exist but never render.
        // Also, an NPC in its default state is static: locomotion is not driven
        // so the skeleton stays in the bind pose (the "T pose"). Put it into the
        // LOCO state so its walk/jump locomotion drivers run and animate it.
        c->AddToWorldScene();
        CharacterAi::StateManager* sm = c->GetStateManager();
        if ( sm != NULL && sm->GetState() != CharacterAi::LOCO )
        {
            sm->SetState<CharacterAi::Loco>();
        }
        return c;
    }

    void RemotePlayerMove( int playerId, void* handle, const PlayerNetworkState& state )
    {
        (void)playerId;
        Character* c = static_cast<Character*>( handle );
        if ( c == NULL ) return;
        rmt::Vector pos( state.x, state.y, state.z );

// Drive locomotion so the remote character animates (walks/runs) rather
        // than sliding in the bind pose. StateManager::Update / the action
        // controller only run for NPCs when they are inside someone's frustum
        // and even then only intermittently, so we cannot rely on
        // WalkerLocomotionAction::DoSimulation to apply a desired speed. Instead
        // feed the locomotion DRIVER directly (exactly what DoSimulation does),
        // and let UpdatePuppet()->Advance() (run in PostSimUpdate for NPCs) turn
        // that into a walk/run animation.
        if ( c->GetStateManager() != NULL && c->GetStateManager()->GetState() == CharacterAi::LOCO )
        {
            choreo::LocomotionDriver* driver =
                c->GetWalkerLocomotionAction()->GetDriver();
            if ( driver != NULL )
            {
                // Use the reported continuous-locomotion fields directly. Only
                // when characterState says the sender is LOCO (walking/running)
                // do we drive the pose with these; otherwise we leave the remote
                // character sitting in whatever its current action is.
                if ( state.characterState == static_cast<uint8_t>(CharacterAi::LOCO) )
                {
                    float motionRad = state.motionAngle / 1000.0f;
                    float facingRad = state.facingAngle / 1000.0f;

                    // Scale the 0..255 fraction by the sender-consistent max
                    // speed. GetMaxSpeed() already folds turbo in; a remote turbo
                    // bumper is mirrored by also raising the clamp here.
                    float maxSpeed = c->GetMaxSpeed();
                    if ( ( state.flags & PlayerNetworkState::FLAG_LOCO_TURBO )
                         && !c->IsTurbo() )
                    {
                        maxSpeed = CharacterTune::sfMaxSpeed + CharacterTune::sfDashBurstMax;
                    }
                    float speed = ( state.speedFactor / 255.0f ) * maxSpeed;

                    driver->SetDesiredMotionAngle( motionRad );
                    driver->SetDesiredFacingAngle( facingRad );
                    driver->SetDesiredVelocity( speed );
                    c->SetFacingDir( facingRad );
                    c->SetDesiredDir( motionRad );
                }
            }
        }

        // Snap the world position only when the locomotion position drifts too
        // far from the authoritative network position (otherwise we fight the
        // locomotion every frame and cause jitter).
        rmt::Vector cur;
        c->GetPosition( &cur );
        rmt::Vector delta = pos - cur;
        if ( delta.MagnitudeSqr() > 1.0f )
        {
            c->SetPosition( pos );
        }
}

    void RemotePlayerDespawn( int playerId, void* handle )
    {
        (void)playerId;
        Character* c = static_cast<Character*>( handle );
        if ( c == NULL ) return;
        GetCharacterManager()->RemoveCharacter( c );
    }

    // The remote player started a jump (rising edge of FLAG_JUMPING): play the
    // skeletal jump animation once, exactly as the local player's Loco state
    // does, so the remote character visibly jumps instead of just snapping up.
    void RemotePlayerJump( int playerId, void* handle )
    {
        (void)playerId;
        Character* c = static_cast<Character*>( handle );
        if ( c == NULL ) return;
        if ( c->GetStateManager() == NULL || c->GetStateManager()->GetState() != CharacterAi::LOCO ) return;

        JumpAction* jump = c->GetJumpLocomotionAction();
        if ( jump == NULL ) return;
        jump->Reset( CharacterTune::sfJumpHeight, false );
        c->GetWalkerLocomotionAction()->StopDriver();
        c->GetActionController()->GetNextSequencer()->BeginSequence();
        c->GetActionController()->GetNextSequencer()->AddAction( jump );
        c->GetActionController()->GetNextSequencer()->EndSequence();
    }

    // Plays a one-shot clip on the remote character as its own sequence.
    // PlayAnimationAction degrades to a no-op when the remote's animation bank
    // lacks the clip (remote players use an NPC bank), so nothing asserts.
    void RemotePlayClip( Character* c, const char* clip, bool clearFirst )
    {
        if ( clearFirst )
        {
            c->GetActionController()->Clear();
        }
        Sequencer* pSeq = c->GetActionController()->GetNextSequencer();
        pSeq->BeginSequence();
        pSeq->AddAction( new PlayAnimationAction( c, clip ) );
        pSeq->EndSequence();
        c->GetWalkerLocomotionAction()->StopDriver();
    }

    // The remote player started a one-shot action (a new [seq|id] byte): replay
    // the matching skeletal clip locally. Visual only: gameplay effects of the
    // action (a kick breaking a prop, ...) reach us as world events instead.
    void RemotePlayerAction( int playerId, void* handle, uint8_t actionId )
    {
        Character* c = static_cast<Character*>( handle );
        if ( c == NULL || c->GetActionController() == NULL ) return;

        switch ( actionId )
        {
        case PlayerNetworkState::ACTION_JUMP:
            RemotePlayerJump( playerId, handle );
            break;
        // Airborne moves: the remote is mid-JumpAction, whose sequence would hold
        // a queued clip until landing, so overlay the clip on the puppet directly.
        case PlayerNetworkState::ACTION_JUMPKICK:
            if ( c->GetPuppet() != NULL ) c->GetPuppet()->PlayAnimation( "jump_kick" );
            break;
        case PlayerNetworkState::ACTION_SLAM:
            if ( c->GetPuppet() != NULL ) c->GetPuppet()->PlayAnimation( "stomp_in_air" );
            break;
        case PlayerNetworkState::ACTION_DODGE:       RemotePlayClip( c, "dodge", false ); break;
        case PlayerNetworkState::ACTION_CRINGE:      RemotePlayClip( c, "cringe", false ); break;
        case PlayerNetworkState::ACTION_ATTACK:      RemotePlayClip( c, "break_quick", false ); break;
        case PlayerNetworkState::ACTION_CELEB_SMALL: RemotePlayClip( c, "victory_small", false ); break;
        case PlayerNetworkState::ACTION_CELEB_BIG:   RemotePlayClip( c, "victory_large", false ); break;
        // Knocked down: drop whatever it was doing and flail; the sender reports
        // ACTION_GETUP when its own get-up starts.
        case PlayerNetworkState::ACTION_FLAIL:       RemotePlayClip( c, "flail", true ); break;
        case PlayerNetworkState::ACTION_GETUP:       RemotePlayClip( c, "get_up", true ); break;
        default:
            break; // reserved / unknown (newer peer)
        }
    }

// Ghost-vehicle sink: mirrors a remote driver's machine in the world.
    // The vehicle is created through VehicleCentral just like a normal car, so
    // the correct model renders. We keep it simple: the ghost tracks the remote
    // driver's reported position each frame; collisions are those of a normal
    // active vehicle.
void* RemoteVehicleSpawn( int playerId, const char* model, int netVehicleId )
    {
        (void)netVehicleId;
        (void)playerId;
        // Create a network ghost that is opaque to the rest of the game: it is
        // placed in the world scene + collision so the remote car renders and
        // blocks, but it is NOT registered in VehicleCentral's sweepable
        // active-vehicle list and carries no type any game manager sweeps. This
        // guarantees TrafficManager / parked-cars / mission-reset / husk logic can
        // never steal or free the ghost under the network layer (the use-after-free
        // / heap-corruption hazard on the multi-client host). Ownership is
        // relinquished on despawn by RemoteVehicleDespawn (which must NOT delete
        // the ghost: the final Release() inside RemoveNetworkGhostVehicle already
        // frees it).
        Vehicle* v = GetVehicleCentral()->AddNetworkGhostVehicle( model );
        if ( v == NULL ) return NULL;
        return v;
    }

    // Ghosts whose destruction (explosion) has already been shown.
    std::set<Vehicle*> s_explodedGhosts;

    // Kinematic mirror shared by remote drivers' machines and remote-owned
    // traffic: position + heading through a matrix (the established spawn
    // pattern, see vehicle.cpp ResetOnSpot), then the reported speed (which
    // SetTransform zeroes), steering and lights. A ghost never simulates.
    void ApplyGhostKinematics( Vehicle* v, float x, float y, float z, float yaw, float speedKmh,
                               float steerRad, bool brake, bool reverse, bool destroyed )
    {
        rmt::Matrix m;
        m.Identity();
        m.FillRotateXYZ( 0.0f, yaw, 0.0f );
        m.FillTranslate( rmt::Vector( x, y, z ) );
        v->SetTransform( m );

        if ( speedKmh < 0.0f ) speedKmh = 0.0f;
        v->mSpeedKmh = speedKmh;
        v->mSpeed = speedKmh / 3.6f;
        v->SetWheelTurnAngleDirectlyInRadiansForDusitOnly( steerRad );

        if ( v->mGeometryVehicle != NULL )
        {
            if ( brake )   v->mGeometryVehicle->ShowBrakeLights();   else v->mGeometryVehicle->HideBrakeLights();
            if ( reverse ) v->mGeometryVehicle->ShowReverseLights(); else v->mGeometryVehicle->HideReverseLights();
        }

        // The owner blew it up: show the explosion once on this ghost.
        if ( destroyed && s_explodedGhosts.insert( v ).second )
        {
            v->PlayExplosionEffect();
        }
        else if ( !destroyed )
        {
            s_explodedGhosts.erase( v );
        }
    }

    void RemoteVehicleMove( int playerId, void* handle, const PlayerNetworkState& state )
    {
        (void)playerId;
        Vehicle* v = static_cast<Vehicle*>( handle );
        if ( v == NULL ) return;
        // Steering: the signed steer byte maps -128..127 to -45..+45 degrees.
        float steerDeg = static_cast<int8_t>( state.vehSteer ) / kSteerUnitsPerDeg;
        ApplyGhostKinematics( v, state.x, state.y, state.z, state.vehYaw,
                              static_cast<float>( state.vehSpeedKmh ),
                              steerDeg * ( rmt::PI / 180.0f ),
                              ( state.vehFlags & PlayerNetworkState::VEH_FLAG_BRAKE ) != 0,
                              ( state.vehFlags & PlayerNetworkState::VEH_FLAG_REVERSE ) != 0,
                              ( state.vehFlags & PlayerNetworkState::VEH_FLAG_DESTROYED ) != 0 );
    }

    void RemoteVehicleDespawn( int playerId, void* handle )
    {
        (void)playerId;
        Vehicle* v = static_cast<Vehicle*>( handle );
        if ( v == NULL ) return;
        // The ghost is NOT in the active list (see AddNetworkGhostVehicle), so
        // RemoveVehicleFromActiveList would be a no-op. Use the dedicated ghost
        // teardown that undoes the world-scene + collision registration.
        //
        // NOTE: we must NOT `delete v` here. AddNetworkGhostVehicle took two
        // references (the explicit ownership AddRef + the world scene's AddRef),
        // so RemoveNetworkGhostVehicle's final Release() drops the count to 0 and
        // radLoadObject::Release() SELF-DELETES the Vehicle (it frees the object
        // whenever the count is < 2). An extra `delete v` here is therefore a
        // second free of the same block -> heap corruption (0xc0000005) that
        // kills the host as soon as two remotes drive and their ghosts
        // spawn/despawn. This sink's only job is to relinquish the ownership; the
        // teardown already freed the object.
        s_explodedGhosts.erase( v );
        GetVehicleCentral()->RemoveNetworkGhostVehicle( v );
    }

    // ---- remote-owned dynamic entities (NetEntityManager sink) -------------

    void* NetEntitySpawnGhost( uint64_t id, const net::NetEntitySpawn& spawn )
    {
        (void)id;
        if ( spawn.kind != net::NEK_TRAFFIC_CAR ) return NULL;
        Vehicle* v = GetVehicleCentral()->AddNetworkGhostVehicle( spawn.model );
        if ( v != NULL )
        {
            ApplyGhostKinematics( v, spawn.x, spawn.y, spawn.z, spawn.yaw, 0.0f, 0.0f, false, false, false );
        }
        return v;
    }

    void NetEntityMoveGhost( void* handle, const net::NetEntityMotion& m )
    {
        Vehicle* v = static_cast<Vehicle*>( handle );
        if ( v == NULL ) return;
        float steerRad = ( m.steer / 127.0f ) * 45.0f * ( rmt::PI / 180.0f );
        ApplyGhostKinematics( v, m.x, m.y, m.z, m.yaw, static_cast<float>( m.speedKmh ), steerRad,
                              ( m.flags & net::NetEntityMotion::MF_BRAKE ) != 0,
                              ( m.flags & net::NetEntityMotion::MF_REVERSE ) != 0,
                              ( m.flags & net::NetEntityMotion::MF_DESTROYED ) != 0 );
    }

    void NetEntityDespawnGhost( void* handle )
    {
        Vehicle* v = static_cast<Vehicle*>( handle );
        if ( v == NULL ) return;
        s_explodedGhosts.erase( v );
        // See RemoteVehicleDespawn: the teardown releases (and frees) the ghost.
        GetVehicleCentral()->RemoveNetworkGhostVehicle( v );
    }

    // Samples one of OUR traffic cars for the entity stream.
    bool NetEntitySampleTraffic( void* handle, net::NetEntityMotion& out )
    {
        Vehicle* v = static_cast<Vehicle*>( handle );
        if ( v == NULL ) return false;
        rmt::Vector pos;
        v->GetPosition( &pos );
        out.x = pos.x; out.y = pos.y; out.z = pos.z;
        out.yaw = v->GetFacingInRadians();
        float kmh = v->GetSpeedKmh();
        out.speedKmh = kmh < 0.0f ? 0 : ( kmh > 65535.0f ? 65535 : static_cast<uint16_t>( kmh ) );
        float steerDeg = v->mWheelTurnAngle * ( 180.0f / rmt::PI );
        float steerUnits = steerDeg / 45.0f * 127.0f;
        out.steer = static_cast<int8_t>( steerUnits > 127.0f ? 127 : ( steerUnits < -127.0f ? -127 : steerUnits ) );
        out.flags = 0;
        if ( v->mBrakeLightsOn )       out.flags |= net::NetEntityMotion::MF_BRAKE;
        if ( v->mReverseLightsOn )     out.flags |= net::NetEntityMotion::MF_REVERSE;
        if ( v->IsVehicleDestroyed() ) out.flags |= net::NetEntityMotion::MF_DESTROYED;
        return true;
    }

    // ---- local coin bank <-> server ledger (PlayerEconomy) -----------------

    int64_t EconomyGetBalance( net::CounterId counter )
    {
        (void)counter;
        return GetCoinManager()->GetBankValue();
    }

    void EconomySetBalance( net::CounterId counter, int64_t balance )
    {
        (void)counter;
        if ( balance < 0 ) balance = 0;
        if ( balance > INT32_MAX ) balance = INT32_MAX;
        // Straight to the character sheet (the bank's store): not reported back.
        GetCharacterSheetManager()->SetNumberOfTokens( GetGameplayManager()->GetCurrentLevelIndex(),
                                                       static_cast<int>( balance ) );
    }

    // Singleton access without leading to circular includes.
    RemotePlayerWorldView* GetRemotePlayerWorldView()
    {
        static RemotePlayerWorldView sWorldView;
        return &sWorldView;
    }

    RemoteVehicleWorldView* GetRemoteVehicleWorldView()
    {
        static RemoteVehicleWorldView sWorldView;
        return &sWorldView;
    }
}

//******************************************************************************
//
// Public Member Functions
//
//******************************************************************************

//==============================================================================
// GameplayContext::GetInstance
//==============================================================================
//
// Description: - Access point for the GameplayContext singleton.  
//              - Creates the GameplayContext if needed.
//
// Parameters:	None.
//
// Return:      Pointer to the GameplayContext.
//
// Constraints: This is a singleton so only one instance is allowed.
//
//==============================================================================
GameplayContext* GameplayContext::GetInstance()
{
    if( spInstance == NULL )
    {
        spInstance = new(GMA_PERSISTENT) GameplayContext;
        rAssert( spInstance );
    }
    
    return spInstance;
}

//==============================================================================
// GameplayContext::GameplayContext
//==============================================================================
// Description: Constructor.
//
// Parameters: None.
//
// Return:      N/A.
//
//==============================================================================
GameplayContext::GameplayContext() :
    m_PausedAllButPresentation( false ),
    mSlowMoHack( false )
{
#ifdef DEBUGWATCH
    radDbgWatchAddUnsignedInt( &mDebugPhysTiming, "Debug Phys micros", "GameplayContext", NULL, NULL );
    radDbgWatchAddUnsignedInt( &mDebugTimeDelta, "Debug dT micros", "GameplayContext", NULL, NULL );
    radDbgWatchAddUnsignedInt( &mDebugOnUpdateDT, "Gameplay dT micros", "GameplayContext", NULL, NULL );
    radDbgWatchAddBoolean(&mSlowMoHack, "Slow Mo Hack", "GameplayContext", NULL, NULL);
#endif
}

//==============================================================================
// GameplayContext::~GameplayContext
//==============================================================================
// Description: Destructor.
//
// Parameters: None.
//
// Return:      N/A.
//
//==============================================================================
GameplayContext::~GameplayContext()
{
#ifdef DEBUGWATCH
    radDbgWatchDelete(&mDebugPhysTiming);
    radDbgWatchDelete(&mDebugTimeDelta);
    radDbgWatchDelete(&mDebugOnUpdateDT);
    
    radDbgWatchDelete(&mSlowMoHack);
#endif
}

//******************************************************************************
//
// Private Member Functions
//
//******************************************************************************

//=============================================================================
// GameplayContext::OnStart
//=============================================================================
// Description: Comment
//
// Parameters:  ( ContextEnum previousContext )
//
// Return:      void 
//
//=============================================================================
void GameplayContext::OnStart( ContextEnum previousContext )
{
    // Common to all playing contexts.
    //
    PlayingContext::OnStart( previousContext );

    // Runtime check that the live Scrooby loader reads a P3DWriter-generated
    // .p3d (proves our asset generator's output is loadable). One-time, safe.
    // [DISABLED pending hang investigation]

    // Initialise the multiplayer match controller and feed it the local player's
    // live state. It stays idle until host/join is invoked.
    // [ISOLATION TEST: RegisterNetworkConsoleCommands disabled]
    //RegisterNetworkConsoleCommands();
    NetworkMatchManager* pMatchMgr = NetworkMatchManager::CreateInstance();
    pMatchMgr->SetLocalStateProvider( &GameplayLocalStateProvider );
    pMatchMgr->SetMissionStateProvider( &GameplayMissionStateProvider );

    // Replicated world services: the coin bank feeds the server ledger, and
    // remote-owned traffic is mirrored as kinematic ghosts. Static world
    // objects register themselves while the level loads (NetWorldRegistry).
    net::LocalActionLatch::Reset();
    NetworkMatchManager::Economy().SetBalanceAccess( &EconomyGetBalance, &EconomySetBalance );
    {
        NetEntityManager::Sink entitySink;
        entitySink.spawn = &NetEntitySpawnGhost;
        entitySink.move = &NetEntityMoveGhost;
        entitySink.despawn = &NetEntityDespawnGhost;
        NetworkMatchManager::Entities().SetSink( entitySink );
        NetworkMatchManager::Entities().SetSampler( &NetEntitySampleTraffic );
    }

    // Auto-start a match from the command line (-host <port> / -join <ip:port>).
    // Useful for scripted/testing launches of host and client copies.
    bool autoHost = CommandLineOptions::GetHostMode();
    bool autoJoin = CommandLineOptions::GetJoinMode();
    unsigned short hostPort = CommandLineOptions::GetHostPort();
    unsigned short joinPort = CommandLineOptions::GetJoinPort();
    strcpy( s_autoJoinIp, CommandLineOptions::GetJoinIp() );
    const char* hostPassword = CommandLineOptions::GetHostPassword();
    unsigned short hostSlots = CommandLineOptions::GetHostSlots();
    const char* joinPassword = CommandLineOptions::GetJoinPassword();
    CommandLineOptions::ClearNetworkOptions();
    (void)hostPassword; (void)hostSlots; (void)joinPassword;
    if (autoHost || autoJoin)
    {
        if (autoHost)
        {
            pMatchMgr->SetHostConfig( hostPassword[0] != '\0' ? hostPassword : NULL, hostSlots );
            if ( !pMatchMgr->StartHost( hostPort ) )
            {
                rReleasePrintf( "net: could not host on port %u\n", hostPort );
            }
        }
        else if (autoJoin)
        {
            if (joinPassword[0] != '\0') pMatchMgr->SetClientPassword( joinPassword );
            if ( !pMatchMgr->JoinHost( s_autoJoinIp, joinPort ) )
            {
                rReleasePrintf( "net: could not join %s:%u\n", s_autoJoinIp, joinPort );
            }
        }
    }

    // Wire the in-world view of remote players to real in-game characters.
    RemotePlayerWorldView::Sink sink;
    sink.spawn = &RemotePlayerSpawn;
    sink.move = &RemotePlayerMove;
    sink.jump = &RemotePlayerJump;
    sink.action = &RemotePlayerAction;
    sink.despawn = &RemotePlayerDespawn;
    GetRemotePlayerWorldView()->SetSink( sink );

    // Wire the ghost-vehicle view so remote drivers are mirrored as machines.
    RemoteVehicleWorldView::Sink vsink;
    vsink.spawn = &RemoteVehicleSpawn;
    vsink.move = &RemoteVehicleMove;
    vsink.despawn = &RemoteVehicleDespawn;
    GetRemoteVehicleWorldView()->SetSink( vsink );

    MEMTRACK_PUSH_FLAG( "Gameplay" );
    // do gameplay initializations only if previous context was loading context
    if( previousContext == CONTEXT_LOADING_GAMEPLAY )
    {
        // RenderManager
        //
        RenderManager* rm = GetRenderManager();

        RenderLayer* rl = rm->mpLayer( RenderEnums::LevelSlot );
        rAssert( rl );

#ifdef DEBUGWATCH
        // bootstrap vehicleai renderer
        VehicleAIRender::GetVehicleAIRender();
#endif

        // Set up cameras
        //
        unsigned int iNumPlayers = GetGameplayManager()->GetNumPlayers();

        rl->SetNumViews( iNumPlayers );
        rl->SetUpViewCam();

		p3d::inventory->SelectSection("Default");
        tLightGroup* sun = p3d::find<tLightGroup>("sun");   
        rAssert( sun );
        rm->SetLevelLayerLights( sun );

        float aspect = p3d::display->IsWidescreen() ? (16.0f / 9.0f) : (4.0f / 3.0f);

        if( iNumPlayers == 2 )
        {
            aspect = 4.0f / 1.5f;
        }

        for( unsigned int view = 0; view < iNumPlayers; view++ )
        {

            tPointCamera* cam = static_cast<tPointCamera*>( rl->pCam( view ) );
            rAssert( dynamic_cast<tPointCamera*> ( cam ) != NULL );
            rAssert( cam );

            SuperCamCentral* scc = GetSuperCamManager()->GetSCC( view );
            rAssert( scc );
            scc->SetCamera( cam );
    
            FollowCam* fc = new FollowCam( FollowCam::FOLLOW_NEAR );
            fc->SetAspect( aspect );
            fc->CopyToData();
            scc->RegisterSuperCam( fc );

            fc = new FollowCam( FollowCam::FOLLOW_FAR );
            fc->SetAspect( aspect );
            fc->CopyToData();
            scc->RegisterSuperCam( fc );

            SuperCam* sc = new BumperCam();
            sc->SetAspect( aspect );
            scc->RegisterSuperCam( sc );
            sc = new ChaseCam();
            sc->SetAspect( aspect );
            scc->RegisterSuperCam( sc );
            sc = new KullCam();
            sc->SetAspect( aspect );
            scc->RegisterSuperCam( sc );
            sc = new DebugCam();
            sc->SetAspect( aspect );
            scc->RegisterSuperCam( sc );
            sc = new TrackerCam();
            sc->SetAspect( aspect );
            scc->RegisterSuperCam( sc );
            sc = new WalkerCam();
            sc->SetAspect( aspect );
            scc->RegisterSuperCam( sc );
            sc = new ComedyCam();
            sc->SetAspect( aspect );
            scc->RegisterSuperCam( sc );
            sc = new WrecklessCam();
            sc->SetAspect( aspect );
            scc->RegisterSuperCam( sc );
            sc = new ConversationCam();
            sc->SetAspect( aspect );
            scc->RegisterSuperCam( sc );
            sc = new ReverseCam();
            sc->SetAspect( aspect );
            scc->RegisterSuperCam( sc );
            sc = new AnimatedCam();
            sc->SetAspect( aspect );
            scc->RegisterSuperCam( sc );
            sc = new RelativeAnimatedCam();
            sc->SetAspect( aspect );
            scc->RegisterSuperCam( sc );
//            sc = new FirstPersonCam();
//            sc->SetAspect( aspect );
//            scc->RegisterSuperCam( sc );
#ifdef RAD_WIN32
            sc = new PCCam();
            sc->SetAspect( aspect );
            scc->RegisterSuperCam( sc );
#endif
#if !defined(FINAL) && defined(RAD_UWP)
            sc = new SnapshotCam();
            sc->SetAspect( aspect );
            scc->RegisterSuperCam( sc );
#endif
        }

        // Boot strap the AvatarManager.
        //
        GetAvatarManager( )->EnterGame( );
        GetActionButtonManager( )->EnterGame( );

        TrafficManager::GetInstance()->Init();
        PedestrianManager::GetInstance()->Init();
        
        // Prepare the manager to start gameplay
        //
        //GetGameplayManager()->Reset();

#ifdef RAD_DEMO
        GetGameplayManager()->ResetIdleTime();
#endif

        // Start up GUI in-game manager
        GetGuiSystem()->HandleMessage( GUI_MSG_RUN_INGAME );

        // register GUI user input handler for active players only
        //
        int activeControllerIDs = 0;
        for( int i = 0; i < MAX_PLAYERS; i++ )
        {
            int controllerID = GetInputManager()->GetControllerIDforPlayer( i );
            if( controllerID != -1 )
            {
                activeControllerIDs |= (1 << controllerID);
            }
        }

        GetGuiSystem()->RegisterUserInputHandlers( activeControllerIDs );

        // Tell GameData Manager that game has been started
        //
        GetGameDataManager()->SetGameLoaded();

        //Set up the sorting of the intersections and stuff.
        RoadManager::GetInstance()->CreateRoadNetwork();

        //Find skid mark shaders in the inventory and set proper values
        SkidMarkGenerator::InitShaders();

        GetSkidmarkManager()->Init();

        //
        // Notify the sound system of gameplay start.  This has been moved after
        // the GUI startup above, since that leads to a mission reset, which causes
        // character position in or out of the car to be decided, which the sound
        // system uses to determine which sounds to start playing.
        //
        SoundManager::GetInstance()->OnGameplayStart();

        GetInteriorManager()->OnGameplayStart();

        GetHitnRunManager()->ResetState();

        //tick character manager once, to get state system and choreo started
        GetCharacterManager()->PreSimUpdate( 0.0001f );
        GetCharacterManager()->PreSubstepUpdate( 0.0001f );
        GetCharacterManager()->Update( 0.0001f );
        GetCharacterManager()->PostSubstepUpdate( 0.0001f );
        GetCharacterManager()->PostSimUpdate( 0.0001f );

    }

    GetPresentationManager()->OnGameplayStart();

    HeapMgr()->ResetArtStats();
    GetEventManager()->TriggerEvent( EVENT_LEVEL_START );

    if( previousContext != CONTEXT_PAUSE )
    {
        AnimatedCam::CheckPendingCameraSwitch();
    }

    for( int view = 0; view < GetGameplayManager()->GetNumPlayers(); view++ )
    {
        SuperCamCentral* scc = GetSuperCamManager()->GetSCC( view );
        scc->SetIsInitialCamera(true);
    }
    GetGameplayManager()->Update( 0 );
    GetTriggerVolumeTracker()->Update( 0 );

    // Register multiplayer console commands once the console is guaranteed
    // ready (registration earlier in OnStart hung on startup).
    RegisterNetworkConsoleCommands();

    // [DISABLED] LobbyRuntimeTest::Run() — the live Scrooby::App->LoadProject
    // call during active gameplay replaces the running project and causes an
    // access violation, so it is removed from the startup path. Readability of
    // generated .p3d is covered by unit tests instead.
}

//=============================================================================
// GameplayContext::OnStop
//=============================================================================
// Description: Comment
//
// Parameters:  ( ContextEnum nextContext )
//
// Return:      void 
//
//=============================================================================
void GameplayContext::OnStop( ContextEnum nextContext )
{
    GetPresentationManager()->OnGameplayStop();

    // turn off controller rumble, to comply w/ TCR/TRC standards
    //
    GetInputManager()->ToggleRumble( false );


    // Disconnect any active online match when leaving gameplay — but NOT when
    // we are only pausing (ESC opens the Pause context, then returns to
    // gameplay). Tearing down the match on pause made one client stop seeing
    // the other after the pause menu was closed.
    //
    NetworkMatchManager* pMatch = NetworkMatchManager::GetInstance();
    if ( pMatch != NULL && pMatch->IsInMatch() && nextContext != CONTEXT_PAUSE )
    {
        pMatch->Leave();
    }

    // Common to all playing contexts.
    //
    PlayingContext::OnStop( nextContext );
}

//=============================================================================
// GameplayContext::OnUpdate
//=============================================================================
// Description: Comment
//
// Parameters:  ( unsigned int elapsedTime )
//
// Return:      void 
//
//=============================================================================
void GameplayContext::OnUpdate( unsigned int elapsedTime )
{
#ifdef DEBUGWATCH
    mDebugOnUpdateDT = radTimeGetMicroseconds();
#endif

#ifndef FINAL
    if( CommandLineOptions::Get( CLO_PRINT_FRAMERATE ) )
    {
        //
        // Merasure the time taken to render frames 100-500
        //
        const unsigned int startFrame   = 200;
        const unsigned int endFrame     = 600;
        static unsigned int count = 0;
        ++count;
        static unsigned int totalTime = 0;
        totalTime += elapsedTime;

        if( count == startFrame )
        {
            totalTime = 0;
        }
        if( count == endFrame )
        {
            const unsigned int frames = endFrame - startFrame;
            float msPerFrame = totalTime / static_cast< float >( frames );
            rReleasePrintf( "====================================\n" );
            rReleasePrintf( "%d frames rendered in %dms\n", frames, totalTime );
            rReleasePrintf( "%fms per frame\n", msPerFrame );
            rReleasePrintf( "====================================\n" );
        }
    }

    static int frameCount = 60;
    frameCount--;
    if( frameCount == 0 )
    {
        HeapMgr()->ResetArtStats();
    }
#endif

    // hack for yousuf
    if( mSlowMoHack )
    {
        //elapsedTime = 1;
        elapsedTime = 2;
    }

    if( !m_PausedAllButPresentation )
    {
        float timeins = (float)(elapsedTime) / 1000.0f;

        BEGIN_PROFILE("GameplayManager");
        if ( m_state != S_SUSPENDED )
        {
            GetGameplayManager()->Update( elapsedTime );
        }
        END_PROFILE("GameplayManager");


        GetAvatarManager()->Update( timeins );

        GetFootprintManager()->Update( elapsedTime );

	    GetCharacterManager()->GarbageCollect( );

	    BEGIN_PROFILE( "Update Particles" );

	    GetParticleManager()->Update( elapsedTime );
        GetBreakablesManager()->Update( elapsedTime );
	    GetAnimEntityDSGManager()->Update( elapsedTime );

	    END_PROFILE( "Update Particles" );

    #ifdef DEBUGWATCH
        unsigned int t0 = radTimeGetMicroseconds();
    #endif 

    BEGIN_PROFILE("WorldPhysics");
        GetWorldPhysicsManager()->Update(elapsedTime);
    END_PROFILE("WorldPhysics");

        // ordering is important. Unless other parts of code change, we must call
        // this before WorldPhysManager::Update() because PedestrianManager
        // sets the flags for all characters to be updated in WorldPhys Update 
        PedestrianManager::GetInstance()->Update( elapsedTime );

    #ifdef DEBUGWATCH
        mDebugTimeDelta = elapsedTime;
        mDebugPhysTiming = radTimeGetMicroseconds()-t0 ;
    #endif 

    BEGIN_PROFILE("TriggerVolumeTracker");
        GetTriggerVolumeTracker()->Update( elapsedTime );
    END_PROFILE("TriggerVolumeTracker");

    BEGIN_PROFILE("TrafficManager");
        TrafficManager::GetInstance()->Update( elapsedTime );
    END_PROFILE("TrafficManager");

    BEGIN_PROFILE("ActorManager");
        ActorManager::GetInstance()->Update( elapsedTime );
    END_PROFILE("ActorManager");

    BEGIN_PROFILE("InteriorManager");
        GetInteriorManager()->Update( elapsedTime );
    END_PROFILE("InteriorManager");

    BEGIN_PROFILE("SkidmarkManager");
        GetSkidmarkManager()->Update( elapsedTime );
    END_PROFILE("SkidmarkManager");

    BEGIN_PROFILE( "CoinManager" );
        GetCoinManager()->Update( elapsedTime );
        GetSparkleManager()->Update( elapsedTime );
    END_PROFILE( "CoinManager" );

    // Drive the online match (host/join). Only acts if a match is active.
    NetworkMatchManager* pMatch = NetworkMatchManager::GetInstance();
    if (pMatch != NULL && pMatch->IsInMatch())
    {
        pMatch->Update( static_cast<float>( elapsedTime ) / 1000.0f );
        // Reflect remote players' latest states in the world.
        GetRemotePlayerWorldView()->Sync( pMatch->GetRemotePlayers() );
        // Advance each remote player's visual position smoothly toward its
        // target so network refresh does not produce teleports.
        //
        // NOTE: the world-view Update(float dt) contract is SECONDS (its
        // m_smoothingSeconds field and every unit test treat dt as seconds).
        // elapsedTime is MILLISECONDS, so we must normalise before passing it,
        // otherwise alpha = 1 - exp(-lambda*dt) is fed a value ~1000x too large
        // and every remote player's visual position teleports its full distance
        // to the latest reported target on the very first Update — destroying
        // the smoothing this call exists to provide.
        GetRemotePlayerWorldView()->Update( static_cast<float>( elapsedTime ) / 1000.0f );
        // Mirror remote drivers' ghost vehicles (spawn/move/despawn) and step
        // their positions smoothly as well.
        pMatch->GetRemotePlayers().ForEachPlayer(
            []( int playerId, const PlayerNetworkState& state, void* ) -> bool
            {
                GetRemoteVehicleWorldView()->SyncDriver( playerId, state );
                return true;
            },
            NULL );
        // Drop ghost vehicles of remote players who are no longer connected.
        GetRemoteVehicleWorldView()->Prune( pMatch->GetRemotePlayers() );
        // Ms -> seconds, as with the player view above (see the NOTE there).
        GetRemoteVehicleWorldView()->Update( static_cast<float>( elapsedTime ) / 1000.0f );

        // Surface join/leave notifications as console messages.
        std::vector<NetworkMatchManager::JoinLeaveEvent> events;
        pMatch->DrainJoinLeaveEvents( events );
        for ( size_t i = 0; i < events.size(); ++i )
        {
            if ( events[i].joined )
            {
                GetConsole()->Printf( "net: player %u connected", events[i].clientId );
            }
            else
            {
                GetConsole()->Printf( "net: player %u disconnected", events[i].clientId );
            }
        }
    }

    BEGIN_PROFILE( "HitnRunManager" );
        GetHitnRunManager()->Update( elapsedTime );
    END_PROFILE( "HitnRunManager" );
    }

#ifdef RAD_DEMO
    GetGameplayManager()->UpdateIdleTime( elapsedTime );
#endif

    BEGIN_PROFILE("PresentationManager");
       GetPresentationManager()->Update( elapsedTime );
    END_PROFILE("PresentationManager");

    // Common to all playing contexts.
    //
    PlayingContext::OnUpdate( elapsedTime );
#ifdef DEBUGWATCH
    mDebugOnUpdateDT = radTimeGetMicroseconds()-mDebugOnUpdateDT;
#endif
}

//=============================================================================
// GameplayContext::OnSuspend
//=============================================================================
// Description: Comment
//
// Parameters:  ()
//
// Return:      void 
//
//=============================================================================
void GameplayContext::OnSuspend()
{
    // Common to all playing contexts.
    //
    PlayingContext::OnSuspend();
}

//=============================================================================
// GameplayContext::OnResume
//=============================================================================
// Description: Comment
//
// Parameters:  ()
//
// Return:      void 
//
//=============================================================================
void GameplayContext::OnResume()
{
    // Common to all playing contexts.
    //
    PlayingContext::OnResume();
}

//=============================================================================
// GameplayContext::PauseAllButPresentation
//=============================================================================
// Description: Comment
//
// Parameters:  ()
//
// Return:      void 
//
//=============================================================================
void GameplayContext::PauseAllButPresentation( const bool pause )
{
    m_PausedAllButPresentation = pause;
}

