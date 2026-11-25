#include <glad/gl.h>
#include <SDL.h>
#include <switch.h>
#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include "core.h"
#include "switch/output.h"

#define ASSET_ROOT "sdmc:/switch/gtavcs_nx/gamefolder"

static int nx_socket = -1;
static char last_message[512];
static int resumed;
static unsigned focus_notifications;

static void lifecycle_hook(AppletHookType type, void *parameter)
{
    (void)parameter;
    if (type == AppletHookType_OnResume) resumed = 1;
    if (type == AppletHookType_OnFocusState) ++focus_notifications;
}

/* stderr remains on SD; stdout is optionally sent to the nxlink listener. */
static void diagnostic(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    vsnprintf(last_message, sizeof(last_message), format, args);
    va_end(args);
    fputs(last_message, stderr);
    fflush(stderr);
    if (nx_socket >= 0) { fputs(last_message, stdout); fflush(stdout); }
}

/* A failure after SDL started: the system's error dialog shows the message without needing our display window
 * (see fatal_screen). It stays up until the player presses OK; the full text is under Details. */
static void error_dialog(int in_play)
{
    char details[1024];
    snprintf(details, sizeof(details), "%s\nCheck /switch/gtavcs_nx/gtavcs-boot.log or gamefolder/gtavcs.log.",
             last_message);
    ErrorApplicationConfig config;
    /* M10-09: a failure in play (a game file unreadable) is not a failure to start */
    Result rc = errorApplicationCreate(&config, in_play ? "GTA VCS NX stopped." : "GTA VCS NX failed to start.",
                                       details);
    if (R_SUCCEEDED(rc)) rc = errorApplicationShow(&config);
    fprintf(stderr, "[boot] failure shown in the system error dialog (%08x)\n", rc);
    fflush(stderr);
}

static void fatal_screen(void)
{
    /* Only for failures before SDL started (see error_dialog). Once Mesa EGL has used the default NWindow, the
     * software console cannot take it back: its first framebufferBegin fails to dequeue a buffer
     * (LibnxError_BadGfxDequeueBuffer, 2345-0021) and libnx aborts, even after nwindowReleaseBuffers (M04
     * bad-configuration runs). */
    PrintConsole *screen = consoleInit(NULL);
    char message[768];
    snprintf(message, sizeof(message), "GTA VCS NX failed\n%s\n"
           "Check /switch/gtavcs_nx/gtavcs-boot.log or gamefolder/gtavcs.log.\n"
           "Returning in 15 seconds.\n", last_message);
    /* nxlink may own stdout's file descriptor. Draw via the public console
     * renderer so fatal text is visible regardless of stdio redirection. */
    int x = 0, y = 0;
    if (screen && screen->consoleInitialised) {
        for (const unsigned char *p = (const unsigned char *)message; *p; ++p) {
            if (*p == '\n') { x = 0; ++y; continue; }
            if (x >= screen->consoleWidth) { x = 0; ++y; }
            if (y >= screen->consoleHeight) break;
            if (*p >= screen->font.asciiOffset &&
                *p - screen->font.asciiOffset < screen->font.numChars)
                screen->renderer->drawChar(screen, x, y, *p - screen->font.asciiOffset);
            ++x;
        }
    }
    for (int i = 0; i < 900 && appletMainLoop(); ++i) consoleUpdate(NULL);
    consoleExit(NULL);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int result = EXIT_FAILURE, core_started = 0;
    SDL_Window *window = NULL;
    SDL_GLContext context = NULL;
    FILE *log = NULL;
    int sockets_started = 0;
    AppletHookCookie hook;
    int hook_started = 0;
    int exit_locked = 0;
    int sdl_started = 0;
    if (__nxlink_host.s_addr && R_SUCCEEDED(socketInitializeDefault())) {
        sockets_started = 1;
        nx_socket = nxlinkConnectToHost(true, false);
    }
    log = freopen("sdmc:/switch/gtavcs_nx/gtavcs-boot.log", "w", stderr);
    if (!log) {
        snprintf(last_message, sizeof(last_message), "Cannot open /switch/gtavcs_nx/gtavcs-boot.log");
        goto cleanup;
    }
    setvbuf(log, NULL, _IOLBF, 0);
    diagnostic("[boot] GTA VCS NX; HOS=%08x libnx=%s applet=%d\n",
        hosversionGet(), GTAVCS_LIBNX_PACKAGE, appletGetAppletType());
    if (appletGetAppletType() != AppletType_Application &&
        appletGetAppletType() != AppletType_SystemApplication) {
        diagnostic("[boot] Full application memory required; launch using title override\n");
        goto cleanup;
    }
    /* Without the exit lock Horizon ends the process at once when the player closes the game from HOME, so
     * nothing below runs (first M04 close run). Locked, the close arrives as an applet exit request:
     * appletMainLoop() returns false, the loop stops the core in order, and the lock is released last. */
    if (R_SUCCEEDED(appletLockExit())) exit_locked = 1;
    else diagnostic("[lifecycle] Cannot lock exit: a close from HOME will skip the orderly shutdown\n");

    /* Standard libnx startup mounts sdmc. Do not mount a second copy or change
     * cwd later: workers and all relative asset/mod lookups share this root. */
    if (chdir(ASSET_ROOT) != 0) {
        diagnostic("[boot] chdir(%s): %s\n", ASSET_ROOT, strerror(errno));
        goto cleanup;
    }
    log = freopen("gtavcs.log", "w", stderr);
    if (!log) {
        snprintf(last_message, sizeof(last_message), "Cannot open gamefolder/gtavcs.log");
        goto cleanup;
    }
    setvbuf(log, NULL, _IOLBF, 0);
    diagnostic("[boot] checked chdir succeeded: %s\n", ASSET_ROOT);
    SDL_SetMainReady();
    /* SDL owns audren (opened by the core's platform layer) and the controllers. */
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER | SDL_INIT_TIMER) < 0)
        goto sdl_error;
    sdl_started = 1;
    if (appletGetAppletType() == AppletType_Application) {
        const Result rc = appletSetFocusHandlingMode(AppletFocusHandlingMode_SuspendHomeSleepNotify);
        if (R_FAILED(rc)) {
            diagnostic("[lifecycle] Cannot enable focus notifications: %08x\n", rc);
            goto cleanup;
        }
    }
    diagnostic("[boot] SDL %s; audio %s\n", SDL_GetCurrentVideoDriver(), SDL_GetCurrentAudioDriver());

    if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3) < 0 ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3) < 0 ||
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE) < 0 ||
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1) < 0 ||
        SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24) < 0 ||
        SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8) < 0) goto sdl_error;

    /* Fixed size, not SDL_WINDOW_RESIZABLE: switch-sdl2 2.28 resizes a resizable window at every dock change and
     * each resize leaks the EGL surface's buffers (+44 MB to 1080p; M05 third review). Never call SDL_SetWindowSize:
     * it leaks the same way. The window is the largest output, 1920x1080, in both modes; the picture is drawn at the
     * mode's output size and the layer cropped to it (src/switch/output.c, M06-20). */
    window = SDL_CreateWindow("GTA VCS NX", SDL_WINDOWPOS_UNDEFINED,
        SDL_WINDOWPOS_UNDEFINED, GTAVCS_SURFACE_WIDTH, GTAVCS_SURFACE_HEIGHT,
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);
    if (!window) goto sdl_error;
    context = SDL_GL_CreateContext(window);
    if (!context || SDL_GL_MakeCurrent(window, context) < 0) goto sdl_error;
    if (!gladLoadGL((GLADloadfunc)SDL_GL_GetProcAddress) || !GLAD_GL_VERSION_3_3) {
        diagnostic("[boot] OpenGL 3.3 entry points unavailable\n");
        goto cleanup;
    }
    diagnostic("[boot] GL %s; renderer %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    {
        int surface_w, surface_h;
        SDL_GL_GetDrawableSize(window, &surface_w, &surface_h);
        gtavcs_output_update(surface_w, surface_h); /* before the core: the renderer starts at the output size */
    }
    /* The core paces host frames (platform::waitNextFrame, 60 Hz); the RHI sets the
     * interval from the Display > Vertical sync setting when it presents. */
    if (SDL_GL_SetSwapInterval(0) < 0)
        diagnostic("[boot] swap interval 0 unavailable: %s\n", SDL_GetError());

    /* From here the core owns the SDL event queue, the controllers and audio. */
    const int boot = gtavcs_core_boot(ASSET_ROOT);
    if (boot != 0) {
        diagnostic("[boot] core boot failed (exit code %d): %s\n", boot, gtavcs_core_error());
        goto cleanup;
    }
    core_started = 1;

    int running = 1;
    u64 active_time = 0;
    const int suspension_clock = R_SUCCEEDED(appletGetProgramTotalActiveTime(&active_time));
    u64 suspended_epoch = suspension_clock ? armTicksToNs(armGetSystemTick()) - active_time : 0;
    diagnostic("[lifecycle] suspension_clock=%d\n", suspension_clock);
    int focused = 1, previous_width = 0, previous_height = 0, previous_mode = -1;
    u64 window_frames = 0, window_start = armTicksToNs(armGetSystemTick());
    appletHook(&hook, lifecycle_hook, NULL);
    hook_started = 1;
    unsigned long frames_presented = 0;
    while (running && appletMainLoop()) {
        /* Focus messages can be coalesced while Horizon suspends the process.
         * Compare real ticks to libnx's clock excluding accumulated suspension;
         * an ordinary render stall advances both clocks. */
        if (suspension_clock) {
            if (R_FAILED(appletGetProgramTotalActiveTime(&active_time))) {
                diagnostic("[lifecycle] suspension clock failed\n");
                goto cleanup;
            }
            const u64 epoch = armTicksToNs(armGetSystemTick()) - active_time;
            if (epoch > suspended_epoch && epoch - suspended_epoch > UINT64_C(100000000)) {
                diagnostic("[lifecycle] confirmed suspension_ns=%llu\n",
                    (unsigned long long)(epoch - suspended_epoch));
                resumed = 1;
            }
            suspended_epoch = epoch;
        }
        const int active = appletGetFocusState() == AppletFocusState_InFocus;
        if (active != focused || resumed || focus_notifications) {
            diagnostic("[lifecycle] resume=%d focus_notifications=%u focus=%d\n", resumed, focus_notifications, active);
            resumed = 0;
            focus_notifications = 0;
            /* A suspension seen without a focus change still restarts pacing. */
            if (active && focused) gtavcs_core_set_focus(0);
            focused = active;
            gtavcs_core_set_focus(focused);
        }
        if (!focused) { SDL_Delay(20); continue; }
        int width, height, window_w, window_h;
        SDL_GL_GetDrawableSize(window, &width, &height);
        SDL_GetWindowSize(window, &window_w, &window_h);
        /* Docking (M05-10): every operation-mode or drawable change, with the frames presented and their mean
         * interval since the last one (a dock change that stops presentation shows as a collapsed interval). */
        const int mode = appletGetOperationMode();
        gtavcs_output_update(width, height);
        int output_w, output_h;
        gtavcs_output_size(&output_w, &output_h);
        if (width != previous_width || height != previous_height || mode != previous_mode) {
            const u64 now = armTicksToNs(armGetSystemTick());
            diagnostic("[video] drawable=%dx%d window=%dx%d output=%dx%d mode=%d (was %d); %llu frames, %.2f ms each since the last change; heap %zu\n",
                width, height, window_w, window_h, output_w, output_h, mode, previous_mode, (unsigned long long)window_frames,
                window_frames ? (double)(now - window_start) / 1e6 / (double)window_frames : 0.0,
                (size_t)mallinfo().uordblks);
            previous_width = width; previous_height = height; previous_mode = mode;
            window_frames = 0;
            window_start = now;
        }
        ++window_frames;
        const int state = gtavcs_core_frame();
        if (state < 0) { diagnostic("[core] frame failed: %s\n", gtavcs_core_error()); goto cleanup; }
        if (state == 0) { diagnostic("[core] orderly stop after %lu frames\n", frames_presented); break; }
        /* The OpenGL RHI drew and swapped inside the frame. With the null RHI
         * (VCS_RHI=null) nothing is drawn: present a cleared frame to keep the
         * display owned. */
        if (gtavcs_core_presents()) {
            ++frames_presented;
        } else if (width > 0 && height > 0) {
            glViewport(0, 0, width, height);
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
            SDL_GL_SwapWindow(window);
            ++frames_presented;
        }
    }
    if (running && !appletMainLoop())
        diagnostic("[lifecycle] applet asked the program to exit\n");
    result = EXIT_SUCCESS;
    goto cleanup;

sdl_error:
    diagnostic("[SDL] %s\n", SDL_GetError());
cleanup:;
    char failure[sizeof(last_message)];
    memcpy(failure, last_message, sizeof(failure));
    if (hook_started) appletUnhook(&hook);
    /* Joins every core worker and releases the disc before SDL goes. */
    if (core_started) gtavcs_core_shutdown();
    if (context) SDL_GL_DeleteContext(context);
    if (window) SDL_DestroyWindow(window);
    SDL_Quit(); /* Also unwind a partially successful SDL_Init. */
    if (result == EXIT_SUCCESS)
        diagnostic("[shutdown] core, context, window and SDL closed\n");
    else {
        memcpy(last_message, failure, sizeof(last_message));
        if (sdl_started) error_dialog(frames_presented != 0);
        else fatal_screen();
    }
    if (log) { fflush(log); fclose(log); }
    if (nx_socket >= 0) {
        /* nxlink dup2's the socket onto stdout. libc must close that FILE
         * while the socket device is still registered, before socketExit. */
        fflush(stdout);
        fclose(stdout);
        close(nx_socket);
    }
    if (sockets_started) socketExit();
    if (exit_locked) appletUnlockExit(); /* the system may end the process from here on */
    return result;
}
