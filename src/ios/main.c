/*
 * Butterscotch iOS — game_main entry point.
 *
 * ios_backend.c (Un1q32's UIKit/EAGL backend) defines main() and calls
 * game_main() on a background thread once the GL context is ready.
 *
 * argv[last] = path to data.win (set by ios_backend.c's startGameWithPath:)
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <Foundation/Foundation.h>

#include "common.h"
#include "runner.h"
#include "runner_keyboard.h"
#include "runner_mouse.h"
#include "overlay_file_system.h"
#include "data_win.h"
#include "gettime.h"
#include "stb_ds.h"

#if defined(USE_MINIAUDIO)
#include "ma_audio_system.h"
#elif defined(USE_OPENAL)
#include "al_audio_system.h"
#endif
#include "noop_audio_system.h"

#ifdef ENABLE_MODERN_GL
#include "gl/gl_renderer.h"
#endif

#include "ios/platformdefs.h"

enum GraphicsAPI gfx = MODERN_GL;
InputRecording *globalInputRecording = NULL;

int game_main(int argc, char *argv[]) {
    /* ios_backend.c passes data.win path as the last argument */
    const char *dataWinPath = (argc > 1) ? argv[argc - 1] : NULL;

    if (!dataWinPath || dataWinPath[0] == '\0') {
        NSBundle *bundle = [NSBundle mainBundle];
        NSString *path = [bundle pathForResource:@"data" ofType:@"win"];
        if (!path) path = [bundle pathForResource:@"game" ofType:@"ios"];
        if (path) dataWinPath = [path fileSystemRepresentation];
    }

    if (!dataWinPath || dataWinPath[0] == '\0') {
        fprintf(stderr, "[iOS] data.win not found\n");
        return 1;
    }

    /* Save folder: passed via --save-folder <path> */
    const char *saveFolder = NULL;
    for (int i = 1; i < argc - 1; i++) {
        if (strcmp(argv[i], "--save-folder") == 0 && i + 1 < argc - 1) {
            saveFolder = argv[i + 1];
            break;
        }
    }
    if (!saveFolder) {
        NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
        NSString *docs = [paths objectAtIndex:0];
        NSString *savePath = [docs stringByAppendingPathComponent:@"saves"];
        [[NSFileManager defaultManager] createDirectoryAtPath:savePath
                                  withIntermediateDirectories:YES attributes:nil error:nil];
        saveFolder = [savePath fileSystemRepresentation];
    }

    NSString *dataWinNS = [NSString stringWithUTF8String:dataWinPath];
    const char *basePath = [[dataWinNS stringByDeletingLastPathComponent] fileSystemRepresentation];

    DataWin *metaWin = DataWin_parse(dataWinPath, (DataWinParserOptions){
        .parseGen8 = true, .parseStrg = true,
    });
    int32_t winW = 640, winH = 480;
    const char *gameTitle = "Undertale";
    if (metaWin) {
        if (metaWin->gen8.defaultWindowWidth  > 0) winW = (int32_t)metaWin->gen8.defaultWindowWidth;
        if (metaWin->gen8.defaultWindowHeight > 0) winH = (int32_t)metaWin->gen8.defaultWindowHeight;
        if (metaWin->gen8.displayName && metaWin->gen8.displayName[0])
            gameTitle = metaWin->gen8.displayName;
        DataWin_free(metaWin);
    }

    if (!platformInit(winW, winH, gameTitle, false)) {
        fprintf(stderr, "[iOS] platformInit failed\n");
        return 1;
    }

    DataWin *dataWin = DataWin_parse(dataWinPath, (DataWinParserOptions){
        .parseGen8 = true, .parseOptn = true, .parseLang = true, .parseExtn = true,
        .parseSond = true, .parseAgrp = true, .parseSprt = true, .parseBgnd = true,
        .parsePath = true, .parseScpt = true, .parseGlob = true, .parseShdr = true,
        .parseFont = true, .parseTmln = true, .parseObjt = true, .parseRoom = true,
        .parseTpag = true, .parseCode = true, .parseVari = true, .parseFunc = true,
        .parseStrg = true, .parseTxtr = true, .parseAudo = true,
        .skipLoadingPreciseMasksForNonPreciseSprites = true,
        .loadType = DATAWINLOADTYPE_LOAD_IN_MEMORY_AHEAD_OF_TIME,
        .lazyLoadRooms = false,
        .eagerlyLoadedRooms = NULL,
    });
    if (!dataWin) {
        fprintf(stderr, "[iOS] Failed to parse %s\n", dataWinPath);
        platformExit();
        return 1;
    }

#ifdef ENABLE_MODERN_GL
    Renderer *renderer = GLRenderer_create();
    ((GLRenderer *)renderer)->isGLES = true;  /* iOS always uses OpenGL ES */
#else
    Renderer *renderer = NULL;
#endif

    VMContext  *vm    = VM_create(dataWin);
    FileSystem *fs    = (FileSystem *)OverlayFileSystem_create(basePath, saveFolder);

#if defined(USE_MINIAUDIO)
    AudioSystem *audio = (AudioSystem *)MaAudioSystem_create(dataWin);
    if (!audio) audio = (AudioSystem *)NoopAudioSystem_create();
#elif defined(USE_OPENAL)
    AudioSystem *audio = (AudioSystem *)AlAudioSystem_create(dataWin);
    if (!audio) audio = (AudioSystem *)NoopAudioSystem_create();
#else
    AudioSystem *audio = (AudioSystem *)NoopAudioSystem_create();
#endif

    Runner *runner = Runner_create(dataWin, vm, renderer, fs, audio);
    runner->osType         = OS_IOS;
    runner->setWindowTitle = platformSetWindowTitle;
    runner->windowHasFocus = NULL;
    runner->getWindowSize  = platformGetWindowSize;

    platformInitFunctions(runner);
    Runner_initFirstRoom(runner);

    uint64_t frameDue = (uint64_t)nowNanos();
    while (true) {
        RunnerKeyboard_beginFrame(runner->keyboard);
        RunnerMouse_beginFrame(runner->mouse);

        if (platformHandleEvents()) break;
        if (runner->shouldExit)    break;

        Runner_step(runner);

        int32_t fbW = 0, fbH = 0;
        platformGetWindowSize(&fbW, &fbH);
        if (fbW < 1) fbW = 1;
        if (fbH < 1) fbH = 1;

        int32_t gameW = (int32_t)dataWin->gen8.defaultWindowWidth;
        int32_t gameH = (int32_t)dataWin->gen8.defaultWindowHeight;
        if (gameW < 1) gameW = fbW;
        if (gameH < 1) gameH = fbH;

        Runner_beginFrame(runner, gameW, gameH, fbW, fbH, fbW, fbH);
        Runner_drawViews(runner, gameW, gameH, false);
        renderer->vtable->endFrameInit(renderer);
        Runner_drawPost(runner, fbW, fbH);
        renderer->vtable->endFrameEnd(renderer);
        Runner_drawGUI(runner, fbW, fbH, gameW, gameH);
        Runner_handlePendingRoomChange(runner);

        platformSwapBuffers();

        uint32_t fps = (runner->currentRoom && runner->currentRoom->speed > 0)
                     ? runner->currentRoom->speed : 30;
        frameDue += (uint64_t)(1000000000ULL / fps);
        platformSleepUntil(frameDue);
    }

    platformExit();
    return 0;
}
