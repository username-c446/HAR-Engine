#ifndef NETWORKPUPPETCONTROLLER_H
#define NETWORKPUPPETCONTROLLER_H

#include <worldsim/character/charactercontroller.h>

// NetworkPuppetController
// -----------------------
// Drives the character that shows another player (a network puppet) with that
// player's replicated input, so the game's own locomotion pipeline animates it
// exactly like the local player: Character::UpdateDesiredDirAndSpeed reads the
// direction, the walker picks the walk / run cycle from the speed, Loco
// sequences the jump from the Jump intention and turbo from Dash.
//
// It is an NPCController because the puppet is an NPC character and game code
// casts an NPC's controller to NPCController without checking; none of the NPC
// behaviour (paths, panic, dodging) runs — Update does nothing.
class NetworkPuppetController : public NPCController
{
public:
    NetworkPuppetController( void );
    virtual ~NetworkPuppetController( void );

    // The shown network pose: horizontal velocity (m/s), turbo, airborne flags.
    // `maxSpeed` is the character's current maximum (NPC input scale is 1).
    void SetMotion( float vx, float vz, float maxSpeed, bool turbo, bool jumping, bool airborne );
    bool IsJumping( void ) const { return mJumping; }
    bool IsAirborne( void ) const { return mAirborne; }
    // The sender took off: Loco starts the jump action on the next sequence.
    void RequestJump( void );

    virtual void  Update( float seconds );
    virtual void  GetDirection( rmt::Vector& outDirection );
    virtual float GetValue( int buttonId ) const;
    virtual bool  IsButtonDown( int buttonId ) const;

private:
    rmt::Vector mDirection;
    bool        mTurbo;
    bool        mJumping;
    bool        mAirborne;
};

#endif
