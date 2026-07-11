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

/* Force the linker to include SDLUIKitDelegate's object file from the
 * SDL2 static framework. Without this, the linker dead-strips the class's
 * method implementations even with -ObjC, because SDL2 is a static lib
 * wrapped in a .framework rather than a plain .a file.
 * This extern creates a direct symbol reference at compile time. */
extern id OBJC_CLASS_$_SDLUIKitDelegate;
__attribute__((used)) static void *_forceSDLUIKitDelegate = &OBJC_CLASS_$_SDLUIKitDelegate;

int main(int argc, char *argv[]) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, @"SDLUIKitDelegate");
    }
}
