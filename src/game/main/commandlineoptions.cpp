//=============================================================================
// Copyright (C) 2002 Radical Entertainment Ltd.  All rights reserved.
//
// File:        commandlineoptions.cpp
//
// Description: CommandLineOptions class implementation.
//
// History:     + Created -- Darwin Chau
//
//=============================================================================

//========================================
// System Includes
//========================================
// Standard C Library
#include <string.h>
// Foundation Tech
#include <raddebug.hpp>
#include <SDL2/SDL.h>
#define strupr SDL_strupr

//========================================
// Project Includes
//========================================
#include <main/commandlineoptions.h>
#include <memory/srrmemory.h>

//******************************************************************************
//
// Global Data, Local Data, Local Classes
//
//******************************************************************************

//
// Bit flag for options.
//
#ifdef NEALL_DEMOTEST
simpsonsUInt64 CommandLineOptions::sOptions = ( 1 << CLO_DEMO_TEST | 1 << CLO_NO_TRAFFIC );
#else
simpsonsUInt64 CommandLineOptions::sOptions = 0;
#endif

short CommandLineOptions::s_defaultLevel = 0;
short CommandLineOptions::s_defaultMission = 0;

int CommandLineOptions::s_hostPort = -1;
int CommandLineOptions::s_joinPort = -1;
char CommandLineOptions::s_joinIp[128] = {0};
char CommandLineOptions::s_hostPassword[65] = {0};
int CommandLineOptions::s_hostSlots = 0;
char CommandLineOptions::s_joinPassword[65] = {0};

void CommandLineOptions::ClearNetworkOptions()
{
    s_hostPort = -1;
    s_joinPort = -1;
    s_joinIp[0] = '\0';
    s_hostPassword[0] = '\0';
    s_hostSlots = 0;
    s_joinPassword[0] = '\0';
}

//******************************************************************************
//
// Public Member Functions
//
//******************************************************************************

//==============================================================================
// CommandLineOptions::InitDefaults
//==============================================================================
//
// Description: Initialize default command-line options.
//
// Parameters:  None.
//
// Return:      None.
//
//==============================================================================
void CommandLineOptions::InitDefaults()
{
}

//==============================================================================
// CommandLineOptions::HandleOption
//==============================================================================
//
// Description: Interpret the command line string token.
//
// Parameters:  option - command line string token.
//
// Return:      None.
//
//==============================================================================
void CommandLineOptions::HandleOption( const char* const optionIn )
{
    simpsonsUInt64 andValue;
    simpsonsUInt64 orValue = 1;
    bool optionFound = true;

    rReleaseAssertMsg( NUM_CLO < (sizeof( sOptions ) * 8),
                       "*** WARNING: Too many commandline options!" );

    char option[ 256 ];
    strcpy( option, optionIn );
    // Tolerate a single leading '-' (or "--") so CLI forms like `-skipfe`,
    // `-L1`, `-HOSTPORT=47820`, `-join ...` are accepted identically to the
    // no-dash legacy forms (`skipfe L1 HOSTPORT=...`). The rest of this
    // function compares options verbatim, so without this strip a dash-prefixed
    // argument silently reaches the "Unhandled command line option" fallback and
    // the game boots into the normal menu instead of auto-starting a match.
    {
        const char* p = strchr( option, '=' );
        // Only strip leading dashes when there is no '=' before them (so
        // HOSTPORT=... and JOINIP=... keep their value intact and untouched).
        // 'strncmp(option, "HOSTPORT=",9)' below relies on starts-with, so we
        // must not shift past the token name.
        size_t dash = 0;
        while ( option[dash] == '-' ) ++dash;
        if ( dash > 0 )
        {
            const char* eq = strchr( option, '=' );
            size_t nameLen = eq ? static_cast<size_t>( eq - option ) : strlen( option );
            // Only strip if the dashes precede the whole name (not an '=' value).
            if ( dash <= nameLen )
            {
                // Shift the name+value left over the dashes; the '=' (if any)
                // and the name stay, only the leading '-' run is removed.
                size_t total = strlen( option );
                memmove( option, option + dash, total - dash + 1 );
            }
        }
    }
    rReleasePrintf( "Commandline Option: %s\n", option );

    // Network auto-start: -host <port> and -join <ip[:port]>
    if( strncmp( strupr(option), "HOSTPORT=", 9 ) == 0 )
    {
        s_hostPort = atoi( option + 9 );
        return;
    }
    else if( strncmp( strupr(option), "JOINIP=", 7 ) == 0 )
    {
        const char* spec = option + 7;
        int port = 3737;
        char ip[128];
        ip[0] = '\0';
        const char* colon = strchr( spec, ':' );
        if( colon != NULL )
        {
            size_t len = static_cast<size_t>( colon - spec );
            if( len >= sizeof(ip) ) len = sizeof(ip) - 1;
            memcpy( ip, spec, len );
            ip[len] = '\0';
            port = atoi( colon + 1 );
        }
        else
        {
            strcpy( ip, spec );
        }
        strncpy( s_joinIp, ip, sizeof(s_joinIp) - 1 );
        s_joinIp[sizeof(s_joinIp) - 1] = '\0';
        s_joinPort = port;
        return;
    }
    else if( strncmp( strupr(option), "HOSTPASS=", 9 ) == 0 )
    {
        strncpy( s_hostPassword, option + 9, sizeof(s_hostPassword) - 1 );
        s_hostPassword[sizeof(s_hostPassword) - 1] = '\0';
        return;
    }
    else if( strncmp( strupr(option), "HOSTSLOTS=", 10 ) == 0 )
    {
        s_hostSlots = atoi( option + 10 );
        return;
    }
    else if( strncmp( strupr(option), "JOINPASS=", 9 ) == 0 )
    {
        strncpy( s_joinPassword, option + 9, sizeof(s_joinPassword) - 1 );
        s_joinPassword[sizeof(s_joinPassword) - 1] = '\0';
        return;
    }

    if( strcmp( strupr(option), "NOMUSIC" ) == 0 )
    {
        orValue <<= CLO_NO_MUSIC;
    }
    else if( strcmp( strupr(option), "NOEFFECTS" ) == 0 )
    {
        orValue <<= CLO_NO_EFFECTS;
    }
    else if( strcmp( strupr(option), "NODIALOG" ) == 0 )
    {
        orValue <<= CLO_NO_DIALOG;
    }
    else if( strcmp( strupr(option), "MUTE" ) == 0 )
    {
        orValue <<= CLO_MUTE;
    }
    else if( strcmp( strupr(option), "SKIPMOVIE" ) == 0 )
    {
        orValue <<= CLO_SKIP_MOVIE;
    }
    else if( strcmp( strupr(option), "MEMMONITOR" ) == 0 )
    {
        orValue <<= CLO_MEMORY_MONITOR;
    }
    else if( strcmp( strupr(option), "HEAPSTATS" ) == 0 )
    {
        orValue <<= CLO_HEAP_STATS;
    }
    else if( strcmp( strupr(option), "ARTSTATS" ) == 0 )
    {
        orValue <<= CLO_ART_STATS;
    }
    else if( strcmp( strupr(option), "PROPSTATS" ) == 0 )
    {
        orValue <<= CLO_PROP_STATS;
    }
    else if( strcmp( strupr(option), "FEUNJOINED" ) == 0 )
    {
        orValue <<= CLO_FE_UNJOINED;
    }
    else if( strcmp( strupr(option), "SPEEDOMETER" ) == 0 )
    {
        orValue <<= CLO_SHOW_SPEED;
    }
    else if( strcmp( strupr(option), "NOHUD" ) == 0 )
    {
        orValue <<= CLO_NO_HUD;
    }
    else if( strcmp( strupr(option), "DEBUGBV" ) == 0 )
    {
        orValue <<= CLO_DEBUGBV;
    }
    else if( strcmp( strupr(option), "NOTRAFFIC" ) == 0 )
    {
        orValue <<= CLO_NO_TRAFFIC;
    }
    else if( strcmp( strupr(option), "SKIPSUNDAY" ) == 0 )
    {
        orValue <<= CLO_SKIP_SUNDAY;
    }
    else if( strcmp( strupr(option), "FREEROAM" ) == 0 )
    {
        orValue <<= CLO_FREEROAM;
    }
    else if( strcmp( strupr(option), "SKIPFE" ) == 0 )
    {
        orValue <<= CLO_SKIP_FE;
    }
    else if( strcmp( strupr(option), "HEADLESS" ) == 0 )
    {
        orValue <<= CLO_HEADLESS;
    }
    else if( strcmp( strupr(option), "ALLOWCARFLIP" ) == 0 )
    {
        orValue <<= CLO_ALLOW_CAR_FLIP;
    }
    else if( strcmp( strupr(option), "FEGAGS" ) == 0 )
    {
        orValue <<= CLO_FE_GAGS_TEST;
    }
    else if( strcmp( strupr(option), "FPS" ) == 0 )
    {
        orValue <<= CLO_FPS;
    }
    else if( strcmp( strupr(option), "DESIGNER" ) == 0 )
    {
        orValue <<= CLO_DESIGNER;
    }
    else if( strcmp( strupr(option), "DETECTLEAKS" ) == 0 )
    {
        orValue <<= CLO_DETECT_LEAKS;
    }
    else if( strcmp( strupr(option), "NOHEAPS" ) == 0 )
    {
        orValue <<= CLO_NO_HEAPS;
    }
    else if( strcmp( strupr(option), "PRINTMEMORY" ) == 0 )
    {
        orValue <<= CLO_PRINT_MEMORY;
    }
    else if( strcmp( strupr(option), "DEMOTEST" ) == 0 )
    {
        orValue <<= CLO_DEMO_TEST;
    }
    else if( strcmp( strupr(option), "NOSPLASH" ) == 0 )
    {
        orValue <<= CLO_NO_SPLASH;
    }
    else if( strcmp( strupr(option), "LANGUAGE" ) == 0 )
    {
        orValue <<= CLO_LANG_PROMPT;
    }
    else if( strcmp( strupr(option), "SKIPMEMCHECK" ) == 0 )
    {
        orValue <<= CLO_SKIP_MEMCHECK;
    }
    else if( strcmp( strupr(option), "NOHAPTIC" ) == 0 )
    {
        orValue <<= CLO_NO_HAPTIC;
    }
    else if( strcmp( strupr(option), "RANDOMBUTTONS" ) == 0 )
    {
        orValue <<= CLO_RANDOM_BUTTONS;
    }
    else if( strcmp( strupr(option), "SEQUENTIALDEMO" ) == 0 )
    {
        orValue <<= CLO_SEQUENTIAL_DEMO;
    }
    else if ( strcmp( strupr(option), "PCTEST" ) == 0 )
    {
        orValue <<= CLO_PARKED_CAR_TEST;
    }
    else if ( strcmp( strupr(option), "NOAVRIL" ) == 0 )
    {
        orValue <<= CLO_NO_AVRIL;
    }
    else if ( strcmp( strupr(option), "SHORTDEMO" ) == 0 )
    {
        orValue <<= CLO_SHORT_DEMO;
    }
    else if( strcmp( strupr( option), "PRINTLOADTIME" ) == 0 )
    {
        orValue <<= CLO_PRINT_LOAD_TIME;
    }
    else if( strcmp( strupr( option), "PRINTFRAMERATE" ) == 0 )
    {
        orValue <<= CLO_PRINT_FRAMERATE;
    }
    else if( strcmp( strupr( option), "CINEMATICJUMPS" ) == 0 )
    {
        orValue <<= CLO_CINEMATIC_JUMPS;
    }
    else if( strcmp( strupr( option), "EXITCARFADE" ) == 0 )
    {
        orValue <<= CLO_EXIT_CAR_FADE;
    }
    else if ( strcmp( strupr( option ), "SHOWDYNALOAD" ) == 0 )
    {
        orValue <<= CLO_SHOW_DYNA_ZONES;
    }
    else if ( strcmp( strupr( option ), "NOPEDS" ) == 0 )
    {
        orValue <<= CLO_NO_PEDS;
    }
    else if ( strcmp( strupr( option ), "MANUALRESETDAMAGE" ) == 0 )
    {
        orValue <<= CLO_MANUAL_RESET_DAMAGE;
    }
    else if ( strcmp( strupr( option ), "WINDOW" ) == 0 )
    {
        orValue <<= CLO_WINDOW_MODE;
    }
    else if ( strcmp( strupr( option ), "NOTUTORIAL" ) == 0 )
    {
        orValue <<= CLO_NO_TUTORIAL;
    }
    else if ( strcmp( strupr( option ), "COINS" ) == 0 )
    {
        orValue <<= CLO_COINS;
    }
    else if ( strcmp( strupr( option ), "PROGSCAN" ) == 0 )
    {
        orValue <<= CLO_PROGRESSIVE_SCAN;
    }
    else if ( strcmp( strupr( option ), "LARGEHEAPS" ) == 0 )
    {
        orValue <<= CLO_LARGEHEAPS;
    }
    else if ( strcmp( strupr( option ), "MEMCARDCHEAT" ) == 0 )
    {
        orValue <<= CLO_MEMCARD_CHEAT;
    }
    else if ( strcmp( strupr( option ), "FILENOTFOUND" ) == 0 )
    {
        orValue <<= CLO_FILE_NOT_FOUND;
    }
    else if ( strcmp( strupr( option ), "NOLOADINGSPEW" ) == 0 )
    {
        orValue <<= CLO_NO_LOADING_SPEW;
    }    
    else if ( strcmp( strupr( option ), "AUDIOSPEW" ) == 0 )
    {
        orValue <<= CLO_AUDIO_LOADING_SPEW;
    }    
    else if ( strcmp( strupr( option ), "RELEASEPRINT" ) == 0 )
    {
        extern bool g_AllowDebugOutput;
        g_AllowDebugOutput = true;
        optionFound = false;     
    }
    else if ( strcmp( strupr( option ), "NOFRUITLESS" ) == 0 )
    {
        extern bool gFruitless;
        gFruitless = false;
        optionFound = false;        
    }
    else if ( strcmp( strupr( option ), "RADTUNER" ) == 0 )
    {
        extern bool gTuneSound;
        gTuneSound = true;
        optionFound = false;
    }

    else
    {
        // special case for "l<N>" and "m<N>" commandline options
        //
        optionFound = false;
        int stringLength = strlen( option );
        char lastChar = option[ stringLength - 1 ];
        option[ stringLength - 1 ] = '\0';

        if( strcmp( strupr(option), "L" ) == 0 )
        {
            s_defaultLevel = lastChar - '1';
        }
        else if( strcmp( strupr(option), "M" ) == 0 )
        {
            s_defaultMission = lastChar - '1';
        }
    }

    if( optionFound )
    {
        sOptions = sOptions | orValue;
    }
    else
    {
        rDebugPrintf( "Unhandled command line option: %s\n", option );
    }
}

  
//==============================================================================
// CommandLineOptions::Get
//==============================================================================
//
// Description: Retrieve the specified option.
//
// Parameters:  eOption - the enumerated command line option.
//
// Return:      bool - true if the option is enabled, false otherwise.
//
//==============================================================================
bool CommandLineOptions::Get( CmdLineOptionEnum eOption )
{
    simpsonsUInt64 andValue = 1;

    rAssert( NUM_CLO <= 64 );

    andValue <<= eOption;
    return( ( sOptions & andValue ) != 0 );
}


//******************************************************************************
//
// Private Member Functions
//
//******************************************************************************
