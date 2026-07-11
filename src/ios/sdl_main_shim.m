/*
 * sdl_main_shim.m — iOS entry point for Butterscotch.
 *
 * SDL_main.h renames "main" -> "SDL_main" in every TU that includes it.
 * SDL_UIKit_RunApp() is the SDL2 static-framework way to bootstrap UIKit:
 * it creates SDLUIKitDelegate (via ObjC runtime internals, not a string
 * lookup), sets up the run-loop, then calls our SDL_main.
 *
 * Do NOT include SDL_main.h here — we need the real "main" symbol.
 */

/* Forward-declare SDL_UIKit_RunApp so we don't need SDL headers. */
extern int SDL_UIKit_RunApp(int argc, char *argv[], void *mainFunction);

/* Forward-declare the renamed entry point from main.c. */
extern int SDL_main(int argc, char *argv[]);

int main(int argc, char *argv[]) {
    return SDL_UIKit_RunApp(argc, argv, SDL_main);
}
