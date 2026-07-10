#ifndef _BS_IOS_PLATFORMDEFS_H_
#define _BS_IOS_PLATFORMDEFS_H_

#include <stdbool.h>
#include <stdint.h>

#include "runner.h"

/* Shared interface expected by main.c (desktop/main.c convention) */
bool   platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless);
void   platformInitFunctions(Runner *runner);
void   platformExit(void);
void   platformSwapBuffers(void);
void  *platformGetProcAddress(const char *name);
double platformGetTime(void);
bool   platformHandleEvents(void);
void   platformGetMousePos(double *xPos, double *yPos);
bool   platformGetWindowSize(int32_t *outW, int32_t *outH);
bool   platformGetScaledWindowSize(int32_t *outW, int32_t *outH);
void   platformSetWindowSize(int32_t width, int32_t height);
void   platformSetWindowTitle(const char *title);
void   platformSleepUntil(uint64_t time);

/* iOS always uses modern GL (GLES via SDL2/Metal) */
enum GraphicsAPI { SOFTWARE = 0, MODERN_GL = 1, LEGACY_GL = 2 };
extern enum GraphicsAPI gfx;

/* Input recording stub — not used on iOS but required by shared headers */
#include "input_recording.h"
extern InputRecording *globalInputRecording;

/* GL version table — not used on iOS, but included to satisfy gl_common headers */
static const struct {
    uint8_t major, minor;
    bool gles;
} GLCommon_versions[] = {
    { 3, 0, true },
    { 2, 0, true },
};

#endif /* _BS_IOS_PLATFORMDEFS_H_ */
