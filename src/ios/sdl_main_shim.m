/*
 * sdl_main_shim.m — iOS entry point for Butterscotch.
 *
 * SDL_main.h renames "main" -> "SDL_main" in every TU that includes it.
 * We provide the real main() here and hand off to UIApplicationMain with
 * SDLUIKitDelegate as the app delegate class.
 *
 * IMPORTANT: the CMake iOS block passes -ObjC to the linker so that
 * SDLUIKitDelegate (and all ObjC classes in the SDL2 static framework)
 * are not dead-stripped.  Without -ObjC the class lookup below fails at
 * runtime with "No class named SDLUIKitDelegate is loaded".
 *
 * Do NOT include SDL_main.h here — we need the real "main" symbol.
 */

#import <UIKit/UIKit.h>

/* Forward-declare the renamed entry point produced by SDL_main.h in main.c. */
extern int SDL_main(int argc, char *argv[]);

int main(int argc, char *argv[]) {
    return UIApplicationMain(argc, argv, nil, @"SDLUIKitDelegate");
}
