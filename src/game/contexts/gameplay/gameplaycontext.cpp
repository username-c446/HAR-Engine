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
#include <map>
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
#include <worldsim/parkedcars/parkedcarmanager.h>
#include <worldsim/character/networkpuppetcontroller.h>
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

    // Deliberate discontinuities of the local player's position (respawn,
    // interior / mission teleports) are declared to the server's movement
    // validator through PlayerNetworkState::teleportSeq. The game teleports the
    // avatar from many places, so the provider detects it: a jump between two
    // consecutive samples that no vehicle or runner can make. A server
    // correction is not a teleport of ours (the server already knows the pose).
    const float kTeleportJumpMeters = 25.0f;
    struct LocalTeleportTracker
    {
        bool     hasLast;
        float    x, y, z;
        uint8_t  seq;
        bool     skipNext;
    } s_teleport = { false, 0.0f, 0.0f, 0.0f, 0, false };

    void TrackLocalTeleport( float x, float y, float z, bool inVehicle, float dtSeconds )
    {
        if ( s_teleport.hasLast && !s_teleport.skipNext )
        {
            float dx = x - s_teleport.x, dy = y - s_teleport.y, dz = z - s_teleport.z;
            float reach = ( inVehicle ? 110.0f : 20.0f ) * dtSeconds + kTeleportJumpMeters;
            if ( dx * dx + dy * dy + dz * dz > reach * reach )
            {
                ++s_teleport.seq;
            }
        }
        s_teleport.skipNext = false;
        s_teleport.hasLast = true;
        s_teleport.x = x; s_teleport.y = y; s_teleport.z = z;
    }

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
        TrackLocalTeleport( pos.x, pos.y, pos.z, avatar->IsInCar(), 0.1f );
        outState.teleportSeq = s_teleport.seq;
        outState.vehDamage = 0;

        // Facing angle in radians (rotY used to carry heading.y — the vertical
        // component of the direction vector, i.e. ~0).
        outState.rotY = avatar->GetCharacter() != NULL ? avatar->GetCharacter()->GetFacingDir() : 0.0f;

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
            float wreck = ( 1.0f - vehicle->GetHealthFraction() ) * 255.0f;
            outState.vehDamage = static_cast<uint8_t>( wreck < 0.0f ? 0.0f : ( wreck > 255.0f ? 255.0f : wreck ) );
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

    // How a remote player looks: its replicated appearance (PlayerPresence),
    // or the default character until that record arrives (then restyled).
    void RemoteAppearanceOf( int playerId, const char*& model, const char*& anim )
    {
        model = "homer";
        anim = "homer";
        const net::PlayerAppearance* app =
            NetworkMatchManager::Presence().Appearance( static_cast<net::PeerId>( playerId ) );
        if ( app != NULL )
        {
            model = app->model;
            if ( app->anim[0] != '\0' ) anim = app->anim;
        }
    }

    // A character that shows something another player's game simulates (its
    // player, its pedestrians): in the world scene, driven by the owner's
    // replicated motion through NetworkPuppetController and animated by the
    // game's own locomotion (walk / run / jump exactly like the local player).
    void MakeNetworkPuppet( Character* c )
    {
        // AddCharacter does NOT put NPCs into the world scene (only PCs get
        // AddToWorldScene there), so an NPC would exist but never render; in
        // its default state an NPC is static (the bind / "T" pose): LOCO runs
        // its locomotion drivers.
        c->AddToWorldScene();
        NetworkPuppetController* ctrl = new( GMA_LEVEL_OTHER ) NetworkPuppetController;
        c->SetController( ctrl );
        ctrl->SetCharacter( c );
        c->SetNetworkPuppet( true );
        CharacterAi::StateManager* sm = c->GetStateManager();
        if ( sm != NULL && sm->GetState() != CharacterAi::LOCO )
        {
            sm->SetState<CharacterAi::Loco>();
        }
    }

    // World sink for remote players: spawn/move/despawn lightweight NPCs that
    // mirror what other network players report through their PlayerNetworkState.
    void* RemotePlayerSpawn( int playerId )
    {
        char name[32];
        sprintf( name, "mp_remote_%d", playerId );
        const char* model = NULL;
        const char* anim = NULL;
        RemoteAppearanceOf( playerId, model, anim );
        Character* c = GetCharacterManager()->AddCharacter(
            CharacterManager::NPC, name, model, anim, NULL );
        if ( c == NULL )
        {
            rReleasePrintf( "net: could not spawn remote player %d\n", playerId );
            return NULL;
        }
        MakeNetworkPuppet( c );
        return c;
    }

    NetworkPuppetController* PuppetControllerOf( Character* c )
    {
        return ( c != NULL && c->IsNetworkPuppet() ) ? static_cast<NetworkPuppetController*>( c->GetController() ) : NULL;
    }

    void RemotePlayerMove( int playerId, void* handle, const PlayerNetworkState& state, const RemoteMotion& motion )
    {
        (void)playerId;
        Character* c = static_cast<Character*>( handle );
        if ( c == NULL ) return;

        // The puppet IS the interpolated network pose: placed exactly, every
        // frame (this runs after the world physics update, so it is the last
        // write before rendering). Locomotion only animates it — with the
        // pose's measured velocity, so the animation's own advance matches the
        // next interpolated pose and nothing has to be corrected. (Driving the
        // position by locomotion at a guessed speed and snapping it back when
        // it drifted was the rubber-banding.)
        c->SetPosition( rmt::Vector( state.x, state.y, state.z ) );

        // The game animates it from the owner's input: the measured velocity
        // of the shown pose (walk / run cycle), turbo, and whether it is in the
        // air (the jump action lands when the owner did).
        const bool jumping = ( state.flags & PlayerNetworkState::FLAG_JUMPING ) != 0;
        const bool airborne = ( state.flags & ( PlayerNetworkState::FLAG_JUMPING | PlayerNetworkState::FLAG_IN_AIR ) ) != 0;
        c->SetNetworkAirborne( airborne );
        NetworkPuppetController* ctrl = PuppetControllerOf( c );
        if ( ctrl == NULL ) return;
        const bool walking = state.characterState == static_cast<uint8_t>( CharacterAi::LOCO );
        ctrl->SetMotion( walking ? motion.vx : 0.0f, walking ? motion.vz : 0.0f, c->GetMaxSpeed(),
                         ( state.flags & PlayerNetworkState::FLAG_LOCO_TURBO ) != 0, jumping, airborne );
        if ( !walking || motion.HorizontalSpeed() < 0.15f )
        {
            // Standing: no direction input, so take the owner's facing.
            c->SetFacingDir( state.facingAngle / 1000.0f );
        }
    }

    // The remote player's appearance changed (costume, character): swap the
    // puppet's model and animation bank in place.
    void RemotePlayerRestyle( int playerId, void* handle )
    {
        Character* c = static_cast<Character*>( handle );
        if ( c == NULL ) return;
        const char* model = NULL;
        const char* anim = NULL;
        RemoteAppearanceOf( playerId, model, anim );
        GetCharacterManager()->SwapData( c, model, anim );
    }

    // Publishes where the local player is and how it looks (PlayerPresence:
    // replicated only when it changes).
    void PublishLocalPresence()
    {
        AvatarManager* am = GetAvatarManager();
        Avatar* avatar = am != NULL ? am->GetAvatarForPlayer( 0 ) : NULL;
        if ( avatar == NULL || avatar->GetCharacter() == NULL ) return;

        net::PlayerContext ctx;
        ctx.level = static_cast<uint8_t>( GetGameplayManager()->GetCurrentLevelIndex() );
        ctx.zone = GetInteriorManager()->IsInside() ? static_cast<uint64_t>( GetInteriorManager()->GetInterior() ) : 0;
        NetworkMatchManager::Presence().SetLocalContext( ctx );

        net::PlayerAppearance app;
        const char* model = GetCharacterManager()->GetModelName( avatar->GetCharacter() );
        const char* anim = GetCharacterManager()->GetAnimName( avatar->GetCharacter() );
        if ( app.Set( model, anim ) )
        {
            NetworkMatchManager::Presence().SetLocalAppearance( app );
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
        NetworkPuppetController* ctrl = PuppetControllerOf( c );
        JumpAction* jump = c->GetJumpLocomotionAction();
        if ( ctrl == NULL || jump == NULL || jump->IsInJump() ) return;

        if ( ctrl->IsJumping() )
        {
            // A jump: Loco sequences it from the intention, as for a pressed button.
            ctrl->RequestJump();
        }
        else
        {
            // Walked off a ledge: the game's own fall (as Character does for a
            // character that loses the ground).
            jump->Reset( 0.0f, true );
            c->GetActionController()->Clear();
            Sequencer* seq = c->GetActionController()->GetNextSequencer();
            seq->BeginSequence();
            seq->AddAction( jump );
            seq->EndSequence();
        }
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

        NetworkPuppetController* ctrl = PuppetControllerOf( c );
        JumpAction* jump = c->GetJumpLocomotionAction();
        const bool inJump = jump != NULL && jump->IsInJump();
        switch ( actionId )
        {
        // Airborne moves go through the puppet's own jump action, as for the
        // local player: it owns the clip and stops it on landing (an overlaid
        // clip kept playing after the landing and sank the puppet into the
        // ground until it ended).
        case PlayerNetworkState::ACTION_JUMP: // double jump
            if ( ctrl != NULL && inJump ) ctrl->RequestJump();
            break;
        case PlayerNetworkState::ACTION_JUMPKICK:
        case PlayerNetworkState::ACTION_SLAM:
            if ( ctrl != NULL && inJump ) ctrl->RequestAirAttack();
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
        v->ApplyNetworkDamage( 1.0f - state.vehDamage / 255.0f );
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

    // Another player's pedestrian: a puppet character of its model (or the
    // stand-in "npd" if that model is not in our ped groups), moved by the
    // owner's stream; its walk cycle follows the measured speed of the shown
    // pose. Kept apart from the ped manager's own pool.
    struct PedGhost
    {
        rmt::Vector  last;
        unsigned int lastMs;
        bool         primed;
        bool         knocked;
    };
    std::map<Character*, PedGhost> s_pedGhosts;

    void* NetEntitySpawnPedestrian( uint64_t id, const net::NetEntitySpawn& spawn )
    {
        // Leave room for the characters missions and the ped manager add.
        const int kReservedSlots = 12;
        if ( GetCharacterManager()->NumFreeNPCSlots() <= kReservedSlots ) return NULL; // retried later
        const char* model = PedestrianManager::GetInstance()->IsModelRegistered( spawn.model ) ? spawn.model : "npd";
        char name[32];
        sprintf( name, "mp_ped_%08x", static_cast<unsigned int>( id ^ ( id >> 32 ) ) );
        Character* c = GetCharacterManager()->AddCharacter( CharacterManager::NPC, name, model, "npd", NULL );
        if ( c == NULL ) return NULL;
        MakeNetworkPuppet( c );
        c->SetPosition( rmt::Vector( spawn.x, spawn.y, spawn.z ) );
        c->SetFacingDir( spawn.yaw );
        PedGhost g;
        g.last.Set( spawn.x, spawn.y, spawn.z );
        g.lastMs = radTimeGetMilliseconds();
        g.primed = false;
        g.knocked = false;
        s_pedGhosts[ c ] = g;
        return c;
    }

    void NetEntityMovePedestrian( Character* c, PedGhost& g, const net::NetEntityMotion& m )
    {
        rmt::Vector pos( m.x, m.y, m.z );
        c->SetPosition( pos );
        const bool knocked = ( m.flags & net::NetEntityMotion::MF_KNOCKED ) != 0;
        if ( knocked && !g.knocked )
        {
            RemotePlayClip( c, "flail", true ); // the owner's ped was sent flying
        }
        g.knocked = knocked;

        // Velocity of the shown (interpolated) pose between two frames.
        unsigned int now = radTimeGetMilliseconds();
        float dt = static_cast<float>( now - g.lastMs ) / 1000.0f;
        float vx = 0.0f, vz = 0.0f;
        if ( g.primed && dt > 0.0001f && dt < 0.5f )
        {
            vx = ( pos.x - g.last.x ) / dt;
            vz = ( pos.z - g.last.z ) / dt;
        }
        g.last = pos;
        g.lastMs = now;
        g.primed = true;

        c->SetNetworkAirborne( knocked );
        NetworkPuppetController* ctrl = PuppetControllerOf( c );
        if ( ctrl == NULL ) return;
        if ( knocked ) { vx = 0.0f; vz = 0.0f; }
        ctrl->SetMotion( vx, vz, c->GetMaxSpeed(), false, false, knocked );
        if ( vx * vx + vz * vz < 0.15f * 0.15f )
        {
            c->SetFacingDir( m.yaw ); // standing: the owner's facing
        }
    }

    // Samples one of OUR pedestrians: pose, facing / motion direction, speed,
    // and whether it was knocked down (in simulation or out of locomotion).
    bool NetEntitySamplePedestrian( Character* c, net::NetEntityMotion& out )
    {
        if ( c == NULL ) return false;
        rmt::Vector p, v;
        c->GetPosition( p );
        c->GetVelocity( v );
        out.x = p.x; out.y = p.y; out.z = p.z;
        float speed = rmt::Sqrt( v.x * v.x + v.z * v.z );
        out.yaw = c->GetFacingDir();
        float kmh = speed * 3.6f;
        out.speedKmh = static_cast<uint16_t>( kmh > 65535.0f ? 65535.0f : kmh );
        out.steer = 0;
        out.flags = 0;
        bool inSim = c->GetSimState() != NULL && c->GetSimState()->GetControl() == sim::simSimulationCtrl;
        CharacterAi::StateManager* sm = c->GetStateManager();
        bool inLoco = sm != NULL && sm->GetState() == CharacterAi::LOCO;
        if ( inSim || !inLoco ) out.flags |= net::NetEntityMotion::MF_KNOCKED;
        out.damage = 0;
        out.state = sm != NULL ? static_cast<uint8_t>( sm->GetState() ) : 0;
        return true;
    }

    void* NetEntitySpawnGhost( uint64_t id, const net::NetEntitySpawn& spawn )
    {
        if ( spawn.kind == net::NEK_PEDESTRIAN ) return NetEntitySpawnPedestrian( id, spawn );
        // A parked car another player pushes: our own copy follows their stream.
        if ( spawn.kind == net::NEK_PARKED_CAR ) return ParkedCarManager::NetDriveBound( id );
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
        std::map<Character*, PedGhost>::iterator ped = s_pedGhosts.find( static_cast<Character*>( handle ) );
        if ( ped != s_pedGhosts.end() )
        {
            NetEntityMovePedestrian( ped->first, ped->second, m );
            return;
        }
        Vehicle* v = static_cast<Vehicle*>( handle );
        if ( v == NULL ) return;
        float steerRad = ( m.steer / 127.0f ) * 45.0f * ( rmt::PI / 180.0f );
        ApplyGhostKinematics( v, m.x, m.y, m.z, m.yaw, static_cast<float>( m.speedKmh ), steerRad,
                              ( m.flags & net::NetEntityMotion::MF_BRAKE ) != 0,
                              ( m.flags & net::NetEntityMotion::MF_REVERSE ) != 0,
                              ( m.flags & net::NetEntityMotion::MF_DESTROYED ) != 0 );
        v->ApplyNetworkDamage( 1.0f - m.damage / 255.0f );
    }

    void NetEntityDespawnGhost( void* handle, uint8_t kind )
    {
        if ( handle == NULL ) return;
        if ( kind == net::NEK_PEDESTRIAN )
        {
            Character* c = static_cast<Character*>( handle );
            s_pedGhosts.erase( c );
            GetCharacterManager()->RemoveCharacter( c );
            return;
        }
        Vehicle* v = static_cast<Vehicle*>( handle );
        if ( kind == net::NEK_PARKED_CAR )
        {
            ParkedCarManager::NetReleaseBound( v ); // our car, back to our physics
            return;
        }
        s_explodedGhosts.erase( v );
        // See RemoteVehicleDespawn: the teardown releases (and frees) the ghost.
        GetVehicleCentral()->RemoveNetworkGhostVehicle( v );
    }

    // We won the claim on a traffic car another player released: drop the
    // ghost and continue the car as our own traffic at the same pose / speed.
    void* NetEntityAdoptGhost( uint64_t id, void* ghostHandle, const net::NetEntitySpawn& spawn,
                               const net::NetEntityMotion& m )
    {
        (void)id;
        if ( spawn.kind != net::NEK_TRAFFIC_CAR ) return NULL;
        Vehicle* adopted = TrafficManager::GetInstance()->AdoptNetworkTraffic(
            spawn.model, rmt::Vector( m.x, m.y, m.z ), m.yaw, m.speedKmh / 3.6f );
        if ( adopted == NULL ) return NULL; // the ghost stays; the entity goes back up for grabs
        NetEntityDespawnGhost( ghostHandle, spawn.kind );
        return adopted;
    }

    // Another player's game hit one of OUR entities: their car one of our
    // cars, their kick one of our pedestrians (applied as the game's own kick).
    void NetEntityHitLocal( void* localHandle, uint8_t kind, const net::NetEntityHit& hit )
    {
        if ( localHandle == NULL ) return;
        if ( kind == net::NEK_PEDESTRIAN )
        {
            NPCharacter* ped = static_cast<NPCharacter*>( static_cast<Character*>( localHandle ) );
            rmt::Vector dir( hit.ix, hit.iy, hit.iz );
            float force = dir.Magnitude();
            if ( force <= 0.0f ) return;
            dir.Scale( 1.0f / force );
            ped->SetHasBeenHit( true );
            if ( ped->GetStateManager()->GetState() == CharacterAi::LOCO )
            {
                ped->ApplyKickForce( dir, force );
            }
            else
            {
                ped->ApplyForce( dir, force );
            }
            return;
        }
        Vehicle* v = static_cast<Vehicle*>( localHandle );
        v->ApplyNetworkHit( rmt::Vector( hit.ix, hit.iy, hit.iz ), hit.damage / 255.0f );
    }

    bool NetEntitySampleTraffic( void* handle, net::NetEntityMotion& out );

    // Samples one of OUR entities for the stream.
    bool NetEntitySample( void* handle, uint8_t kind, net::NetEntityMotion& out )
    {
        if ( kind == net::NEK_PEDESTRIAN ) return NetEntitySamplePedestrian( static_cast<Character*>( handle ), out );
        return NetEntitySampleTraffic( handle, out ); // traffic, pushed parked cars
    }

    // Samples one of OUR cars for the entity stream.
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
        float wreck = ( 1.0f - v->GetHealthFraction() ) * 255.0f;
        out.damage = static_cast<uint8_t>( wreck < 0.0f ? 0.0f : ( wreck > 255.0f ? 255.0f : wreck ) );
        out.state = 0;
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

    int RemoteDriverOfGhost( void* ghost )
    {
        return GetRemoteVehicleWorldView()->FindDriver( ghost );
    }

    int RemotePlayerOfPuppet( void* puppet )
    {
        return GetRemotePlayerWorldView()->FindPlayer( puppet );
    }

    // The server refused our movement: put the local player back where the
    // server last accepted it.
    void GameplayStateCorrection( const PlayerNetworkState& st )
    {
        AvatarManager* am = GetAvatarManager();
        Avatar* avatar = am != NULL ? am->GetAvatarForPlayer( 0 ) : NULL;
        if ( avatar == NULL ) return;
        rmt::Vector pos( st.x, st.y, st.z );
        Vehicle* v = avatar->GetVehicle();
        if ( avatar->IsInCar() && v != NULL )
        {
            rmt::Matrix m;
            m.Identity();
            m.FillRotateXYZ( 0.0f, v->GetFacingInRadians(), 0.0f );
            m.FillTranslate( pos );
            v->SetTransform( m );
        }
        else if ( avatar->GetCharacter() != NULL )
        {
            avatar->GetCharacter()->SetPosition( pos );
        }
        s_teleport.skipNext = true; // not a teleport of ours
    }

    // PvP: another player hit us.
    void GameplayPlayerHit( unsigned short fromPeer, unsigned char kind, const net::NetEntityHit& hit )
    {
        (void)fromPeer;
        AvatarManager* am = GetAvatarManager();
        Avatar* avatar = am != NULL ? am->GetAvatarForPlayer( 0 ) : NULL;
        if ( avatar == NULL ) return;
        if ( kind == NetworkMatchManager::PE_VEHICLE_HIT )
        {
            Vehicle* v = avatar->GetVehicle();
            if ( avatar->IsInCar() && v != NULL )
            {
                v->ApplyNetworkHit( rmt::Vector( hit.ix, hit.iy, hit.iz ), hit.damage / 255.0f );
            }
        }
        else if ( kind == NetworkMatchManager::PE_KNOCKDOWN )
        {
            if ( !avatar->IsInCar() && avatar->GetCharacter() != NULL )
            {
                avatar->GetCharacter()->ApplyNetworkKnockdown();
            }
        }
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
        entitySink.adopt = &NetEntityAdoptGhost;
        entitySink.hit = &NetEntityHitLocal;
        NetworkMatchManager::Entities().SetSink( entitySink );
        NetworkMatchManager::Entities().SetSampler( &NetEntitySample );
        // Only entities around us need a ghost (collision areas are scarce).
        NetworkMatchManager::Entities().SetRelevanceRadius( 200.0f );
        // Migration: a released car is taken over by a player near it, within
        // our own traffic budget.
        NetworkMatchManager::Entities().SetPresence(
            []( float& x, float& z ) -> bool
            {
                AvatarManager* am = GetAvatarManager();
                Avatar* avatar = am != NULL ? am->GetAvatarForPlayer( 0 ) : NULL;
                if ( avatar == NULL ) return false;
                rmt::Vector p;
                avatar->GetPosition( p );
                x = p.x; z = p.z;
                return true;
            },
            []( float x, float z, float radius ) -> bool
            {
                NetworkMatchManager* m = NetworkMatchManager::GetInstance();
                return m != NULL && m->IsRemotePlayerNear( x, z, radius );
            },
            []( uint8_t kind, float x, float y, float z ) -> bool
            {
                if ( kind != net::NEK_TRAFFIC_CAR ) return false;
                return TrafficManager::GetInstance()->WouldKeepNetworkTraffic( rmt::Vector( x, y, z ) );
            } );
    }
    pMatchMgr->SetStateCorrectionHandler( &GameplayStateCorrection );
    pMatchMgr->SetPlayerHitHandler( &GameplayPlayerHit );
    pMatchMgr->SetRemoteLookups( &RemoteDriverOfGhost, &RemotePlayerOfPuppet );

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
    sink.restyle = &RemotePlayerRestyle;
    GetRemotePlayerWorldView()->SetSink( sink );
    // A remote player is shown only in our level / interior, in its own
    // replicated appearance.
    GetRemotePlayerWorldView()->SetPresence(
        []( int playerId ) { return NetworkMatchManager::Presence().SharesContext( static_cast<net::PeerId>( playerId ) ); },
        []( int playerId ) { return NetworkMatchManager::Presence().AppearanceVersion( static_cast<net::PeerId>( playerId ) ); } );
    GetRemoteVehicleWorldView()->SetVisibility(
        []( int playerId ) { return NetworkMatchManager::Presence().SharesContext( static_cast<net::PeerId>( playerId ) ); } );

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
        ParkedCarManager::GetInstance().UpdateNetworkPushes( elapsedTime );
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
        if ( !pMatch->IsInMatch() )
        {
            // The match just ended on its own (refused / connection lost).
            if ( pMatch->GetLastRefusal() != net::REFUSE_NONE )
            {
                GetConsole()->Printf( "net: the server refused us (reason %d)", (int)pMatch->GetLastRefusal() );
            }
            else if ( pMatch->WasConnectionLost() )
            {
                GetConsole()->Printf( "net: connection to the server lost" );
            }
            GetRemotePlayerWorldView()->Clear();
            GetRemoteVehicleWorldView()->Clear();
        }
        else
        {
        PublishLocalPresence();
        // Remote players are rendered behind the server clock (snapshot
        // interpolation), so their motion does not depend on packet timing.
        {
            RemoteStateTrack::Timing timing;
            timing.adaptive = true; // distant players arrive at a lower rate
            timing.clock = []() -> uint32_t
            {
                NetworkMatchManager* m = NetworkMatchManager::GetInstance();
                net::Session* s = m != NULL ? m->GetSession() : NULL;
                return s != NULL ? s->ServerTimeMs() : 0;
            };
            GetRemotePlayerWorldView()->SetTiming( timing );
            GetRemoteVehicleWorldView()->SetTiming( timing );
        }
        // Reflect remote players' latest states in the world.
        GetRemotePlayerWorldView()->Sync( pMatch->GetRemotePlayers() );
        // Move each remote player to its interpolated pose.
        GetRemotePlayerWorldView()->Update( static_cast<float>( elapsedTime ) / 1000.0f );
        // Mirror remote drivers' ghost vehicles (spawn/move/despawn).
        pMatch->GetRemotePlayers().ForEachPlayer(
            []( int playerId, const PlayerNetworkState& state, void* user ) -> bool
            {
                const RemotePlayerManager* players = static_cast<const RemotePlayerManager*>( user );
                GetRemoteVehicleWorldView()->SyncDriver( playerId, state, players->GetPlayerTime( playerId ) );
                return true;
            },
            const_cast<RemotePlayerManager*>( &pMatch->GetRemotePlayers() ) );
        // Drop ghost vehicles of remote players who are no longer connected.
        GetRemoteVehicleWorldView()->Prune( pMatch->GetRemotePlayers() );
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
        } // still in the match
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

