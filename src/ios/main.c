/*
 * Butterscotch iOS platform backend
 *
 * Uses SDL2 for window/event/GL management (SDL2 has first-class iOS support
 * via UIKit). Touch input is handled directly by SDL2 and forwarded to the
 * runner as mouse events, so the existing GML mouse_ functions work out of the
 * box. Physical/virtual MFi game controllers are handled via SDL2's
 * SDL_GameController API, which on iOS is backed by the GameController
 * framework.
 *
 * The app bundle layout expected:
 *   <App>.app/
 *     data.win   (or game.ios — checked in order)
 *
 * Saves go to NSDocumentDirectory via SDL_GetPrefPath().
 *
 * Build: PLATFORM=ios, requires SDL2.xcframework linked into the Xcode target.
 */

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_main.h>   /* provides UIApplicationMain on iOS */

#include "common.h"
#include "runner.h"
#include "runner_keyboard.h"
#include "runner_mouse.h"
#include "runner_gamepad.h"
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

#include "gl/gl_renderer.h"   /* types: GLRenderer, GLRenderer_create, etc. */
/* Note: gl_renderer.h is patched by patch_cmake.py to guard glad/glad.h with
 * PLATFORM_IOS, so on iOS it includes <OpenGLES/ES3/gl.h> instead. */

/* Shared platformdefs symbols required by main.c */
#include "ios/platformdefs.h"

/* ===[ Globals ]================================================== */

enum GraphicsAPI gfx = MODERN_GL;
InputRecording *globalInputRecording = NULL;

static Runner         *g_runner   = NULL;
static SDL_Window     *g_window   = NULL;
static SDL_GLContext   g_glctx    = NULL;

/* Up to MAX_GAMEPADS physical controllers tracked by SDL instance id */
static SDL_GameController *g_controllers[MAX_GAMEPADS];

/* ===[ Platform function implementations ]========================= */

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "[iOS] SDL_Init failed: %s\n", SDL_GetError());
        return false;
    }

    /* Request OpenGL ES 3.0 (Metal-backed via SDL on modern iOS) */
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI;
    if (headless) flags = SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN;

    g_window = SDL_CreateWindow(title ? title : "Butterscotch",
                                SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                reqW, reqH, flags);
    if (!g_window) {
        fprintf(stderr, "[iOS] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return false;
    }

    g_glctx = SDL_GL_CreateContext(g_window);
    if (!g_glctx) {
        fprintf(stderr, "[iOS] SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        return false;
    }

    SDL_GL_MakeCurrent(g_window, g_glctx);
    SDL_GL_SetSwapInterval(1); /* vsync */

    for (int i = 0; i < MAX_GAMEPADS; i++)
        g_controllers[i] = NULL;

    return true;
}

void platformExit(void) {
    for (int i = 0; i < MAX_GAMEPADS; i++) {
        if (g_controllers[i]) {
            SDL_GameControllerClose(g_controllers[i]);
            g_controllers[i] = NULL;
        }
    }
    if (g_glctx) { SDL_GL_DeleteContext(g_glctx); g_glctx = NULL; }
    if (g_window) { SDL_DestroyWindow(g_window);  g_window = NULL; }
    SDL_Quit();
}

void platformSwapBuffers(void) {
    SDL_GL_SwapWindow(g_window);
}

void *platformGetProcAddress(const char *name) {
    return SDL_GL_GetProcAddress(name);
}

double platformGetTime(void) {
    return (double)SDL_GetTicks() / 1000.0;
}

bool platformGetWindowSize(int32_t *outW, int32_t *outH) {
    if (!outW || !outH || !g_window) return false;
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(g_window, &w, &h);
    if (w <= 0 || h <= 0) return false;
    *outW = w;
    *outH = h;
    return true;
}

bool platformGetScaledWindowSize(int32_t *outW, int32_t *outH) {
    if (!outW || !outH || !g_window) return false;
    int w = 0, h = 0;
    SDL_GetWindowSize(g_window, &w, &h);
    if (w <= 0 || h <= 0) return false;
    *outW = w;
    *outH = h;
    return true;
}

void platformSetWindowSize(int32_t width, int32_t height) {
    /* Full-screen on iOS — ignore resize requests from GML */
    (void)width; (void)height;
}

void platformSetWindowTitle(const char *title) {
    if (g_window && title) SDL_SetWindowTitle(g_window, title);
}

void platformGetMousePos(double *xPos, double *yPos) {
    if (!xPos || !yPos) return;
    int mx = 0, my = 0;
    SDL_GetMouseState(&mx, &my);
    /* Scale logical → drawable pixels */
    int32_t dw = 0, dh = 0;
    int lw = 0, lh = 0;
    SDL_GL_GetDrawableSize(g_window, &dw, &dh);
    SDL_GetWindowSize(g_window, &lw, &lh);
    float sx = (lw > 0) ? (float)dw / lw : 1.0f;
    float sy = (lh > 0) ? (float)dh / lh : 1.0f;
    *xPos = (double)mx * sx;
    *yPos = (double)my * sy;
}

void platformSleepUntil(uint64_t time) {
    int64_t remaining = (int64_t)(time - (uint64_t)nowNanos());
    if (remaining > 2000000)
        SDL_Delay((Uint32)((remaining - 1000000) / 1000000));
    while ((int64_t)((uint64_t)nowNanos() - time) < 0) {
        YIELD();
    }
}

/* ===[ Key mapping ]============================================== */

static int32_t sdlKeyToGml(SDL_Keycode k) {
    if (k >= 'a' && k <= 'z') return toupper(k);
    if (k >= '0' && k <= '9') return (int32_t)k;
    switch (k) {
        case SDLK_ESCAPE:    return VK_ESCAPE;
        case SDLK_RETURN:    return VK_ENTER;
        case SDLK_TAB:       return VK_TAB;
        case SDLK_BACKSPACE: return VK_BACKSPACE;
        case SDLK_SPACE:     return VK_SPACE;
        case SDLK_LSHIFT:
        case SDLK_RSHIFT:    return VK_SHIFT;
        case SDLK_LCTRL:
        case SDLK_RCTRL:     return VK_CONTROL;
        case SDLK_LALT:
        case SDLK_RALT:      return VK_ALT;
        case SDLK_UP:        return VK_UP;
        case SDLK_DOWN:      return VK_DOWN;
        case SDLK_LEFT:      return VK_LEFT;
        case SDLK_RIGHT:     return VK_RIGHT;
        case SDLK_F1:        return VK_F1;
        case SDLK_F2:        return VK_F2;
        case SDLK_F3:        return VK_F3;
        case SDLK_F4:        return VK_F4;
        case SDLK_F5:        return VK_F5;
        case SDLK_F6:        return VK_F6;
        case SDLK_F7:        return VK_F7;
        case SDLK_F8:        return VK_F8;
        case SDLK_F9:        return VK_F9;
        case SDLK_F10:       return VK_F10;
        case SDLK_F11:       return VK_F11;
        case SDLK_F12:       return VK_F12;
        case SDLK_INSERT:    return VK_INSERT;
        case SDLK_DELETE:    return VK_DELETE;
        case SDLK_HOME:      return VK_HOME;
        case SDLK_END:       return VK_END;
        case SDLK_PAGEUP:    return VK_PAGEUP;
        case SDLK_PAGEDOWN:  return VK_PAGEDOWN;
        default:             return -1;
    }
}

static uint32_t utf8ToCodepoint(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    if (p[0] < 0x80) return p[0];
    if ((p[0] & 0xE0) == 0xC0)
        return ((p[0] & 0x1F) << 6) | (p[1] & 0x3F);
    if ((p[0] & 0xF0) == 0xE0)
        return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
    if ((p[0] & 0xF8) == 0xF0)
        return ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) | (p[3] & 0x3F);
    return 0xFFFD;
}

static int32_t sdlMouseButtonToGml(Uint8 btn) {
    switch (btn) {
        case SDL_BUTTON_LEFT:   return GML_MB_LEFT;
        case SDL_BUTTON_RIGHT:  return GML_MB_RIGHT;
        case SDL_BUTTON_MIDDLE: return GML_MB_MIDDLE;
        default: return -1;
    }
}

/* ===[ Gamepad mapping ]========================================== */

enum { IDX_LT = 6, IDX_RT = 7 };

static void mapSdlToGamepadSlot(SDL_GameController *gc, GamepadSlot *slot) {
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_A))            slot->buttonDown[0]  = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_B))            slot->buttonDown[1]  = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_X))            slot->buttonDown[2]  = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_Y))            slot->buttonDown[3]  = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) slot->buttonDown[4]  = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))slot->buttonDown[5]  = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK))         slot->buttonDown[8]  = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START))        slot->buttonDown[9]  = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_GUIDE))        slot->buttonDown[16] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_LEFTSTICK))    slot->buttonDown[10] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_RIGHTSTICK))   slot->buttonDown[11] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_UP))      slot->buttonDown[12] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_DOWN))    slot->buttonDown[13] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_LEFT))    slot->buttonDown[14] = true;
    if (SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_DPAD_RIGHT))   slot->buttonDown[15] = true;

    float lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  / 32767.0f;
    float rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) / 32767.0f;
    if (lt < 0.0f) lt = 0.0f;
    if (rt < 0.0f) rt = 0.0f;
    slot->buttonValue[IDX_LT] = lt;
    slot->buttonValue[IDX_RT] = rt;
    if (lt >= slot->triggerThreshold) slot->buttonDown[IDX_LT] = true;
    if (rt >= slot->triggerThreshold) slot->buttonDown[IDX_RT] = true;

    float lh = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX)  / 32767.0f;
    float lv = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY)  / 32767.0f;
    float rh = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTX) / 32767.0f;
    float rv = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_RIGHTY) / 32767.0f;
    slot->axisValue[0] = lh;
    slot->axisValue[1] = lv;
    slot->axisValue[2] = rh;
    slot->axisValue[3] = rv;

    for (int i = 0; i < GP_BUTTON_COUNT; i++) {
        if (i == IDX_LT || i == IDX_RT) continue;
        slot->buttonValue[i] = slot->buttonDown[i] ? 1.0f : 0.0f;
    }
}

/* ===[ Event loop ]=============================================== */

bool platformHandleEvents(void) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                return true;

            case SDL_KEYDOWN:
                if (e.key.repeat) break;
                RunnerKeyboard_onKeyDown(g_runner->keyboard, sdlKeyToGml(e.key.keysym.sym));
                break;
            case SDL_KEYUP:
                RunnerKeyboard_onKeyUp(g_runner->keyboard, sdlKeyToGml(e.key.keysym.sym));
                break;
            case SDL_TEXTINPUT:
                RunnerKeyboard_onCharacter(g_runner->keyboard, utf8ToCodepoint(e.text.text));
                break;

            case SDL_MOUSEBUTTONDOWN: {
                int32_t btn = sdlMouseButtonToGml(e.button.button);
                if (btn >= 0) RunnerMouse_onButtonDown(g_runner->mouse, btn);
                break;
            }
            case SDL_MOUSEBUTTONUP: {
                int32_t btn = sdlMouseButtonToGml(e.button.button);
                if (btn >= 0) RunnerMouse_onButtonUp(g_runner->mouse, btn);
                break;
            }
            case SDL_MOUSEWHEEL:
                if (e.wheel.y != 0)
                    RunnerMouse_onWheel(g_runner->mouse, (float)e.wheel.y);
                break;

            /* SDL2 maps MFi / GCController events to SDL_GameController on iOS */
            case SDL_CONTROLLERDEVICEADDED: {
                int devIdx = e.cdevice.which;
                for (int i = 0; i < MAX_GAMEPADS; i++) {
                    if (g_controllers[i] == NULL) {
                        g_controllers[i] = SDL_GameControllerOpen(devIdx);
                        break;
                    }
                }
                break;
            }
            case SDL_CONTROLLERDEVICEREMOVED: {
                SDL_JoystickID iid = e.cdevice.which;
                for (int i = 0; i < MAX_GAMEPADS; i++) {
                    if (g_controllers[i]) {
                        SDL_Joystick *joy = SDL_GameControllerGetJoystick(g_controllers[i]);
                        if (joy && SDL_JoystickInstanceID(joy) == iid) {
                            SDL_GameControllerClose(g_controllers[i]);
                            g_controllers[i] = NULL;
                            break;
                        }
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    /* Update gamepad state */
    if (g_runner && g_runner->gamepads) {
        g_runner->gamepads->connectedCount = 0;
        for (int si = 0; si < MAX_GAMEPADS; si++) {
            GamepadSlot *slot = &g_runner->gamepads->slots[si];
            SDL_GameController *gc = g_controllers[si];

            memcpy(slot->buttonDownPrev, slot->buttonDown, sizeof(slot->buttonDown));
            memset(slot->buttonDown,     0, sizeof(slot->buttonDown));
            memset(slot->buttonPressed,  0, sizeof(slot->buttonPressed));
            memset(slot->buttonReleased, 0, sizeof(slot->buttonReleased));
            memset(slot->buttonValue,    0, sizeof(slot->buttonValue));
            memset(slot->axisValue,      0, sizeof(slot->axisValue));

            if (gc && SDL_GameControllerGetAttached(gc)) {
                slot->connected = true;
                slot->jid = si;
                const char *name = SDL_GameControllerName(gc);
                if (name) {
                    strncpy(slot->description, name, sizeof(slot->description) - 1);
                    slot->description[sizeof(slot->description) - 1] = '\0';
                }
                char guidStr[64] = {0};
                SDL_Joystick *joy = SDL_GameControllerGetJoystick(gc);
                if (joy) SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy), guidStr, sizeof(guidStr));
                strncpy(slot->guid, guidStr, sizeof(slot->guid) - 1);
                slot->guid[sizeof(slot->guid) - 1] = '\0';

                mapSdlToGamepadSlot(gc, slot);
                for (int b = 0; b < GP_BUTTON_COUNT; b++) {
                    bool was = slot->buttonDownPrev[b];
                    if (slot->buttonDown[b] && !was) slot->buttonPressed[b]  = true;
                    if (!slot->buttonDown[b] && was)  slot->buttonReleased[b] = true;
                }
                g_runner->gamepads->connectedCount++;
            } else {
                if (gc) { SDL_GameControllerClose(gc); g_controllers[si] = NULL; }
                slot->connected = false;
                slot->guid[0]   = '\0';
            }
        }
    }

    return false;
}

/* ===[ platformInitFunctions — called after runner is created ]=== */

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
}

/* ===[ main ]==================================================== */

/*
 * On iOS, SDL2 provides its own UIApplicationMain entry point and calls
 * SDL_main (this function) from the app delegate after the run loop starts.
 */
int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    /* Locate data.win inside the app bundle */
    char *basePath = SDL_GetBasePath(); /* returns "<bundle>/  " with trailing slash */
    if (!basePath) {
        fprintf(stderr, "[iOS] SDL_GetBasePath() failed: %s\n", SDL_GetError());
        return 1;
    }

    /* Try data.win, then game.ios (some builds rename it) */
    const char *candidates[] = { "data.win", "game.ios", NULL };
    char dataWinPath[1024] = {0};
    for (int ci = 0; candidates[ci]; ci++) {
        snprintf(dataWinPath, sizeof(dataWinPath), "%s%s", basePath, candidates[ci]);
        FILE *f = fopen(dataWinPath, "rb");
        if (f) { fclose(f); break; }
        dataWinPath[0] = '\0';
    }
    if (dataWinPath[0] == '\0') {
        fprintf(stderr, "[iOS] data.win not found in bundle at %s\n", basePath);
        SDL_free(basePath);
        return 1;
    }

    /* Saves go to the app's Documents directory via SDL_GetPrefPath */
    char *prefPath = SDL_GetPrefPath("Butterscotch", "Undertale");
    if (!prefPath) {
        fprintf(stderr, "[iOS] SDL_GetPrefPath() failed: %s\n", SDL_GetError());
        SDL_free(basePath);
        return 1;
    }

    /* Light parse to get window size */
    DataWin *metaWin = DataWin_parse(dataWinPath, (DataWinParserOptions){
        .parseGen8 = true,
        .parseStrg = true,
    });
    int32_t winW = 640, winH = 480;
    const char *gameTitle = "Butterscotch";
    if (metaWin) {
        if (metaWin->gen8.defaultWindowWidth  > 0) winW = (int32_t)metaWin->gen8.defaultWindowWidth;
        if (metaWin->gen8.defaultWindowHeight > 0) winH = (int32_t)metaWin->gen8.defaultWindowHeight;
        if (metaWin->gen8.displayName && metaWin->gen8.displayName[0])
            gameTitle = metaWin->gen8.displayName;
        else if (metaWin->gen8.name && metaWin->gen8.name[0])
            gameTitle = metaWin->gen8.name;
        DataWin_free(metaWin);
    }

    if (!platformInit(winW, winH, gameTitle, false)) {
        SDL_free(basePath);
        SDL_free(prefPath);
        return 1;
    }

    /* Full parse */
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
        SDL_free(basePath);
        SDL_free(prefPath);
        return 1;
    }

    VMContext  *vm       = VM_create(dataWin);
    Renderer   *renderer = GLRenderer_create();
    FileSystem *fs       = (FileSystem *)OverlayFileSystem_create(basePath, prefPath);

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
    runner->osType = OS_IOS;   /* report iOS to GML (os_type == os_ios == 9) */
    runner->setWindowTitle  = platformSetWindowTitle;
    runner->windowHasFocus  = NULL;
    runner->getWindowSize   = platformGetWindowSize;

    platformInitFunctions(runner);

    Runner_initFirstRoom(runner);

    SDL_free(basePath);
    SDL_free(prefPath);

    /* Main loop */
    uint64_t frameDue = (uint64_t)nowNanos();
    while (true) {
        RunnerKeyboard_beginFrame(runner->keyboard);
        RunnerMouse_beginFrame(runner->mouse);

        /* Clear one-frame gamepad flags */
        if (runner->gamepads) {
            for (int i = 0; i < MAX_GAMEPADS; i++) {
                GamepadSlot *slot = &runner->gamepads->slots[i];
                slot->connectedPrev = slot->connected;
            }
        }

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

        /* Frame pacing */
        uint32_t fps = (runner->currentRoom && runner->currentRoom->speed > 0)
                     ? runner->currentRoom->speed : 30;
        frameDue += (uint64_t)(1000000000ULL / fps);
        platformSleepUntil(frameDue);
    }

    /* Teardown */
    audio->vtable->destroy(audio);
    renderer->vtable->destroy(renderer);
    Runner_free(runner);
    VM_free(vm);
    DataWin_free(dataWin);
    platformExit();
    return 0;
}
