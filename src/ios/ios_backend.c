/*
 * ios_backend.c — UIKit/EAGL platform backend for Butterscotch iOS.
 *
 * Based on Un1q32's iOS port (PR #307, ButterscotchRunner/Butterscotch).
 * Modified for single-game bundle: instead of scanning Documents for games,
 * it auto-launches data.win from the app bundle.
 *
 * This file defines main() and all platform* functions declared in platformdefs.h.
 * game_main() (in main.c) is called on a background thread once the GL context
 * and framebuffer are ready.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dlfcn.h>
#include <limits.h>
#include "math_compat.h"
#include <UIKit/UIKit.h>
#include <CoreGraphics/CoreGraphics.h>
#include <objc/message.h>
#include <OpenGLES/EAGL.h>
#include <QuartzCore/CAEAGLLayer.h>
#include <Availability.h>
#include <OpenGLES/ES3/gl.h>
#include <OpenGLES/ES3/glext.h>
#include "common.h"
#include "input_recording.h"
#include "desktop/platformdefs.h"
#include "gettime.h"
#include "runner_mouse.h"
#ifdef ENABLE_MODERN_GL
#include "gl_renderer.h"
#endif

static atomic_bool needsResize = false;
static atomic_bool quitRequested = false;
static atomic_bool viewLaidOut = false;  /* set by first layoutSubviews */
static atomic_bool appIsActive = false;  /* false until applicationDidBecomeActive */

/* CADisplayLink-based present: game thread signals readyToPresent, display
 * link on main thread does the actual presentRenderbuffer on vsync. */
static dispatch_semaphore_t sema_frameReady;   /* game thread posts, DL waits */
static dispatch_semaphore_t sema_framePresented; /* DL posts, game thread waits */
static CADisplayLink *g_displayLink = nil;
static EAGLContext *glcontext;
static GLuint framebuffer;
static GLuint renderbuffer;
static bool glInited = false;
static GLint fbWidth  = 0;
static GLint fbHeight = 0;
static CAEAGLLayer *layer;

static int32_t g_reqRenderWidth  = 0;
static int32_t g_reqRenderHeight = 0;
static float g_aspectRatio = 1.0f;
static UIView *g_glView = nil;
static UIView *g_overlayView = nil;
static UIDeviceOrientation g_lastAppliedOrientation = UIDeviceOrientationUnknown;
static char g_gamePath[PATH_MAX];
static char g_saveFolderPath[PATH_MAX];

#define BS_KEY_QUEUE_SIZE 32
typedef struct { int32_t key; bool isDown; } BSKeyEvent;
static BSKeyEvent bsKeyQueue[BS_KEY_QUEUE_SIZE];
static atomic_int bsKeyQueueHead = 0;
static atomic_int bsKeyQueueTail = 0;

static void bsEnqueueKeyEvent(int32_t key, bool isDown) {
    int head = atomic_load(&bsKeyQueueHead);
    int next = (head + 1) % BS_KEY_QUEUE_SIZE;
    if (next == atomic_load(&bsKeyQueueTail)) return;
    bsKeyQueue[head].key = key;
    bsKeyQueue[head].isDown = isDown;
    atomic_store(&bsKeyQueueHead, next);
}

static void bsDrainKeyEvents(void) {
    extern void *g_runner_ptr;
    for (;;) {
        int tail = atomic_load(&bsKeyQueueTail);
        if (tail == atomic_load(&bsKeyQueueHead)) break;
        BSKeyEvent ev = bsKeyQueue[tail];
        atomic_store(&bsKeyQueueTail, (tail + 1) % BS_KEY_QUEUE_SIZE);
        /* Key events routed via platformHandleEvents — nothing to do here
         * since the runner pointer is only known inside game_main. We rely
         * on the runner checking the key queue each frame via platformHandleEvents. */
        (void)ev;
    }
}

/* Expose the key queue drain to game_main's platformHandleEvents */
static Runner *g_runner = NULL;

static void bsDrainKeyEventsWithRunner(void) {
    for (;;) {
        int tail = atomic_load(&bsKeyQueueTail);
        if (tail == atomic_load(&bsKeyQueueHead)) break;
        BSKeyEvent ev = bsKeyQueue[tail];
        atomic_store(&bsKeyQueueTail, (tail + 1) % BS_KEY_QUEUE_SIZE);
        if (g_runner) {
            if (ev.isDown) RunnerKeyboard_onKeyDown(g_runner->keyboard, ev.key);
            else           RunnerKeyboard_onKeyUp(g_runner->keyboard, ev.key);
        }
    }
}

#define BS_CONTROL_STRIP_PORTRAIT_H   160.0f
#define BS_CONTROL_STRIP_LANDSCAPE_W  120.0f
#define BS_QUIT_BUTTON_SIZE           22.0f
#define BS_QUIT_BUTTON_MARGIN         8.0f
#define BS_FF_BUTTON_SIZE             32.0f
#define BS_GAME_TOP_PADDING           (BS_QUIT_BUTTON_MARGIN + BS_FF_BUTTON_SIZE + 4.0f)

typedef struct {
    CGRect gameFrame;
    CGRect dpadFrame;
    CGRect buttonsFrame;
    CGRect quitFrame;
    CGRect ffFrame;
    bool   portrait;
} BSLayout;

static bool bsDeviceHasNotch(void) {
    CGRect bounds = [[UIScreen mainScreen] bounds];
    CGFloat maxDim = fmaxf(bounds.size.width, bounds.size.height);
    CGFloat minDim = fminf(bounds.size.width, bounds.size.height);
    return (maxDim / minDim > 2.0f);
}

static BSLayout computeLayout(CGSize screen) {
    BSLayout layout;
    layout.portrait = screen.height >= screen.width;
    if (layout.portrait) {
        CGFloat neededH = screen.width / g_aspectRatio;
        CGFloat gameH = fminf(neededH, screen.height - BS_GAME_TOP_PADDING);
        layout.gameFrame = CGRectMake(0, BS_GAME_TOP_PADDING, screen.width, gameH);
        CGFloat stripY = screen.height - BS_CONTROL_STRIP_PORTRAIT_H;
        layout.dpadFrame    = CGRectMake(16, stripY + 10, 130, 130);
        layout.buttonsFrame = CGRectMake(screen.width - 16 - 150, stripY + 30, 150, 50);
    } else {
        CGFloat neededW = screen.height * g_aspectRatio;
        CGFloat gameW = fminf(neededW, screen.width);
        CGFloat gameX = (screen.width - gameW) / 2.0f;
        layout.gameFrame = CGRectMake(gameX, 0, gameW, screen.height);
        CGFloat notchInset = bsDeviceHasNotch() ? 44.0f : 0.0f;
        layout.dpadFrame    = CGRectMake(10 + notchInset, (screen.height - 130) / 2.0f + 35, 110, 130);
        layout.buttonsFrame = CGRectMake(screen.width - notchInset - 10 - 170, (screen.height - 130) / 2.0f + 60, 170, 60);
    }
    layout.quitFrame = CGRectMake(screen.width - BS_QUIT_BUTTON_MARGIN - BS_QUIT_BUTTON_SIZE,
                                   BS_QUIT_BUTTON_MARGIN, BS_QUIT_BUTTON_SIZE, BS_QUIT_BUTTON_SIZE);
    layout.ffFrame = CGRectMake(BS_QUIT_BUTTON_MARGIN, BS_QUIT_BUTTON_MARGIN,
                                 BS_FF_BUTTON_SIZE, BS_FF_BUTTON_SIZE);
    return layout;
}

static CGFloat joystickRadius(CGRect dpad) {
    return fminf(dpad.size.width, dpad.size.height) * 0.45f;
}

static CGPoint joystickClampOffset(CGRect dpad, CGPoint touchPoint) {
    CGFloat cx = dpad.origin.x + dpad.size.width  / 2.0f;
    CGFloat cy = dpad.origin.y + dpad.size.height / 2.0f;
    CGFloat dx = touchPoint.x - cx;
    CGFloat dy = touchPoint.y - cy;
    CGFloat maxR = joystickRadius(dpad);
    CGFloat dist = sqrtf(dx*dx + dy*dy);
    if (dist < maxR) return CGPointMake(dx, dy);
    return CGPointMake(dx / dist * maxR, dy / dist * maxR);
}

static void dpadDirectionsForPoint(CGRect dpad, CGPoint p, bool *up, bool *down, bool *left, bool *right) {
    CGFloat cx = dpad.origin.x + dpad.size.width  / 2.0f;
    CGFloat cy = dpad.origin.y + dpad.size.height / 2.0f;
    CGFloat dx = p.x - cx;
    CGFloat dy = p.y - cy;
    CGFloat deadzone = fminf(dpad.size.width, dpad.size.height) * 0.15f;
    *up = *down = *left = *right = false;
    if (fabs(dx) < deadzone && fabs(dy) < deadzone) return;
    CGFloat deg = atan2f(dy, dx) * 180.0f / (CGFloat)M_PI;
    if (deg < 0) deg += 360.0f;
    if      (deg >= 337.5f || deg < 22.5f) { *right = true; }
    else if (deg < 67.5f)                  { *right = true; *down = true; }
    else if (deg < 112.5f)                 { *down = true; }
    else if (deg < 157.5f)                 { *down = true; *left = true; }
    else if (deg < 202.5f)                 { *left = true; }
    else if (deg < 247.5f)                 { *left = true; *up = true; }
    else if (deg < 292.5f)                 { *up = true; }
    else                                    { *up = true; *right = true; }
}

static void actionButtonRects(CGRect bf, CGRect out[3]) {
    CGFloat btnSize = fminf(bf.size.height, bf.size.width / 3.0f) - 8.0f;
    for (int i = 0; i < 3; i++) {
        out[i] = CGRectMake(bf.origin.x + i * (bf.size.width / 3.0f) + 4.0f,
                             bf.origin.y + (bf.size.height - btnSize) / 2.0f,
                             btnSize, btnSize);
    }
}

static void bsRequestRelayout(void) {
    if (g_glView) {
        [g_glView performSelectorOnMainThread:@selector(setNeedsLayout) withObject:nil waitUntilDone:YES];
        [g_glView performSelectorOnMainThread:@selector(layoutIfNeeded) withObject:nil waitUntilDone:YES];
    }
    if (g_overlayView) {
        [g_overlayView performSelectorOnMainThread:@selector(setNeedsLayout) withObject:nil waitUntilDone:YES];
        [g_overlayView performSelectorOnMainThread:@selector(layoutIfNeeded) withObject:nil waitUntilDone:YES];
        [g_overlayView performSelectorOnMainThread:@selector(setNeedsDisplay) withObject:nil waitUntilDone:YES];
    }
}

/* ── Platform function implementations ────────────────────────── */

static void resizeFramebuffer(void); /* forward declaration */

void platformSetWindowTitle(const char* title) { (void)title; }

bool platformGetWindowSize(int32_t* outW, int32_t* outH) {
    if (!outW || !outH) return false;
    if (fbWidth <= 0 || fbHeight <= 0) {
        /* Layer not ready yet — retry framebuffer allocation */
        resizeFramebuffer();
    }
    if (fbWidth <= 0 || fbHeight <= 0) return false;
    *outW = fbWidth; *outH = fbHeight;
    return true;
}

bool platformGetScaledWindowSize(int32_t* outW, int32_t* outH) {
    if (!outW || !outH) return false;
    CGRect bounds = [[UIScreen mainScreen] bounds];
    if (bounds.size.width <= 0 || bounds.size.height <= 0) return false;
    *outW = bounds.size.width; *outH = bounds.size.height;
    return true;
}

void platformSetWindowSize(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0) return;
    g_reqRenderWidth  = width;
    g_reqRenderHeight = height;
    g_aspectRatio = (float)width / (float)height;
    bsRequestRelayout();
    atomic_store(&needsResize, true);
}

void platformGetMousePos(double *xPos, double *yPos) {
    *xPos = 0.0; *yPos = 0.0;
}

static CGFloat nativeScreenScale(void) {
    UIScreen *screen = [UIScreen mainScreen];
    if ([screen respondsToSelector:@selector(scale)]) {
        CGFloat (*getScale)(id, SEL) = (CGFloat (*)(id, SEL))objc_msgSend;
        return getScale(screen, @selector(scale));
    }
    return 1.0f;
}

static void applyRenderScale(void) {
    if (!layer) return;
    CGSize sz = layer.bounds.size;
    if (sz.width <= 0.0f || sz.height <= 0.0f) return;
    CGFloat nativeScale = nativeScreenScale();
    CGFloat targetScale = nativeScale;
    if (g_reqRenderWidth > 0 && g_reqRenderHeight > 0) {
        CGFloat sW = (CGFloat)g_reqRenderWidth  / sz.width;
        CGFloat sH = (CGFloat)g_reqRenderHeight / sz.height;
        targetScale = fminf(nativeScale, fminf(sW, sH));
    }
    if (targetScale <= 0.0f) targetScale = nativeScale;
    targetScale += 0.0005f;
    if (targetScale > nativeScale) targetScale = nativeScale;
    if ([layer respondsToSelector:@selector(setContentsScale:)]) {
        void (*setScale)(id, SEL, CGFloat) = (void (*)(id, SEL, CGFloat))objc_msgSend;
        setScale(layer, @selector(setContentsScale:), targetScale);
    }
}

static void resizeFramebuffer(void) {
    applyRenderScale();
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    [glcontext renderbufferStorage:GL_RENDERBUFFER fromDrawable:layer];
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &fbWidth);
    glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &fbHeight);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
#ifdef ENABLE_MODERN_GL
    if (gfx == MODERN_GL && g_runner && g_runner->renderer)
        ((GLRenderer *)g_runner->renderer)->hostFramebuffer = framebuffer;
#endif
    glViewport(0, 0, fbWidth, fbHeight);
    GLenum fbStatus = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    NSLog(@"[BS] resizeFramebuffer: %dx%d fbo=%u rb=%u status=0x%x",
          fbWidth, fbHeight, framebuffer, renderbuffer, fbStatus);
}

bool platformInit(int32_t reqW, int32_t reqH, const char *title, bool headless) {
    (void)title; (void)headless;
    g_aspectRatio = (reqH > 0) ? ((float)reqW / (float)reqH) : 1.0f;
    g_reqRenderWidth  = reqW;
    g_reqRenderHeight = reqH;
    bsRequestRelayout();
    glcontext = [[EAGLContext alloc] initWithAPI:3];
    if (!glcontext) glcontext = [[EAGLContext alloc] initWithAPI:2];
    if (!glcontext) { fprintf(stderr, "Failed to create OpenGLES context\n"); return false; }
    if (![EAGLContext setCurrentContext:glcontext]) {
        [glcontext release]; glcontext = nil;
        fprintf(stderr, "Failed to set OpenGLES context\n"); return false;
    }
    return true;
}

void platformExit(void) {
    if (framebuffer) { glDeleteFramebuffers(1, &framebuffer); framebuffer = 0; }
    if (renderbuffer) { glDeleteRenderbuffers(1, &renderbuffer); renderbuffer = 0; }
    [glcontext release]; glcontext = nil;
    glInited = false;
}

void platformInitFunctions(Runner *runner) {
    g_runner = runner;
    /* framebuffer/renderbuffer already created in startGame before game thread launch */
    if (!glInited) {
        glGenFramebuffers(1, &framebuffer);
        glGenRenderbuffers(1, &renderbuffer);
        glInited = true;
    }
    /* The EAGL layer's drawable may not be ready yet even after layoutSubviews.
     * Retry until we get a non-zero framebuffer size (max ~2.5 s). */
    for (int attempt = 0; attempt < 50; attempt++) {
        resizeFramebuffer();
        if (fbWidth > 0 && fbHeight > 0) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 }; /* 50 ms */
        nanosleep(&ts, NULL);
    }
    NSLog(@"[BS] platformInitFunctions done: %dx%d", fbWidth, fbHeight);
    fprintf(stderr, "[BS] platformInitFunctions done: %dx%d\n", fbWidth, fbHeight);
    atomic_store(&needsResize, false);
}

static int g_swapCount = 0;
void platformSwapBuffers(void) {
    /* GLRenderer may have left a different FBO/RBO bound — restore ours */
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
    glFlush();

    g_swapCount++;
    fprintf(stderr, "[BS] swap #%d: signaling frameReady\n", g_swapCount);

    dispatch_semaphore_signal(sema_frameReady);

    fprintf(stderr, "[BS] swap #%d: waiting for framePresented\n", g_swapCount);
    long r = dispatch_semaphore_wait(sema_framePresented, dispatch_time(DISPATCH_TIME_NOW, 3LL * NSEC_PER_SEC));
    if (r != 0) {
        fprintf(stderr, "[BS] swap #%d: TIMEOUT waiting for framePresented!\n", g_swapCount);
    } else {
        fprintf(stderr, "[BS] swap #%d: framePresented received\n", g_swapCount);
    }

    [EAGLContext setCurrentContext:glcontext];
}

void *platformGetProcAddress(const char *name) {
    return dlsym(RTLD_NEXT, name);
}

bool platformHandleEvents(void) {
    bsDrainKeyEventsWithRunner();
    if (atomic_exchange(&needsResize, false)) resizeFramebuffer();
    if (atomic_load(&quitRequested)) return true;
    return false;
}

void platformSleepUntil(uint64_t time) {
    int64_t remaining = time - nowNanos();
    if (remaining > 2000000) {
        remaining -= 1000000;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = remaining };
        nanosleep(&ts, NULL);
    }
    while (nowNanos() < time) { YIELD(); }
}

/* ── GLView ───────────────────────────────────────────────────── */

@interface GLView : UIView
@end
@implementation GLView
+ (Class)layerClass { return [CAEAGLLayer class]; }
- (id)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        layer = (CAEAGLLayer *)self.layer;
        layer.opaque = YES;
        self.autoresizingMask = UIViewAutoresizingNone;
        CGFloat scale = nativeScreenScale();
        if ([layer respondsToSelector:@selector(setContentsScale:)]) {
            void (*setScale)(id, SEL, CGFloat) = (void (*)(id, SEL, CGFloat))objc_msgSend;
            setScale(layer, @selector(setContentsScale:), scale);
        }
    }
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    if (self.superview) {
        BSLayout bsLayout = computeLayout(self.superview.bounds.size);
        self.frame = bsLayout.gameFrame;
    }
    atomic_store(&needsResize, true);
    /* Do NOT set viewLaidOut here — the CAEAGLLayer isn't committed to the
     * display server yet at this point on cold launch.  We set it in
     * applicationDidBecomeActive instead, which fires only after the app
     * is fully foregrounded and the layer is live. */
}
- (void)dealloc { [super dealloc]; }
@end

/* ── Touch overlay ────────────────────────────────────────────── */

@interface BSTouchOverlay : UIView {
    UITouch *dpadTouch;
    int32_t  dpadKeysDown[4];
    CGPoint  joystickThumbOffset;
    UITouch *buttonTouches[3];
    UITouch *quitTouch;
    UITouch *ffTouch;
}
@end

@implementation BSTouchOverlay
- (id)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        self.backgroundColor = [UIColor clearColor];
        self.opaque = NO;
        self.multipleTouchEnabled = YES;
        self.userInteractionEnabled = YES;
        self.autoresizingMask = UIViewAutoresizingNone;
        dpadTouch = nil; quitTouch = nil; ffTouch = nil;
        for (int i = 0; i < 4; i++) dpadKeysDown[i] = 0;
        joystickThumbOffset = CGPointZero;
        for (int i = 0; i < 3; i++) buttonTouches[i] = nil;
    }
    return self;
}
- (void)layoutSubviews {
    [super layoutSubviews];
    if (self.superview) self.frame = self.superview.bounds;
    [self setNeedsDisplay];
}
static void drawTranslucentCircle(CGContextRef ctx, CGRect frame, BOOL highlighted) {
    CGFloat alpha = highlighted ? 0.55f : 0.30f;
    CGContextSetRGBFillColor(ctx, 1,1,1,alpha);
    CGContextFillEllipseInRect(ctx, frame);
    CGContextSetRGBStrokeColor(ctx, 1,1,1,0.6f);
    CGContextStrokeEllipseInRect(ctx, frame);
}
static void drawCenteredLabel(NSString *text, CGRect rect, UIFont *font) {
    CGSize size = [text sizeWithAttributes:@{NSFontAttributeName: font}];
    CGPoint pt = CGPointMake(rect.origin.x + (rect.size.width  - size.width)  / 2.0f,
                              rect.origin.y + (rect.size.height - size.height) / 2.0f);
    [[UIColor whiteColor] set];
    [text drawAtPoint:pt withAttributes:@{NSFontAttributeName: font}];
}
- (void)drawRect:(CGRect)rect {
    (void)rect;
    CGContextRef ctx = UIGraphicsGetCurrentContext();
    BSLayout bsLayout = computeLayout(self.bounds.size);
    CGFloat cx = bsLayout.dpadFrame.origin.x + bsLayout.dpadFrame.size.width / 2.0f;
    CGFloat cy = bsLayout.dpadFrame.origin.y + bsLayout.dpadFrame.size.height / 2.0f;
    CGFloat baseR = joystickRadius(bsLayout.dpadFrame);
    CGRect baseRect = CGRectMake(cx - baseR, cy - baseR, baseR * 2, baseR * 2);
    BOOL anyDpadDown = dpadKeysDown[0] || dpadKeysDown[1] || dpadKeysDown[2] || dpadKeysDown[3];
    CGContextSetRGBFillColor(ctx, 1,1,1, anyDpadDown ? 0.55f : 0.28f);
    CGContextFillEllipseInRect(ctx, baseRect);
    CGContextSetRGBStrokeColor(ctx, 1,1,1,0.6f);
    CGContextStrokeEllipseInRect(ctx, baseRect);
    CGFloat thumbR = baseR * 0.35f;
    CGRect thumbRect = CGRectMake(cx + joystickThumbOffset.x - thumbR,
                                   cy + joystickThumbOffset.y - thumbR,
                                   thumbR * 2, thumbR * 2);
    CGContextSetRGBFillColor(ctx, 1,1,1,0.55f);
    CGContextFillEllipseInRect(ctx, thumbRect);
    CGRect actionRects[3];
    actionButtonRects(bsLayout.buttonsFrame, actionRects);
    NSString *actionLabels[3] = { @"Z", @"X", @"C" };
    UIFont *actionFont = [UIFont boldSystemFontOfSize:18.0f];
    for (int i = 0; i < 3; i++) {
        drawTranslucentCircle(ctx, actionRects[i], buttonTouches[i] != nil);
        drawCenteredLabel(actionLabels[i], actionRects[i], actionFont);
    }
    drawTranslucentCircle(ctx, bsLayout.quitFrame, quitTouch != nil);
    drawCenteredLabel(@"X", bsLayout.quitFrame, [UIFont boldSystemFontOfSize:14.0f]);
    drawTranslucentCircle(ctx, bsLayout.ffFrame, ffTouch != nil);
    drawCenteredLabel(@">>", bsLayout.ffFrame, [UIFont boldSystemFontOfSize:16.0f]);
}
- (void)updateDpadUp:(bool)up down:(bool)down left:(bool)left right:(bool)right {
    bool newState[4] = { up, down, left, right };
    int32_t keys[4] = { VK_UP, VK_DOWN, VK_LEFT, VK_RIGHT };
    for (int i = 0; i < 4; i++) {
        if (newState[i] && !dpadKeysDown[i])  bsEnqueueKeyEvent(keys[i], true);
        else if (!newState[i] && dpadKeysDown[i]) bsEnqueueKeyEvent(keys[i], false);
        dpadKeysDown[i] = newState[i] ? 1 : 0;
    }
}
- (void)touchesBegan:(NSSet *)touches withEvent:(UIEvent *)event {
    (void)event;
    BSLayout bsLayout = computeLayout(self.bounds.size);
    CGRect actionRects[3];
    actionButtonRects(bsLayout.buttonsFrame, actionRects);
    for (UITouch *touch in touches) {
        CGPoint p = [touch locationInView:self];
        if (!quitTouch && CGRectContainsPoint(CGRectInset(bsLayout.quitFrame, -6,-6), p)) {
            quitTouch = [touch retain]; [self setNeedsDisplay]; continue;
        }
        if (!ffTouch && CGRectContainsPoint(CGRectInset(bsLayout.ffFrame, -6,-6), p)) {
            ffTouch = [touch retain]; bsEnqueueKeyEvent(VK_TAB, true); [self setNeedsDisplay]; continue;
        }
        if (!dpadTouch && CGRectContainsPoint(CGRectInset(bsLayout.dpadFrame, -20,-20), p)) {
            dpadTouch = [touch retain];
            bool up, down, left, right;
            dpadDirectionsForPoint(bsLayout.dpadFrame, p, &up, &down, &left, &right);
            [self updateDpadUp:up down:down left:left right:right];
            joystickThumbOffset = joystickClampOffset(bsLayout.dpadFrame, p);
            [self setNeedsDisplay]; continue;
        }
        for (int i = 0; i < 3; i++) {
            if (!buttonTouches[i] && CGRectContainsPoint(CGRectInset(actionRects[i], -6,-6), p)) {
                buttonTouches[i] = [touch retain];
                int32_t vk = (i == 0) ? 'Z' : (i == 1) ? 'X' : 'C';
                bsEnqueueKeyEvent(vk, true); [self setNeedsDisplay]; break;
            }
        }
    }
}
- (void)touchesMoved:(NSSet *)touches withEvent:(UIEvent *)event {
    (void)event;
    if (!dpadTouch) return;
    BSLayout bsLayout = computeLayout(self.bounds.size);
    for (UITouch *touch in touches) {
        if (touch == dpadTouch) {
            CGPoint p = [touch locationInView:self];
            bool up, down, left, right;
            dpadDirectionsForPoint(bsLayout.dpadFrame, p, &up, &down, &left, &right);
            [self updateDpadUp:up down:down left:left right:right];
            joystickThumbOffset = joystickClampOffset(bsLayout.dpadFrame, p);
            [self setNeedsDisplay]; break;
        }
    }
}
- (void)handleTouchEnd:(NSSet *)touches {
    for (UITouch *touch in touches) {
        if (touch == dpadTouch) {
            [self updateDpadUp:false down:false left:false right:false];
            joystickThumbOffset = CGPointZero;
            [dpadTouch release]; dpadTouch = nil; [self setNeedsDisplay]; continue;
        }
        if (touch == quitTouch) {
            [quitTouch release]; quitTouch = nil;
            atomic_store(&quitRequested, true); [self setNeedsDisplay]; continue;
        }
        if (touch == ffTouch) {
            bsEnqueueKeyEvent(VK_TAB, false);
            [ffTouch release]; ffTouch = nil; [self setNeedsDisplay]; continue;
        }
        for (int i = 0; i < 3; i++) {
            if (touch == buttonTouches[i]) {
                int32_t vk = (i == 0) ? 'Z' : (i == 1) ? 'X' : 'C';
                bsEnqueueKeyEvent(vk, false);
                [buttonTouches[i] release]; buttonTouches[i] = nil; [self setNeedsDisplay]; break;
            }
        }
    }
}
- (void)touchesEnded:(NSSet *)touches withEvent:(UIEvent *)event { (void)event; [self handleTouchEnd:touches]; }
- (void)touchesCancelled:(NSSet *)touches withEvent:(UIEvent *)event { (void)event; [self handleTouchEnd:touches]; }
- (void)dealloc {
    if (dpadTouch) [dpadTouch release];
    if (quitTouch) [quitTouch release];
    if (ffTouch) [ffTouch release];
    for (int i = 0; i < 3; i++) if (buttonTouches[i]) [buttonTouches[i] release];
    [super dealloc];
}
@end

/* ── View controller ──────────────────────────────────────────── */

@interface BSViewController : UIViewController
@end
@implementation BSViewController
- (BOOL)shouldAutorotateToInterfaceOrientation:(UIInterfaceOrientation)interfaceOrientation {
    return interfaceOrientation == UIInterfaceOrientationPortrait;
}
- (BOOL)shouldAutorotate { return NO; }
- (NSUInteger)supportedInterfaceOrientations { return 1; }
- (BOOL)wantsFullScreenLayout { return YES; }
@end

/* ── AppDelegate ──────────────────────────────────────────────── */

extern int game_main(int argc, char *argv[]);

@interface AppDelegate : NSObject <UIApplicationDelegate> {
    UIWindow *window;
    GLView *view;
    BSTouchOverlay *overlay;
    UIView *rootView;
    BOOL usingRootViewController;
    /* _startupDisplayLink removed — using g_displayLink global */
    BOOL _gameThreadStarted;
}
- (void)startGame;
- (void)applyDeviceOrientation:(UIDeviceOrientation)devOrientation;
- (void)orientationChanged:(NSNotification *)note;
@end

@implementation AppDelegate

- (void)gameThread {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    /* Wait for the CADisplayLink first-vsync signal */
    while (!atomic_load(&viewLaidOut)) {
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 8000000 }; /* 8ms */
        nanosleep(&ts, NULL);
    }

    /* The display link already waited for UIApplicationStateActive before
     * setting viewLaidOut, so the GPU is ready by the time we get here. */

    static char arg0[] = "butterscotch";
    static char arg1[] = "--lazy-textures";
    static char arg2[] = "--lazy-rooms";
    static char arg3[] = "--load-type=load-per-chunk";
    static char arg4[] = "--save-folder";

    char *argv[] = { arg0, arg1, arg2, arg3, arg4, g_saveFolderPath, g_gamePath, NULL };
    int argc = 7;

    game_main(argc, argv);

    [pool release];
}

- (void)startGame {
    /* Locate data.win in the app bundle */
    NSBundle *bundle = [NSBundle mainBundle];
    NSString *dataWinPath = [bundle pathForResource:@"data" ofType:@"win"];
    if (!dataWinPath) dataWinPath = [bundle pathForResource:@"game" ofType:@"ios"];
    if (!dataWinPath) {
        fprintf(stderr, "[iOS] data.win not found in bundle\n");
        return;
    }
    strlcpy(g_gamePath, [dataWinPath fileSystemRepresentation], sizeof(g_gamePath));

    /* Save folder in Documents/saves/ */
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *docs = [paths objectAtIndex:0];
    NSString *saveDir = [docs stringByAppendingPathComponent:@"saves"];
    [[NSFileManager defaultManager] createDirectoryAtPath:saveDir
                              withIntermediateDirectories:YES attributes:nil error:nil];
    strlcpy(g_saveFolderPath, [saveDir fileSystemRepresentation], sizeof(g_saveFolderPath));

    atomic_store(&quitRequested, false);
    atomic_store(&viewLaidOut, false);
    _gameThreadStarted = NO;

    sema_frameReady     = dispatch_semaphore_create(0);
    sema_framePresented = dispatch_semaphore_create(0);

    CGRect bounds = [[UIScreen mainScreen] bounds];
    BSLayout bsLayout = computeLayout(bounds.size);

    view = [[GLView alloc] initWithFrame:bsLayout.gameFrame];
    overlay = [[BSTouchOverlay alloc] initWithFrame:bounds];
    g_glView = view;
    g_overlayView = overlay;

    rootView = [[UIView alloc] initWithFrame:bounds];
    rootView.autoresizingMask = UIViewAutoresizingNone;
    [rootView addSubview:view];
    [rootView addSubview:overlay];

    g_lastAppliedOrientation = UIDeviceOrientationUnknown;
    [self applyDeviceOrientation:[[UIDevice currentDevice] orientation]];

    if (usingRootViewController) {
        UIViewController *vc = [[BSViewController alloc] init];
        vc.view = rootView;
        [window performSelector:@selector(setRootViewController:) withObject:vc];
        [vc release];
    } else {
        NSArray *subs = [[window.subviews copy] autorelease];
        for (UIView *sub in subs) [sub removeFromSuperview];
        [window addSubview:rootView];
    }

    /* Pre-create the GL framebuffer/renderbuffer on the main thread so the
     * display link probe can call presentRenderbuffer before the game thread starts. */
    if (!glcontext) {
        glcontext = [[EAGLContext alloc] initWithAPI:3];
        if (!glcontext) glcontext = [[EAGLContext alloc] initWithAPI:2];
    }
    if (glcontext && [EAGLContext setCurrentContext:glcontext] && !glInited) {
        glGenFramebuffers(1, &framebuffer);
        glGenRenderbuffers(1, &renderbuffer);
        glInited = true;
        /* Attach renderbuffer storage from the layer so presentRenderbuffer works */
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        [glcontext renderbufferStorage:GL_RENDERBUFFER fromDrawable:layer];
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &fbWidth);
        glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &fbHeight);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, renderbuffer);
        fprintf(stderr, "[BS] startGame: GL preinit %dx%d\n", fbWidth, fbHeight);
    }

    /* Single CADisplayLink used for both startup signaling and frame presenting */
    g_displayLink = [CADisplayLink displayLinkWithTarget:self
                                                selector:@selector(_displayLinkTick:)];
    [g_displayLink addToRunLoop:[NSRunLoop mainRunLoop]
                        forMode:NSRunLoopCommonModes];

    [NSThread detachNewThreadSelector:@selector(gameThread) toTarget:self withObject:nil];
}

- (void)orientationChanged:(NSNotification *)note {
    (void)note;
    [self applyDeviceOrientation:[[UIDevice currentDevice] orientation]];
}

- (void)applyDeviceOrientation:(UIDeviceOrientation)devOrientation {
    if (!rootView) return;
    CGFloat angle; BOOL swapped;
    switch (devOrientation) {
        case UIDeviceOrientationPortrait:           angle = 0.0f;             swapped = NO;  break;
        case UIDeviceOrientationPortraitUpsideDown: angle = (CGFloat)M_PI;    swapped = NO;  break;
        case UIDeviceOrientationLandscapeLeft:      angle = (CGFloat)M_PI_2;  swapped = YES; break;
        case UIDeviceOrientationLandscapeRight:     angle = -(CGFloat)M_PI_2; swapped = YES; break;
        default: return;
    }
    if (devOrientation == g_lastAppliedOrientation) return;
    g_lastAppliedOrientation = devOrientation;
    CGRect nativeBounds = [[UIScreen mainScreen] bounds];
    CGSize logicalSize = swapped ? CGSizeMake(nativeBounds.size.height, nativeBounds.size.width)
                                  : nativeBounds.size;
    [UIView beginAnimations:nil context:NULL];
    [UIView setAnimationDuration:0.3];
    rootView.transform = CGAffineTransformMakeRotation(angle);
    rootView.bounds = CGRectMake(0, 0, logicalSize.width, logicalSize.height);
    rootView.center = CGPointMake(CGRectGetMidX(nativeBounds), CGRectGetMidY(nativeBounds));
    [UIView commitAnimations];
    bsRequestRelayout();
}

/* CADisplayLink callback — runs on the main thread every vsync.
 * On the first tick, signals the game thread to start.
 * On subsequent ticks, presents the renderbuffer if the game thread
 * has finished rendering a frame. */
static int g_dlTickCount = 0;
- (void)_displayLinkTick:(CADisplayLink *)link {
    (void)link;
    g_dlTickCount++;

    if (!_gameThreadStarted) {
        /* Probe whether the GPU will actually accept a present.
         * Under LiveContainer the guest starts in background; presentRenderbuffer
         * returns NO (and the GPU logs kIOGPUCommandBufferCallbackErrorBackground...)
         * until the host app grants foreground GPU access.  We try a dummy present
         * each tick until it returns YES, then signal the game thread to start.
         * This works regardless of whether applicationDidBecomeActive fires. */
        if (!glcontext || !layer) return;
        [EAGLContext setCurrentContext:glcontext];
        /* Bind our renderbuffer so the present has something to show */
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        /* Clear to black — this is the "splash" frame while we wait */
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glFlush();
        BOOL ok = [glcontext presentRenderbuffer:GL_RENDERBUFFER];
        if (g_dlTickCount % 60 == 0)
            fprintf(stderr, "[BS] DL tick #%d: probe present=%s\n",
                    g_dlTickCount, ok ? "YES" : "NO");
        if (!ok) return; /* GPU not ready yet — try again next vsync */

        /* presentRenderbuffer succeeded — GPU access is granted, start game */
        fprintf(stderr, "[BS] DL tick #%d: GPU ready, starting game thread\n", g_dlTickCount);
        _gameThreadStarted = YES;
        atomic_store(&viewLaidOut, true);
        return;
    }

    /* Normal frame present: game thread signals when a frame is ready */
    if (dispatch_semaphore_wait(sema_frameReady, DISPATCH_TIME_NOW) == 0) {
        [EAGLContext setCurrentContext:glcontext];
        glBindRenderbuffer(GL_RENDERBUFFER, renderbuffer);
        [glcontext presentRenderbuffer:GL_RENDERBUFFER];
        dispatch_semaphore_signal(sema_framePresented);
    }
}

- (void)applicationDidBecomeActive:(UIApplication *)application {
    atomic_store(&appIsActive, true);
}

- (void)applicationWillResignActive:(UIApplication *)application {
    atomic_store(&appIsActive, false);
}

- (void)applicationDidFinishLaunching:(UIApplication *)application {
    [application setStatusBarHidden:YES];
    [[UIDevice currentDevice] beginGeneratingDeviceOrientationNotifications];
    [[NSNotificationCenter defaultCenter] addObserver:self
                                              selector:@selector(orientationChanged:)
                                                  name:UIDeviceOrientationDidChangeNotification
                                                object:nil];
    CGRect bounds = [[UIScreen mainScreen] bounds];
    window = [[UIWindow alloc] initWithFrame:bounds];
    usingRootViewController = [window respondsToSelector:@selector(setRootViewController:)];
    [window makeKeyAndVisible];
    [self startGame];
}

- (void)dealloc {
    [[NSNotificationCenter defaultCenter] removeObserver:self
        name:UIDeviceOrientationDidChangeNotification object:nil];
    [[UIDevice currentDevice] endGeneratingDeviceOrientationNotifications];
    g_glView = nil; g_overlayView = nil;
    [overlay release]; [rootView release]; [view release]; [window release];
    [super dealloc];
}
@end

/* ── Entry point ──────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    NSAutoreleasePool *pool = [[NSAutoreleasePool alloc] init];

    /* Redirect stdout/stderr to a log file in Documents */
    NSArray *paths = NSSearchPathForDirectoriesInDomains(NSDocumentDirectory, NSUserDomainMask, YES);
    NSString *docs = [paths objectAtIndex:0];
    NSString *logPath = [docs stringByAppendingPathComponent:@"latest_log.txt"];
    FILE *f = fopen([logPath fileSystemRepresentation], "w");
    if (f) {
        dup2(fileno(f), STDOUT_FILENO);
        dup2(fileno(f), STDERR_FILENO);
        setbuf(stdout, NULL);
        setbuf(stderr, NULL);
    }

    int ret = UIApplicationMain(argc, argv, nil, @"AppDelegate");
    [pool release];
    return ret;
}
