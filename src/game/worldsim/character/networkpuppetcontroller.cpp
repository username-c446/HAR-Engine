#include <worldsim/character/networkpuppetcontroller.h>

NetworkPuppetController::NetworkPuppetController( void )
    : mTurbo( false ),
      mJumping( false ),
      mAirborne( false )
{
    mDirection.Set( 0.0f, 0.0f, 0.0f );
}

NetworkPuppetController::~NetworkPuppetController( void )
{
}

void NetworkPuppetController::SetMotion( float vx, float vz, float maxSpeed, bool turbo, bool jumping, bool airborne )
{
    // NPC desired speed = |direction| * maxSpeed (input scale 1, no curve), so
    // the direction's length is the speed as a fraction of the maximum.
    const float kStillSpeed = 0.15f; // m/s: below this the player stands
    float speed = rmt::Sqrt( vx * vx + vz * vz );
    if ( speed < kStillSpeed || maxSpeed <= 0.0f )
    {
        mDirection.Set( 0.0f, 0.0f, 0.0f );
    }
    else
    {
        float fraction = speed / maxSpeed;
        if ( fraction > 1.0f ) fraction = 1.0f;
        mDirection.Set( vx / speed * fraction, 0.0f, vz / speed * fraction );
    }
    mTurbo = turbo;
    mJumping = jumping;
    mAirborne = airborne;
}

void NetworkPuppetController::RequestJump( void )
{
    SetIntention( CharacterController::Jump );
}

void NetworkPuppetController::Update( float seconds )
{
    (void)seconds; // no paths, panic or dodging: the network decides
}

void NetworkPuppetController::GetDirection( rmt::Vector& outDirection )
{
    outDirection = mDirection;
}

float NetworkPuppetController::GetValue( int buttonId ) const
{
    return IsButtonDown( buttonId ) ? 1.0f : 0.0f;
}

bool NetworkPuppetController::IsButtonDown( int buttonId ) const
{
    return buttonId == CharacterController::Dash && mTurbo;
}
