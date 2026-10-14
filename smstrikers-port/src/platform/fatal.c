// Fatal-error path: a line on stderr, and a message box where there is a display.

#include "port/fatal.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(PORT_USE_AURORA)
#include <SDL3/SDL.h>
#endif

static int messagebox_suppressed(void)
{
    const char* e = getenv("STRIKERS_NO_MESSAGEBOX");
    return e != NULL && *e != '\0' && e[0] != '0';
}

void port_fatal_notice(const char* title, const char* text)
{
    if (title == NULL)
        title = "Super Mario Strikers";
    if (text == NULL)
        text = "The game cannot start.";

    fprintf(stderr, "\n=== %s ===\n%s\n", title, text);
    fflush(stderr);

#if defined(__ANDROID__) || defined(SV_VISIONOS)
    // strikers-quest r41: on the Quest nobody sees a message box under the immersive game, which
    // just closes. The launcher panel (LobbyActivity) shows this file the next time it opens.
    // StrikersVision: the same file for the visionOS app's panel; SDL there has no video, so no box.
    {
        const char* home = getenv("STRIKERS_HOME");
        if (home != NULL && *home != '\0')
        {
            char path[1024];
            FILE* f;
            snprintf(path, sizeof path, "%s/last-error.txt", home);
            f = fopen(path, "w");
            if (f != NULL)
            {
                fprintf(f, "%s\n%s\n", title, text);
                fclose(f);
            }
        }
    }
#endif

#if defined(PORT_USE_AURORA)
    if (!messagebox_suppressed())
    {
        // SDL turns SIGINT and SIGTERM into a quit event nothing pumps under a box.
        SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
        signal(SIGINT, SIG_DFL);
        signal(SIGTERM, SIG_DFL);

        // Asking for the subsystem is how to find out whether there is a display.
        if (SDL_InitSubSystem(SDL_INIT_VIDEO))
        {
            bool shown = SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, NULL);
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            // SDL tries every driver's box only with video down, so Wayland without zenity gets X11's.
            if (!shown && !SDL_WasInit(SDL_INIT_VIDEO))
                shown = SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, text, NULL);
            if (!shown)
            {
                fprintf(stderr, "[port] (no message box could be shown: %s)\n", SDL_GetError());
                fflush(stderr);
            }
        }
        else
        {
            fprintf(stderr, "[port] (no display for a message box: %s)\n",
                    SDL_GetError());
            fflush(stderr);
        }
    }
#else
    (void)messagebox_suppressed;
#endif
}

#if defined(SV_VISIONOS)
// visionos/xr/sv_game.mm: on the game thread, ends that thread and tells the app why (the app stays
// open and shows the reason); returns on any other thread, where exit() below still applies.
void sv_game_fatal(const char* title, const char* text);
#endif

void port_fatal(const char* title, const char* text)
{
    port_fatal_notice(title, text);
#if defined(SV_VISIONOS)
    sv_game_fatal(title, text);
#endif
    // exit, not abort: the crash handler must not backtrace a refusal to start.
    exit(1);
}
