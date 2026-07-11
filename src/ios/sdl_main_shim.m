/*
 * sdl_main_shim.m — iOS entry point for Butterscotch.
 *
 * SDL_main.h renames "main" -> "SDL_main" in every translation unit that
 * includes it, so main.c's entry point becomes SDL_main(). We bootstrap
 * UIKit here by calling UIApplicationMain with SDLUIKitDelegate, which
 * SDL2 registers as the application delegate and which internally calls
 * SDL_main (our renamed main.c entry point) on a background thread once
 * UIKit is ready.
 *
 * The -ObjC linker flag (set in CMakeLists.txt) is required to prevent
 * the linker from dead-stripping SDLUIKitDelegate's ObjC method
 * implementations out of the static SDL2 framework.
 *
 * Do NOT include SDL_main.h here — we need the real "main" symbol.
 */

#import <UIKit/UIKit.h>

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, @"SDLUIKitDelegate");
    }
}
