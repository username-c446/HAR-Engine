//===========================================================================
// Copyright (C) 2000 Radical Entertainment Ltd.  All rights reserved.
//
// Component:   CGuiScreenMainMenu
//
// Description: 
//              
//
// Authors:     Darwin Chau
//              Tony Chu
//
// Revisions		Date			Author	    Revision
//                  2000/11/20		DChau		Created
//                  2002/06/06      TChu        Modified for SRR2
//
//===========================================================================

#ifndef GUISCREENMAINMENU_H
#define GUISCREENMAINMENU_H

#ifndef FINAL
    #ifndef RAD_DEMO
        #define SRR2_LEVEL_SELECTION // for SRR2 level selection
    #endif
#endif

//===========================================================================
// Nested Includes
//===========================================================================
#include <presentation/gui/guiscreen.h>

#include <render/enums/renderenums.h>

//===========================================================================
// Forward References
//===========================================================================
class CGuiMenu;
class tAnimation;

//===========================================================================
// Interface Definitions
//===========================================================================
class CGuiScreenMainMenu : public CGuiScreen
{
public:
    CGuiScreenMainMenu( Scrooby::Screen* pScreen, CGuiEntity* pParent );
    virtual ~CGuiScreenMainMenu();

	virtual void HandleMessage( eGuiMessage message, 
			                    unsigned int param1 = 0,
								unsigned int param2 = 0 );

    void InitMenu();
    virtual CGuiMenu* HasMenu() { return m_pMenu; }

protected:
    void InitIntro();
	void InitRunning();
	void InitOutro();

    void UpdateIntro( unsigned int elapsedTime );
    void UpdateRunning( unsigned int elapsedTime );
    void UpdateOutro( unsigned int elapsedTime );

    // Multiplayer host/join overlay (custom, code-driven).
    void StartMultiplayerOverlay();
    void EndMultiplayerOverlay();
    void OnMpOverlayConfirm();
    void StartMultiplayerHost( unsigned short port );
    void StartMultiplayerJoin( const char* ip, unsigned short port );
    void UpdateMpOverlayDisplay();
    static void OnMpOverlayChar( const char* utf8Char );
    static void OnMpOverlayBackspace();

private:
    enum eMainMenuSelection
    {
#ifdef RAD_DEMO
        MAIN_MENU_PLAY_LEVEL_2,
        MAIN_MENU_PLAY_LEVEL_7,
        MAIN_MENU_OPTIONS,
#else
        MAIN_MENU_LOAD_GAME,
        MAIN_MENU_CARD_GALLERY,
        MAIN_MENU_OPTIONS,
        MAIN_MENU_MINI_GAME,
        MAIN_MENU_MULTIPLAYER,
  #ifdef RAD_WIN32
        MAIN_MENU_QUIT_GAME,
  #endif
        MAIN_MENU_NEW_GAME,
        MAIN_MENU_RESUME_GAME,
#endif

        NUM_MAIN_MENU_SELECTIONS
    };

    void UpdateGags( unsigned int elapsedTime );
    void StopHomerIdleAnimation();
    void TurnOnGlowItems( unsigned int items );

    void OnNewGameSelected( RenderEnums::LevelEnum level = RenderEnums::L1,
                            RenderEnums::MissionEnum mission = RenderEnums::M1 );

    void OnResumeGameSelected();
    void OnMiniGameSelected();
    void OnMultiplayerSelected();

#ifdef RAD_WIN32
    void OnQuitGameSelected();
#endif

#ifdef SRR2_LEVEL_SELECTION
    void ToggleLevelMenu();
#endif

    CGuiMenu* m_pMenu;

    Scrooby::Page* m_gags;
    int m_currentGagIndex;
    int m_nextGagIndex;
    int m_nextHomerGagIndex;
    unsigned int m_gagsElapsedTime;
    unsigned int m_gagsCycleTime;

    Scrooby::Pure3dObject* m_homer;
    Scrooby::Pure3dObject* m_raceCar;
    tAnimation* m_homerIdleAnim;
    Scrooby::Layer* m_tvFrame;

    Scrooby::Pure3dObject* m_glowingItems[ NUM_MAIN_MENU_SELECTIONS ];

    // Multiplayer overlay state.
    bool m_mpOverlayActive;
    int  m_mpOverlaySelection;   // 0 = Host, 1 = Join
    char m_mpIpBuffer[64];       // typed IP/port string
    Scrooby::Text* m_mpOverlayText;

    static CGuiScreenMainMenu* spMpOverlayOwner;

};


class CGuiScreenIntroTransition: public CGuiScreen
{
public:
    CGuiScreenIntroTransition( Scrooby::Screen* pScreen, CGuiEntity* pParent );
    virtual ~CGuiScreenIntroTransition();

	virtual void HandleMessage( eGuiMessage message, 
			                    unsigned int param1 = 0,
								unsigned int param2 = 0 );

    static bool s_introTransitionPlayed;

protected:
    void InitIntro();
	void InitRunning();
	void InitOutro();

private:
    enum eState
    {
        WAITING_FOR_MULTICONTROLLER,
        RUNNING_INTRO_ANIMATION,

        NUM_STATES
    };

    eState m_introState;
    Scrooby::Pure3dObject* m_homer;
    Scrooby::Layer* m_tvFrame;
    Scrooby::Page* m_gags;

};

#endif // GUISCREENMAINMENU_H
