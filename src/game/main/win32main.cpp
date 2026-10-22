//=============================================================================
// Copyright (C) 2002 Radical Entertainment Ltd.  All rights reserved.
//
// File:        xboxmain.cpp
//
// Description: This file contains the main enrty point to the game.
//
// History:     + Based on xbox main and winmain from squidney
//
//=============================================================================

// resolve annoying as hell "unresolved external symbol _main"
#define SDL_MAIN_HANDLED

//========================================
// System Includes
//========================================
// Standard Library
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <exception>
// Foundation Tech
#include <raddebug.hpp>
#include <radobject.hpp>
// file reading before radtech
#include <stdio.h>

#if defined(RAD_WIN32)
#include <windows.h>
#include <dbghelp.h>
#pragma comment( lib, "dbghelp.lib" )
#endif

//========================================
// Project Includes
//========================================
#include <main/game.h>
#include <main/win32platform.h>
#include <main/singletons.h>
#include <main/commandlineoptions.h>
#include <memory/memoryutilities.h>
#include <memory/srrmemory.h>

//========================================
// Forward Declarations
//========================================
static void ProcessCommandLineArguments( int argc, char *argv[] );

static void ProcessCommandLineArgumentsFromFile();

static void LogOutputFunction(void* userdata, int category, SDL_LogPriority priority, const char* message)
{
    rDebugPrintf("[SDL] %s\n", message);
}

#if defined(RAD_WIN32)
static LONG WINAPI CrashHandler( EXCEPTION_POINTERS* pExceptionInfo )
{
    FILE* pFile = fopen( "crash.log", "w" );
    if( pFile != NULL )
    {
        fprintf( pFile, "CRASH CODE: 0x%08lX at 0x%p\n",
                 pExceptionInfo->ExceptionRecord->ExceptionCode,
                 pExceptionInfo->ExceptionRecord->ExceptionAddress );

        HANDLE hProcess = GetCurrentProcess();
        SymInitialize( hProcess, NULL, TRUE );

        STACKFRAME64 frame;
        memset( &frame, 0, sizeof( frame ) );
        frame.AddrPC.Offset = pExceptionInfo->ContextRecord->Rip;
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrStack.Offset = pExceptionInfo->ContextRecord->Rsp;
        frame.AddrStack.Mode = AddrModeFlat;
        frame.AddrFrame.Offset = pExceptionInfo->ContextRecord->Rbp;
        frame.AddrFrame.Mode = AddrModeFlat;

        const DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
        for( int i = 0; i < 32; i++ )
        {
            if( !StackWalk64( machineType, hProcess, GetCurrentThread(),
                              &frame, pExceptionInfo->ContextRecord, NULL,
                              SymFunctionTableAccess64, SymGetModuleBase64, NULL ) )
            {
                break;
            }
            if( frame.AddrPC.Offset == 0 )
            {
                break;
            }

            char symbolBuf[ sizeof( IMAGEHLP_SYMBOL64 ) + 256 ] = { 0 };
            IMAGEHLP_SYMBOL64* pSym = ( IMAGEHLP_SYMBOL64* ) symbolBuf;
            pSym->SizeOfStruct = sizeof( IMAGEHLP_SYMBOL64 );
            pSym->MaxNameLength = 255;

            DWORD64 displacement = 0;
            char name[ 256 ] = "<unknown>";
            if( SymGetSymFromAddr64( hProcess, frame.AddrPC.Offset, &displacement, pSym ) )
            {
                strncpy( name, pSym->Name, sizeof( name ) - 1 );
            }

            IMAGEHLP_LINE64 line = { 0 };
            line.SizeOfStruct = sizeof( IMAGEHLP_LINE64 );
            DWORD lineDisplacement = 0;
            if( SymGetLineFromAddr64( hProcess, frame.AddrPC.Offset, &lineDisplacement, &line ) )
            {
                fprintf( pFile, "  #%02d  0x%016I64X  %s +0x%I64X  [%s:%lu]\n",
                         i, frame.AddrPC.Offset, name, displacement, line.FileName, line.LineNumber );
            }
            else
            {
                fprintf( pFile, "  #%02d  0x%016I64X  %s +0x%I64X\n",
                         i, frame.AddrPC.Offset, name, displacement );
            }
        }
        fclose( pFile );
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// The crash path may go through the C runtime (abort/terminate/pure virtual)
// which SetUnhandledExceptionFilter does NOT catch. Dump a stack there too.
void WriteCrashDump( const char* reason )
{
    FILE* pFile = fopen( "crash.log", "w" );
    if( pFile == NULL ) return;
    fprintf( pFile, "CRASH CODE: %s\n", reason );

    HANDLE hProcess = GetCurrentProcess();
    SymInitialize( hProcess, NULL, TRUE );

    CONTEXT context;
    RtlCaptureContext( &context );

    STACKFRAME64 frame;
    memset( &frame, 0, sizeof( frame ) );
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;

    const DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
    for( int i = 0; i < 32; i++ )
    {
        if( !StackWalk64( machineType, hProcess, GetCurrentThread(), &frame, &context,
                          NULL, SymFunctionTableAccess64, SymGetModuleBase64, NULL ) )
        {
            break;
        }
        if( frame.AddrPC.Offset == 0 ) break;

        char symbolBuf[ sizeof( IMAGEHLP_SYMBOL64 ) + 256 ] = { 0 };
        IMAGEHLP_SYMBOL64* pSym = ( IMAGEHLP_SYMBOL64* ) symbolBuf;
        pSym->SizeOfStruct = sizeof( IMAGEHLP_SYMBOL64 );
        pSym->MaxNameLength = 255;

        DWORD64 displacement = 0;
        char name[ 256 ] = "<unknown>";
        if( SymGetSymFromAddr64( hProcess, frame.AddrPC.Offset, &displacement, pSym ) )
        {
            strncpy( name, pSym->Name, sizeof( name ) - 1 );
        }

        fprintf( pFile, "  #%02d  0x%016I64X  %s +0x%I64X\n",
                 i, frame.AddrPC.Offset, name, displacement );
    }
    fclose( pFile );
}

static void CrashHandlerAbort( int /*signum*/ )
{
    WriteCrashDump( "abort()" );
}

static void __cdecl CrashHandlerTerminate()
{
    WriteCrashDump( "terminate()" );
}

static void __cdecl CrashHandlerPureCall()
{
    WriteCrashDump( "pure virtual call (R6025)" );
}
#endif

//=============================================================================
// Function:    WinMain
//=============================================================================
//
// Description: Main Windows entry point.
// 
// Parameters:  win32 parameters
//
// Returns:     win32 return.
//
//=============================================================================
int main( int argc, char *argv[] )
{
    //
    // Pick out and store command line settings.
    //
    CommandLineOptions::InitDefaults();
    ProcessCommandLineArguments( argc, argv );
    ProcessCommandLineArgumentsFromFile();

#if defined(RAD_WIN32)
    // Install an unhandled-exception filter so an unexpected crash (e.g. a
    // pure-virtual call / access violation during multiplayer entry) writes a
    // symbolicated stack to crash.log instead of silently dying.
    SetUnhandledExceptionFilter( CrashHandler );
    SymSetOptions( SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS );

    // Routines that bypass the SEH filter (abort/terminate/pure virtual) must
    // be hooked too, otherwise the failure exits silently with no diagnostic.
    _set_abort_behavior( 0, _WRITE_ABORT_MSG );
    signal( SIGABRT, CrashHandlerAbort );
    set_terminate( CrashHandlerTerminate );
    _set_purecall_handler( CrashHandlerPureCall );
#endif
    
    //
    // Initialize SDL subsystems
    //
    SDL_SetMainReady();
    SDL_Init( SDL_INIT_EVENTS | SDL_INIT_VIDEO );
    SDL_LogSetOutputFunction( LogOutputFunction, NULL );

    //
    // Have to get FTech setup first so that we can use all the memory services.
    // The initialize window call will fail if another Simpsons window exists. In
    // this case, we exit.
    //
    if( !Win32Platform::InitializeWindow() )
    {
        return 0;
    }
    Win32Platform::InitializeFoundation();

    srand (Game::GetRandomSeed ());


    // Now disable the default heap
    //
#ifndef RAD_RELEASE
    tName::SetAllocator (GMA_DEBUG);

    //g_HeapActivityTracker.EnableHeapAllocs (GMA_DEFAULT, false);
    //g_HeapActivityTracker.EnableHeapFrees (GMA_DEFAULT, false);
#endif

    HeapMgr()->PushHeap (GMA_PERSISTENT);

    //
    // Instantiate all the singletons before doing anything else.
    //
    CreateSingletons();

    //
    // Construct the platform object.
    //
    Win32Platform* pPlatform = Win32Platform::CreateInstance();
    rAssert( pPlatform != NULL );

    //
    // Create the game object.
    //
    Game* pGame = Game::CreateInstance( pPlatform );
    rAssert( pGame != NULL );


    //
    // Initialize the game.
    //
    pGame->Initialize();

    HeapMgr()->PopHeap (GMA_PERSISTENT);

    //
    // Run it!  Control will not return from here until the game is stopped.
    //
    pGame->Run();

    //
    // Terminate the game (this frees all resources allocated by the game).
    //
    pGame->Terminate();

    //
    // Dump all the singletons.
    //
    DestroySingletons();

    //
    // Destroy the game object.
    //
    Game::DestroyInstance();

    //
    // Shutdown the platform.
    //
    pPlatform->ShutdownPlatform();

    //
    // Destroy the game and platform (do it in this order in case the game's 
    // destructor references the platform.
    //
    Win32Platform::DestroyInstance();

    // As a last thing, shut down the memory.
    Win32Platform::ShutdownMemory();

    // Re-enable the default heap
    //
#ifndef RAD_RELEASE
    tName::SetAllocator (RADMEMORY_ALLOC_DEFAULT);
#endif

    //
    // Shutdown SDL subsystems
    //
    SDL_Quit();

    //
    // Pass any error codes back to the operating system.
    //
    return 0;
}


//=============================================================================
// Function:    ProcessCommandLineArguments
//=============================================================================
//
// Description: Pick out the command line options and store them.
// 
// Parameters:  None.
//
// Returns:     None.
//
//=============================================================================
void ProcessCommandLineArguments(int argc, char* argv[])
{
    rDebugPrintf( "*************************************************************************\n" );
    rDebugPrintf( "Command Line Args:\n" );

    //
    // Pick out all the command line options and store them in GameDB.
    // Also dump them to the output for handy dandy viewing.
    //
    for (int i = 1; i < argc; i++)
    {
        rDebugPrintf( "arg%d: %s\n", i, argv[i] );
        
        CommandLineOptions::HandleOption( argv[i] );
    }

    if( !CommandLineOptions::Get( CLO_ART_STATS ) )
    {
        //CommandLineOptions::HandleOption( "noheaps" );
    }

    rDebugPrintf( "*************************************************************************\n" );
}



void ProcessCommandLineArgumentsFromFile()
{
#ifndef FINAL

    //Chuck: looking for additional command line args being passed in from a file
    //its for QA testing etc.

    FILE* pfile = fopen( "command.txt", "r" );

    if (pfile != NULL)
    {
        int ret = fseek( pfile, 0, SEEK_END ); 
        rAssert( ret == 0 );

        int len = ftell( pfile );
        rAssertMsg( len < 256, "Command line file too large to process." );

        rewind( pfile );

        if( len > 0 && len < 256 )
        {
            char commandlinestring[256] = {0};

            fgets( commandlinestring, 256, pfile );
                    
            char* argument = strtok(commandlinestring," ");
            while (argument != NULL)
            {
                CommandLineOptions::HandleOption(argument);
                argument=strtok(NULL," ");
            }
        }

        fclose( pfile );
    }
#endif //FINAL
} //end of Function
