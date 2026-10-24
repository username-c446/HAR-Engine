//=============================================================================
// Copyright (C) 2002 Radical Entertainment Ltd.  All rights reserved.
//
// File:        parkedcarmanager.h
//
// Description: Blahblahblah
//
// History:     2/6/2003 + Created -- NAME
//
//=============================================================================

#ifndef PARKEDCARMANAGER_H
#define PARKEDCARMANAGER_H

//========================================
// Nested Includes
//========================================
#include <p3d/p3dtypes.hpp>

#include <events/eventlistener.h>

#include <worldsim/vehiclecentral.h>

#include <loading/loadingmanager.h>

//========================================
// Forward References
//========================================
class Vehicle;
namespace net { struct WorldStateValue; }
class CarStartLocator;

//=============================================================================
//
// Synopsis:    Blahblahblah
//
//=============================================================================

class ParkedCarManager : public EventListener, public LoadingManager::ProcessRequestsCallback
{
public:
    enum 
    { 
        MAX_DIFFERENT_CARS = 5,
        MAX_CAR_NAME_LEN = 32,
        MAX_LOCATORS_PER_ZONE = 50,
        MAX_ZONES = 3,
        NUM_TEST_PARKED_CARS = VehicleCentral::MAX_ACTIVE_VEHICLES - 6
    };

    static ParkedCarManager& GetInstance();
    static void DestroyInstance();

    //From EventListener
    virtual void HandleEvent( EventEnum id, void* pEventData );

    // ---- networked match: pushed parked cars (P3) ----
    // Our own simulation (our car / traffic) hit this parked car: we move it
    // for everyone (an entity bound to the car; the first mover wins).
    void OnPushedLocally( Vehicle* car );
    // Per frame: a pushed car we move that came to rest publishes its pose
    // (WOC_POSE) and stops streaming.
    void UpdateNetworkPushes( unsigned int elapsedMs );
    // Entity sink side: another player moves this car of ours (bound id).
    static void* NetDriveBound( uint64_t boundId );
    static void  NetReleaseBound( void* car );

    //From LoadingManager
    virtual void OnProcessRequestsComplete( void* pUserData );   

    //Local
    void AddCarType( const char* name );
    void AddLocator( CarStartLocator* loc );

    void AddFreeCar( const char* name, CarStartLocator* loc );
    void RemoveFreeCar();
    void RemoveFreeCarIfClose( const rmt::Vector& position );
    
    //Chuck: release the parkcar instances so Gameplaymanager can free up the art
    void MDKParkedCars();

    void EnableParkedCars() {mParkedCarsEnabled = true;}
    void DisableParkedCars() {mParkedCarsEnabled = false;}

private:
    static ParkedCarManager* spInstance;

    struct ParkedCarInfo
    {
        ParkedCarInfo() : 
            mCar( NULL ), 
            mHusk( NULL ),
            mLoadedZoneUID( 0 ),
            mNetLoc( -1 )
            //mActiveListIndex( -1 ) 
        { mName[0] ='\0'; };

        char mName[ MAX_CAR_NAME_LEN + 1];
        Vehicle* mCar;
        Vehicle* mHusk;
        tUID mLoadedZoneUID;
        int  mNetLoc;   // locator index in its zone while parked (-1: not parked)
        //int mActiveListIndex; 
    };
    
    //Where all the parked cars are
    ParkedCarInfo* mParkedCars;

    // ---- network: server-authoritative parked cars -------------------------
    // The placement of a zone (which car type on which locator, colour) is a
    // first-write world-state record: the first client to load the zone
    // decides and publishes it, everyone else (and late joiners) uses it, and a
    // client that lost the race re-places from the authoritative plan. A car a
    // player drives away is marked taken so other clients remove their copy.
    struct NetZone
    {
        NetZone() : mZone( 0 ), mNumLocs( 0 ) {}
        tUID         mZone;
        unsigned int mNumLocs;
        rmt::Vector  mPos[ MAX_LOCATORS_PER_ZONE ];
        float        mRot[ MAX_LOCATORS_PER_ZONE ];
    };
    NetZone mNetZones[ MAX_ZONES ];

    static void NetApplyZone( void* handle, unsigned char channel, const net::WorldStateValue& value, bool erased );
    static void NetApplyCar( void* handle, unsigned char channel, const net::WorldStateValue& value, bool erased );
    NetZone* FindNetZone( tUID zone );
    bool     IsTypeFree( unsigned int type ) const;
    bool     IsTaken( tUID zone, unsigned int loc ) const;
    bool     PlaceCar( unsigned int type, NetZone& zone, unsigned int loc, const pddiColour& colour );
    void     RemoveParkedCar( ParkedCarInfo& info );
    void     ClearZoneCars( tUID zone );
    void     ApplyPopulation( NetZone& zone, const net::WorldStateValue& plan );
    unsigned int mNumCarTypes;
    unsigned int mNumParkedCars;

    //For loading locators, we store all the locators loaded while a given UID is active.
    CarStartLocator* mLocators[ MAX_LOCATORS_PER_ZONE ];
    unsigned int mNumLocators;
    tUID mLoadingZoneUID;

    //Free "Moment" cars
    ParkedCarInfo mFreeCar;
    CarStartLocator* mFreeCarLocator;

    ParkedCarManager();
    virtual ~ParkedCarManager();

    void CreateFreeCar();

    void FindParkedCarInfo( Vehicle* v, ParkedCarInfo*& info, bool& isFreeCar );

    bool mParkedCarsEnabled;   // little bool to disable these during street races

    // Network claim of a parked car the local player got into (WOC_TAKEN,
    // first claim wins on the server). Until the verdict the entry is
    // predicted; if another player won, the player is put out of the car, the
    // previous current vehicle comes back and our copy of the car goes.
    struct Pushed
    {
        Vehicle*     car;
        unsigned int restMs;   // how long it has been still
    };
    static const unsigned int MAX_PUSHED = 8;
    Pushed       mPushed[ MAX_PUSHED ];
    unsigned int mNumPushed;
    void StopStreamingPush( unsigned int index, bool publishPose );

    Vehicle* mClaimCar;          // car whose claim is pending (or lost)
    Vehicle* mClaimPrevVehicle;  // current vehicle before the entry (AddRef'd)
    bool     mClaimLost;         // waiting for the get-out to finish
    void ClaimBegin( Vehicle* car );
    void ClaimSettle();          // drop the bookkeeping (entry stands or done)
    void ClaimLost( Vehicle* car );
    void ClaimFinishRollback();

    //Prevent wasteful constructor creation.
    ParkedCarManager( const ParkedCarManager& parkedcarmanager );
    ParkedCarManager& operator=( const ParkedCarManager& parkedcarmanager );
};

//*****************************************************************************
//
//Inline Public Member Functions
//
//*****************************************************************************
inline ParkedCarManager& GetPCM() { return ParkedCarManager::GetInstance(); };

#endif //PARKEDCARMANAGER_H
