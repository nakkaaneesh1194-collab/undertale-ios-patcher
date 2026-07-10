/*
 * VirtualController.m
 *
 * Creates a GCVirtualController on iOS 15+ at app launch.
 * Compiled directly into the Butterscotch iOS binary — no dylib injection needed.
 *
 * The virtual controller connects on MFi slot 0 and is visible to SDL2's
 * SDL_GameController API, so Butterscotch's gamepad input works without any
 * data.win patches for the j_ch device index (SDL manages slot assignment).
 *
 * Input element set is controlled by the VIRTUAL_CONTROLLER_DPAD CMake
 * option (default: direction pad). Pass -DVIRTUAL_CONTROLLER_DPAD=OFF for
 * analog left thumbstick instead.
 */

#import <GameController/GameController.h>
#import <Foundation/Foundation.h>

API_AVAILABLE(ios(15.0))
static GCVirtualController *gVirtualController = nil;

__attribute__((constructor))
static void VCInit(void) {
    if (@available(iOS 15, *)) {
        dispatch_after(
            dispatch_time(DISPATCH_TIME_NOW, (int64_t)(0.5 * NSEC_PER_SEC)),
            dispatch_get_main_queue(),
            ^{
                GCVirtualControllerConfiguration *config =
                    [GCVirtualControllerConfiguration new];
                config.elements = [NSSet setWithArray:@[
                    GCInputButtonA,
                    GCInputButtonB,
                    GCInputButtonX,
                    GCInputButtonY,
#ifdef VIRTUAL_CONTROLLER_THUMBSTICK
                    GCInputLeftThumbstick,
#else
                    GCInputDirectionPad,
#endif
                    GCInputButtonMenu
                ]];
                gVirtualController =
                    [GCVirtualController virtualControllerWithConfiguration:config];
                [gVirtualController connectWithReplyHandler:^(NSError *error) {
                    if (error) {
                        NSLog(@"[Butterscotch] VirtualController connect error: %@", error);
                    } else {
                        NSLog(@"[Butterscotch] VirtualController connected");
                    }
                }];
            }
        );
    }
}
