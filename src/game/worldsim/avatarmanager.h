//=============================================================================
// Copyright (C) 2002 Radical Entertainment Ltd.  All rights reserved.
//
// File:        avatarmanager.h
//
// Description: Blahblahblah
//
// History:     4/3/2002 + Created -- NAME
//
//=============================================================================

#ifndef AVATARMANAGER_H
#define AVATARMANAGER_H

//========================================
// Nested Includes
//========================================
#include <worldsim/avatar.h>
#include <events/eventlistener.h>

#include <constants/maxplayers.h>

//========================================
// Forward References
//========================================

class Character;

//=============================================================================
//
// Synopsis:    Blahblahblah
//
//=============================================================================

class AvatarManager
:
public EventListener
{
public:
    AvatarManager();
	~AvatarManager();
    void Destroy( void );

	static AvatarManager* GetInstance( );
	static AvatarManager* CreateInstance( );
    static void DestroyInstance( );
    
    void EnterGame( void );
    void ExitGame( void );
    virtual void HandleEvent( EventEnum id, void* pEventData );

    Avatar* GetAvatarForPlayer( int playerId );
    Avatar* GetAvatarForVehicleId( int vehicleId );
    
    Avatar* GetAvatarForVehicle(Vehicle* vehicle);

    Avatar* FindAvatarForCharacter( Character* pCharacter );

    // Number of currently active player avatars in this process. This is the
    // multiplayer-aware iteration bound: NPC/traffic systems should loop
    // `0..GetNumAvatars()-1` instead of hard-coding `GetAvatarForPlayer(0)` so
    // the world reacts around every connected player, not just player 0.
    int GetNumAvatars() const { return mNumAvatars; }

    // Nearest active avatar to a world position (or NULL if none). Used by AI
    // that must pick "the player to target/react to" when there are several
    // players in the match. Optionally ignores a specific id (e.g. the caller's
    // own avatar) so a reaction targets another player.
    Avatar* FindNearestAvatar( const rmt::Vector& position, int ignorePlayerId = -1 );
    
    void PutCharacterInCar( Character* pCharacter, Vehicle* pVehicle );
    void PutCharacterOnGround( Character* pCharacter, Vehicle* pVehicle );

    //chuck returns a true if the players character is in get in car or get out of car transition state.
    bool IsAvatarGettingInOrOutOfCar(int playerId);

    void Update(float dt);
    

private:
    
    //Prevent wasteful constructor creation.
	AvatarManager( const AvatarManager& avatarmanager );
	AvatarManager& operator=( const AvatarManager& avatarmanager );

    // Data.
    //
    static AvatarManager* spAvatarManager;

    static const int MAX_AVATARS = MAX_PLAYERS;

    Avatar* mAvatarArray[ MAX_AVATARS ];
    int mNumAvatars;
};

// A little syntactic sugar for getting at this singleton.
inline AvatarManager* GetAvatarManager() { return( AvatarManager::GetInstance() ); }

#endif //AVATARMANAGER_H
