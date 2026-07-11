/*
 * sdl_main_shim.m — iOS entry point for Butterscotch.
 *
 * SDL_main.h renames "main" to "SDL_main" in every translation unit that
 * includes it.  The real main() that calls UIApplicationMain is normally
 * supplied by libSDL2main.a, but the iOS framework DMG doesn't ship that
 * file.  We provide it here instead.
 *
 * SDLUIKitDelegate (inside SDL2.framework) sets up the UIKit run-loop and
 * then calls our SDL_main (i.e. the renamed main() in main.c) once the app
 * is ready.
 */
#import <UIKit/UIKit.h>

/* Do NOT include SDL_main.h here — we want the real symbol name "main". */
int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, @"SDLUIKitDelegate");
    }
}
