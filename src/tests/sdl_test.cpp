// Minimal SDL window creation test for the SRR2 environment.
// Builds and runs a tiny SDL2 window in this session to prove SDL_CreateWindow
// works here at all, isolating whether the game's stall is SDL/environment
// related or deeper in the engine.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <cstdio>

int main(int argc, char** argv)
{
    printf("SDL test start\n"); fflush(stdout);
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        printf("SDL_Init failed: %s\n", SDL_GetError()); fflush(stdout);
        return 2;
    }
    printf("SDL_Init ok\n"); fflush(stdout);
    SDL_Window* w = SDL_CreateWindow("SDLTest", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     320, 240, SDL_WINDOW_OPENGL);
    if (!w) {
        printf("CreateWindow failed: %s\n", SDL_GetError()); fflush(stdout);
        return 3;
    }
    printf("CreateWindow ok\n"); fflush(stdout);
    SDL_DestroyWindow(w);
    SDL_Quit();
    printf("done\n"); fflush(stdout);
    return 0;
}