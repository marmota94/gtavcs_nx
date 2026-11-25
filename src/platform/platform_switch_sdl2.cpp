// SDL2 implementation of upstream's platform.h for the Nintendo Switch (libnx + switch-sdl2 2.28).
//
// Translated from upstream's SDL3 platform_sdl3.cpp at the pinned revision. Differences, all deliberate:
//   - No window is created here. src/main.c owns SDL_Init, the one SDL_WINDOW_OPENGL window and its GL context;
//     init() adopts that window (SDL_GL_GetCurrentWindow) and refuses to start without it. shutdown() releases what
//     init() made (Dear ImGui, the gamepad, audio) and leaves the window, the context and SDL_Quit to main.c.
//   - The desktop window placement, full screen, mouse capture and cursor hiding have no Switch counterpart: they
//     are accepted and remembered so the settings registry keeps working, and logged once.
//   - Vulkan entry points report unavailable (the Switch baseline is OpenGL through SDL2/EGL; no Vulkan driver).
//   - The OpenGL renderer (rhi::GlWindow, M05) gets main.c's context through SDL: its entry points, the swap, the
//     swap interval and, as the drawable size, the operation mode's output region of the fixed 1920x1080 surface
//     (src/switch/output.h, M06-20). Nothing here makes a context or surface.
//   - SDL2 game controllers: buttons by physical position (SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS=0), so
//     south = Cross on every controller (platform.h's contract). Whether a controller is connected comes from HID's
//     npad style sets, not SDL: switch-sdl2 never removes a joystick (M06-14).
//   - L3 and R3 reach the game only through the developer menu's chord filter (src/devmenu/chord.h, M08-14): each press
//     waits VCS_NX_CHORD_MS (100 ms) for the other one; L3 + R3 together toggle the menu and never reach the game.
//   - Touch: switch-sdl2 delivered no touch on hardware, so the panel is read from HID (pollTouch) and fed to Dear
//     ImGui, scaled to its display, the output region the panel shows (1280x720 in handheld, M08-17).
//     Mouse events (a USB mouse) are scaled from the window's coordinates, the fixed 1920x1080 surface, the same way.
//   - Audio: SDL2's callback device (SDL owns audren), pulled from upstream's producer ring by AudioFill on SDL's
//     audio thread. setFocus(false) pauses and resumes it across HOME / sleep.
// Thread rules are platform.h's: the main thread pumps events and owns the frame calls; queries read a snapshot.
#include "platform/platform.h"
#include "platform/platform_switch.h"
#include "platform/gl_loader_egl.h"
#include "devmenu/chord.h"
#include "switch/output.h"

#include <SDL.h>
#include <switch.h>

#include "imgui.h"
#include "imgui_impl_sdl2.h"
#include <zlib.h>

#include <algorithm>
#include <array>
#include <memory>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace platform
{
    namespace
    {
        SDL_Window *g_window = nullptr; // main.c's; never destroyed here
        bool g_quit = false;
        bool g_imgui = false;
        uint64_t g_nextFrameNs = 0;
        std::atomic<uint64_t> g_framePeriodNs{1000000000ull / 60u}; // setFrameRate

        std::array<std::atomic<bool>, KEY_COUNT> g_keys{};
        std::array<bool, KEY_COUNT> g_keysPrev{}; // main thread
        std::array<bool, KEY_COUNT> g_keysPressed{};
        std::array<bool, KEY_COUNT> g_edgesPending{};
        std::array<std::atomic<bool>, GAMEPAD_BUTTON_COUNT> g_padButtons{};
        std::array<std::atomic<float>, AXIS_COUNT> g_padAxes{};
        std::atomic<bool> g_padConnected{false};
        SDL_GameController *g_gamepad = nullptr;
        devmenu::ChordFilter g_chord; // M08-14: L3 / R3 to the game (made again at init with the run's window)
        bool g_rawSticks[2] = {};     // L3, R3 before the filter
        bool g_menuChord = false;     // takeMenuChord
        uint64_t g_initNs = 0;        // the chord log's clock
        std::atomic<uint64_t> g_chords{0}, g_stickPresses{0}, g_stickDelivered{0}, g_maxWithheldNs{0}, g_touches{0};

        SDL_AudioDeviceID g_audio = 0;
        AudioFill g_audioFill = nullptr;
        std::atomic<bool> g_audioCloseHung{false}; // M10-10: SDL_CloseAudioDevice did not return (audioClose)
        std::atomic<bool> g_audioWanted{false}; // open and not paused for focus
        std::atomic<uint64_t> g_audioCallbacks{0}, g_audioFrames{0};

        bool g_fullscreen = false;
        bool g_hiddenWindow = false;
        bool g_focused = true;
        std::atomic<int64_t> g_snapshotNs{0};
        uint64_t g_leadNs = 0;
        FrameLead g_atLead = nullptr;
        bool g_leadRan = false;
        bool g_slept = false;
        std::atomic<uint32_t> g_rumbleWanted{0}; // low << 16 | high
        uint32_t g_rumbleSent = 0;
        uint64_t g_rumbleSentNs = 0;
        std::atomic<uint64_t> g_pumps{0};

        uint64_t nowNs()
        {
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        }

        void sleepNs(uint64_t ns)
        {
            // SDL2 has no SDL_DelayPrecise: sleep most of it, then yield to the deadline (libnx sleeps in ticks)
            const uint64_t deadline = nowNs() + ns;
            if (ns > 2000000ull)
                std::this_thread::sleep_for(std::chrono::nanoseconds(ns - 1000000ull));
            while (nowNs() < deadline)
                std::this_thread::yield();
        }

        void unsupportedOnce(const char *what)
        {
            static std::mutex mutex;
            static std::vector<const char *> said;
            std::lock_guard<std::mutex> lock(mutex);
            if (std::find(said.begin(), said.end(), what) != said.end())
                return;
            said.push_back(what);
            std::fprintf(stderr, "[platform] %s: not on the Switch (accepted, no effect)\n", what);
        }

        // The window's back buffer as the renderer sees it: the operation mode's output region of the fixed surface
        // (src/switch/output.c, M06-20), or the whole drawable before main.c set one.
        void outputSize(int &width, int &height)
        {
            width = height = 0;
            if (!g_window)
                return;
            gtavcs_output_size(&width, &height);
            if (width <= 0 || height <= 0)
                SDL_GL_GetDrawableSize(g_window, &width, &height);
        }

        void openGamepad(int index)
        {
            if (g_gamepad || !SDL_IsGameController(index))
                return;
            g_gamepad = SDL_GameControllerOpen(index);
            if (!g_gamepad)
            {
                std::fprintf(stderr, "[platform] gamepad %d: %s\n", index, SDL_GetError());
                return;
            }
            char *mapping = SDL_GameControllerMapping(g_gamepad);
            std::fprintf(stderr, "[platform] gamepad: %s (instance %d) mapping %s\n", SDL_GameControllerName(g_gamepad),
                         SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_gamepad)), mapping ? mapping : "(none)");
            SDL_free(mapping);
        }

        void openFirstGamepad()
        {
            for (int i = 0; !g_gamepad && i < SDL_NumJoysticks(); ++i)
                openGamepad(i);
        }

        // Physical connection (M06-14). switch-sdl2 2.28's joystick driver keeps a fixed set of joysticks and never
        // removes one, so SDL_GameControllerGetAttached stays 1 after the Joy-Cons disconnect (M01's connected=1
        // observation). SDL's first joystick reads player 1, which is npad No1 or the handheld rail, so HID's style
        // sets of those two are the truth: no style means nothing is connected for player 1.
        uint32_t g_npadStyles = ~0u; // No1 | Handheld << 16 as last logged; ~0: not read yet
        bool g_inputLog = false;     // VCS_NX_INPUT_LOG=1: every controller button / axis change to the log

        const char *orUnknown(const char *name) { return name ? name : "?"; }

        const char *styleName(uint32_t styles)
        {
            if (styles & HidNpadStyleTag_NpadFullKey)
                return "Pro Controller (full key)";
            if (styles & HidNpadStyleTag_NpadHandheld)
                return "Joy-Cons attached (handheld)";
            if (styles & HidNpadStyleTag_NpadJoyDual)
                return "paired Joy-Cons (dual)";
            if (styles & (HidNpadStyleTag_NpadJoyLeft | HidNpadStyleTag_NpadJoyRight))
                return "single Joy-Con";
            return styles ? "other" : "none";
        }

        bool playerOneConnected()
        {
            const uint32_t no1 = hidGetNpadStyleSet(HidNpadIdType_No1), handheld = hidGetNpadStyleSet(HidNpadIdType_Handheld);
            const uint32_t now = (no1 & 0xFFFFu) | (handheld & 0xFFFFu) << 16;
            if (now != g_npadStyles)
            {
                std::fprintf(stderr,
                             "[platform] controller physical: No1 styles 0x%x (%s, device type 0x%x), handheld styles 0x%x (%s, "
                             "device type 0x%x)%s\n",
                             no1, styleName(no1), no1 ? hidGetNpadDeviceType(HidNpadIdType_No1) : 0u, handheld,
                             styleName(handheld), handheld ? hidGetNpadDeviceType(HidNpadIdType_Handheld) : 0u,
                             now ? "" : ": nothing connected for player 1, input neutral");
                g_npadStyles = now;
            }
            return now != 0;
        }

        // M08-14: L3 / R3 through the chord filter, in place. Every event is logged (stick clicks are rare): the log
        // shows what the game was given for each press, and that a chord gave it nothing.
        void filterSticks(bool &l3, bool &r3, bool live)
        {
            using devmenu::ChordFilter;
            g_rawSticks[0] = l3;
            g_rawSticks[1] = r3;
            if (!live)
            {
                // out of focus or no pad: nothing withheld comes out later, and what is still held when the pad is live
                // again counts only once let go (review 1: a chord held across HOME toggled the menu)
                g_chord.reset();
                l3 = r3 = false;
                return;
            }
            const uint64_t now = nowNs();
            const ChordFilter::Result r = g_chord.update(l3, r3, now);
            l3 = r.guest[ChordFilter::L3];
            r3 = r.guest[ChordFilter::R3];
            if (r.chord)
            {
                g_menuChord = true;
                g_chords.fetch_add(1, std::memory_order_relaxed);
            }
            static const char *const kNames[] = {"L3", "R3"};
            for (int b = 0; b < ChordFilter::kButtons; ++b)
            {
                if (r.pressed[b])
                    g_stickPresses.fetch_add(1, std::memory_order_relaxed);
                const ChordFilter::Event e = r.event[b];
                if (e == ChordFilter::Event::None)
                    continue;
                if (e == ChordFilter::Event::Delivered || e == ChordFilter::Event::TapDelivered)
                {
                    g_stickDelivered.fetch_add(1, std::memory_order_relaxed);
                    for (uint64_t seen = g_maxWithheldNs.load(std::memory_order_relaxed);
                         r.heldNs[b] > seen && !g_maxWithheldNs.compare_exchange_weak(seen, r.heldNs[b], std::memory_order_relaxed);)
                    {
                    }
                }
                char held[48] = "";
                if (e == ChordFilter::Event::Delivered || e == ChordFilter::Event::TapDelivered || e == ChordFilter::Event::Chord)
                    std::snprintf(held, sizeof(held), " after %.1f ms", r.heldNs[b] / 1e6);
                std::fprintf(stderr, "[chord] %.3f s %s %s%s: game sees L3=%d R3=%d%s\n", (now - g_initNs) / 1e9, kNames[b],
                             devmenu::eventName(e), held, l3 ? 1 : 0, r3 ? 1 : 0,
                             e == ChordFilter::Event::Chord && b == ChordFilter::R3 ? " (menu toggle)" : "");
            }
        }

        // Dear ImGui's display is the output region the panel shows (switch/output.h). A USB mouse (the touch screen comes
        // through pollTouch: switch-sdl2 delivers none of it): this ASSUMES switch-sdl2 reports the pointer scaled from
        // the screen to the window's 1920x1080, so it is scaled back to the display. If it reports surface pixels instead,
        // the right mapping is the crop's offset without scaling. Unverified: no mouse was used (M16-04).
        void toUiCoordinates(SDL_Event &e)
        {
            int ww = 0, wh = 0, ow = 0, oh = 0;
            SDL_GetWindowSize(g_window, &ww, &wh);
            outputSize(ow, oh);
            if (ww <= 0 || wh <= 0 || ow <= 0 || oh <= 0)
                return;
            auto sx = [&](Sint32 v) { return static_cast<Sint32>(std::lround(static_cast<double>(v) * ow / ww)); };
            auto sy = [&](Sint32 v) { return static_cast<Sint32>(std::lround(static_cast<double>(v) * oh / wh)); };
            if (e.type == SDL_MOUSEMOTION)
            {
                e.motion.x = sx(e.motion.x);
                e.motion.y = sy(e.motion.y);
                e.motion.xrel = sx(e.motion.xrel);
                e.motion.yrel = sy(e.motion.yrel);
            }
            else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP)
            {
                const Sint32 x = e.button.x, y = e.button.y;
                e.button.x = sx(x);
                e.button.y = sy(y);
                if (e.type == SDL_MOUSEBUTTONDOWN)
                {
                    g_touches.fetch_add(1, std::memory_order_relaxed);
                    std::fprintf(stderr, "[touch] %s press at window %d,%d -> display %d,%d (window %dx%d, display %dx%d)\n",
                                 e.button.which == SDL_TOUCH_MOUSEID ? "touch" : "mouse", x, y, e.button.x, e.button.y, ww,
                                 wh, ow, oh);
                }
            }
        }

        // The touch screen (M08-17), read from HID once per pump. switch-sdl2 2.28 delivered no touch to the game on
        // hardware: neither mouse events (its fingers have no window) nor SDL_FINGER* events (the second touch run had
        // none in its log). HID gives the panel's 1280x720 points; the panel shows the output region, Dear ImGui's display,
        // so they scale to it. The first finger only, fed as a touch-screen mouse; nothing while out of focus.
        void pollTouch()
        {
            static bool initialised = false, down = false;
            if (!initialised)
            {
                hidInitializeTouchScreen();
                initialised = true;
            }
            HidTouchScreenState state{};
            const bool touching = g_focused && hidGetTouchScreenStates(&state, 1) == 1 && state.count > 0;
            if (!touching && !down)
                return;
            ImGuiIO &io = ImGui::GetIO();
            io.AddMouseSourceEvent(ImGuiMouseSource_TouchScreen);
            if (!touching)
            {
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                down = false;
                return;
            }
            int ow = 0, oh = 0;
            outputSize(ow, oh);
            const float px = std::clamp(static_cast<float>(state.touches[0].x), 0.0f, 1280.0f);
            const float py = std::clamp(static_cast<float>(state.touches[0].y), 0.0f, 720.0f);
            const float x = px / 1280.0f * static_cast<float>(ow), y = py / 720.0f * static_cast<float>(oh);
            io.AddMousePosEvent(x, y);
            if (!down)
            {
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                down = true;
                g_touches.fetch_add(1, std::memory_order_relaxed);
                std::fprintf(stderr, "[touch] finger down at panel %.0f,%.0f -> display %.0f,%.0f (display %dx%d)\n", px, py, x, y,
                             ow, oh);
            }
        }

        void snapshotGamepad()
        {
            static constexpr SDL_GameControllerButton kButtons[GAMEPAD_BUTTON_COUNT] = {
                SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
                SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_LEFTSTICK,
                SDL_CONTROLLER_BUTTON_RIGHTSTICK, SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
                SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
                SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
            };
            static constexpr SDL_GameControllerAxis kAxes[AXIS_COUNT] = {
                SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY, SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY,
                SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
            };
            // out of focus (HOME, sleep) or without a pad: neutral, so nothing stays held
            const bool physical = playerOneConnected();
            const bool live = g_gamepad && physical && g_focused && SDL_GameControllerGetAttached(g_gamepad);
            g_padConnected.store(g_gamepad != nullptr && physical, std::memory_order_relaxed);
            bool down[GAMEPAD_BUTTON_COUNT];
            for (int b = 0; b < GAMEPAD_BUTTON_COUNT; ++b)
                down[b] = live && SDL_GameControllerGetButton(g_gamepad, kButtons[b]);
            filterSticks(down[GAMEPAD_LEFT_STICK], down[GAMEPAD_RIGHT_STICK], live);
            for (int b = 0; b < GAMEPAD_BUTTON_COUNT; ++b)
                g_padButtons[b].store(down[b], std::memory_order_relaxed);
            for (int a = 0; a < AXIS_COUNT; ++a)
            {
                // SDL2 axes are -32768..32767 (triggers 0..32767); platform.h: sticks -1..1, triggers 0..1
                const float v = live ? SDL_GameControllerGetAxis(g_gamepad, kAxes[a]) / 32767.0f : 0.0f;
                g_padAxes[a].store(std::clamp(v, -1.0f, 1.0f), std::memory_order_relaxed);
            }
        }

        void SDLCALL audioCallback(void *, Uint8 *stream, int bytes)
        {
            const uint32_t frames = static_cast<uint32_t>(bytes) / 4u;
            AudioFill fill = g_audioFill;
            if (!frames)
                return;
            if (!fill || !g_audioWanted.load(std::memory_order_acquire))
            {
                std::memset(stream, 0, static_cast<size_t>(bytes));
                return;
            }
            fill(reinterpret_cast<int16_t *>(stream), frames); // interleaved S16 stereo, the format opened below
            g_audioCallbacks.fetch_add(1, std::memory_order_relaxed);
            g_audioFrames.fetch_add(frames, std::memory_order_relaxed);
        }

        uint32_t pngCrc32(uint32_t crc, const uint8_t *p, size_t n)
        {
            static const auto table = [] {
                std::array<uint32_t, 256> t{};
                for (uint32_t i = 0; i < 256; ++i)
                {
                    uint32_t c = i;
                    for (int k = 0; k < 8; ++k)
                        c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                    t[i] = c;
                }
                return t;
            }();
            crc = ~crc;
            while (n--)
                crc = table[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
            return ~crc;
        }

        void put32(std::vector<uint8_t> &v, uint32_t x)
        {
            v.push_back(static_cast<uint8_t>(x >> 24));
            v.push_back(static_cast<uint8_t>(x >> 16));
            v.push_back(static_cast<uint8_t>(x >> 8));
            v.push_back(static_cast<uint8_t>(x));
        }

        void chunk(std::vector<uint8_t> &out, const char *type, const std::vector<uint8_t> &data)
        {
            put32(out, static_cast<uint32_t>(data.size()));
            const size_t start = out.size();
            out.insert(out.end(), type, type + 4);
            out.insert(out.end(), data.begin(), data.end());
            put32(out, pngCrc32(0, out.data() + start, out.size() - start));
        }
    }

    void setStartPlacement(const WindowPlacement &) { unsupportedOnce("window placement"); }
    void onPlacementChanged(PlacementChanged) {}

    void setFullscreen(bool on)
    {
        // the Switch output is always the whole screen; remember the setting's value for the menu
        if (on != g_fullscreen)
            unsupportedOnce("full screen switch");
        g_fullscreen = on;
    }

    bool fullscreen() { return g_fullscreen; }

    bool init(const WindowDesc &desc)
    {
        if (g_window)
            return true;
        g_window = SDL_GL_GetCurrentWindow();
        if (!g_window || !SDL_GL_GetCurrentContext())
        {
            // main.c makes the window and context first: a second window would be a second EGL surface
            std::fprintf(stderr, "[platform] no current SDL GL window: main.c must create it before the runtime\n");
            g_window = nullptr;
            return false;
        }
        if ((SDL_WasInit(SDL_INIT_GAMECONTROLLER) & SDL_INIT_GAMECONTROLLER) == 0)
        {
            std::fprintf(stderr, "[platform] SDL game controller subsystem not initialised by main.c\n");
            g_window = nullptr;
            return false;
        }
        g_hiddenWindow = desc.hidden;
        g_quit = false;
        g_nextFrameNs = 0;
        SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0"); // positions: south = Cross (platform.h)
        if (const char *e = std::getenv("VCS_NX_INPUT_LOG"); e && *e && *e != '0')
            g_inputLog = true;
        g_npadStyles = ~0u;
        openFirstGamepad();
        // M08-14: the chord window, from the run's environment (nxrun.cfg) when given
        {
            uint64_t ms = 100;
            if (const char *e = std::getenv("VCS_NX_CHORD_MS"); e && *e)
                ms = std::clamp<uint64_t>(std::strtoull(e, nullptr, 10), 20, 500);
            g_chord = devmenu::ChordFilter(ms * 1000000ull);
            g_initNs = nowNs();
            g_menuChord = false;
            std::fprintf(stderr, "[chord] L3 + R3 opens the developer menu; every L3 / R3 press waits %llu ms for the other\n",
                         static_cast<unsigned long long>(ms));
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        // gamepad navigation stays off until a menu opens (the developer and settings menus turn it on and off; on from
        // here it stayed on through gameplay: M08 review 2)
        // M08: the menus use the Nintendo layout, A (the right button) chooses and B (the bottom one) goes back (the
        // user's report: the PS2 positions felt swapped in the menus); the game keeps the physical positions
        io.ConfigNavSwapGamepadButtons = true;
        ImGui::StyleColorsDark();
        g_imgui = ImGui_ImplSDL2_InitForOpenGL(g_window, SDL_GL_GetCurrentContext());
        // M05-09's second threading prototype: a loader context for the OpenGL RHI's free-threaded textures
        if (const char *e = std::getenv("PS2X_GL_WORKER_CONTEXT"); e && *e && *e != '0')
            gl_loader::create();
        if (!g_imgui)
            std::fprintf(stderr, "[platform] Dear ImGui SDL2 input backend failed\n");
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(g_window, &w, &h);
        std::fprintf(stderr, "[platform] SDL2 %d.%d.%d adopted main.c's window %dx%d (asked %dx%d%s); ImGui %s\n",
                     SDL_MAJOR_VERSION, SDL_MINOR_VERSION, SDL_PATCHLEVEL, w, h, desc.width, desc.height,
                     desc.hidden ? ", hidden" : "", g_imgui ? "on" : "off");
        if (desc.vulkan)
            std::fprintf(stderr, "[platform] Vulkan window asked for: no Vulkan on the Switch baseline\n");
        return true;
    }

    void shutdown()
    {
        if (!g_window)
            return;
        audioClose();
        if (g_imgui)
        {
            ImGui_ImplSDL2_Shutdown();
            g_imgui = false;
        }
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        if (g_gamepad)
        {
            SDL_GameControllerRumble(g_gamepad, 0, 0, 0);
            SDL_GameControllerClose(g_gamepad);
        }
        g_gamepad = nullptr;
        g_padConnected.store(false, std::memory_order_relaxed);
        gl_loader::destroy(); // the presenter's device (its only user) is gone by now
        g_window = nullptr; // main.c destroys it, after the core has shut down
        std::fprintf(stderr, "[platform] shut down (window, context and SDL left to main.c)\n");
    }

    bool ready() { return g_window != nullptr; }
    void *nativeWindow() { return nullptr; }
    bool windowHidden() { return g_hiddenWindow; }

    const char *const *vulkanInstanceExtensions(uint32_t *count)
    {
        if (count)
            *count = 0;
        unsupportedOnce("Vulkan");
        return nullptr;
    }

    bool vulkanCreateSurface(void *, uint64_t *)
    {
        unsupportedOnce("Vulkan");
        return false;
    }

    // the OpenGL renderer: main.c's one SDL GL context, current on the main thread (rhi::GlWindow)
    void *glGetProcAddress(const char *name) { return SDL_GL_GetProcAddress(name); }

    void glSwapWindow()
    {
        if (g_window)
            SDL_GL_SwapWindow(g_window);
    }

    void glDrawableSize(uint32_t *width, uint32_t *height)
    {
        int w = 0, h = 0;
        outputSize(w, h);
        *width = static_cast<uint32_t>(std::max(w, 0));
        *height = static_cast<uint32_t>(std::max(h, 0));
    }

    bool glSetSwapInterval(int interval) { return SDL_GL_SetSwapInterval(interval) == 0; }
    bool glLoaderAvailable() { return gl_loader::available(); }
    bool glMakeLoaderCurrent(bool current) { return gl_loader::makeCurrent(current); }

    namespace
    {
        void noteEdge(int k, bool edge, bool frameStart)
        {
            if (!frameStart)
            {
                g_edgesPending[k] = g_edgesPending[k] || edge;
                return;
            }
            g_keysPressed[k] = edge || g_edgesPending[k];
            g_edgesPending[k] = false;
        }
    }

    void pumpEvents(bool frameStart)
    {
        SDL_Event e;
        while (SDL_PollEvent(&e))
        {
            if (g_imgui)
            {
                SDL_Event ui = e;
                toUiCoordinates(ui);
                ImGui_ImplSDL2_ProcessEvent(&ui);
            }
            switch (e.type)
            {
            case SDL_QUIT:
                // switch-sdl2 sends SDL_QUIT when appletMainLoop() reports the applet's exit request (the player
                // closed the game from HOME; main.c holds the exit lock so it arrives here)
                if (!g_quit)
                    std::fprintf(stderr, "[platform] quit requested (applet exit request): stopping in order\n");
                g_quit = true;
                break;
            case SDL_CONTROLLERDEVICEADDED:
                openGamepad(e.cdevice.which);
                break;
            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:
                if (g_inputLog)
                    std::fprintf(stderr, "[input] %u ms instance %d button %s %s\n", e.cbutton.timestamp, e.cbutton.which,
                                 orUnknown(SDL_GameControllerGetStringForButton(static_cast<SDL_GameControllerButton>(e.cbutton.button))),
                                 e.type == SDL_CONTROLLERBUTTONDOWN ? "down" : "up");
                break;
            case SDL_CONTROLLERAXISMOTION:
                if (g_inputLog)
                {
                    // only crossings of half travel and the return to the centre: enough to show direction and sign
                    static int8_t zone[SDL_CONTROLLER_AXIS_MAX] = {};
                    const int8_t z = e.caxis.value > 16384 ? 1 : e.caxis.value < -16384 ? -1 : 0;
                    if (e.caxis.axis < SDL_CONTROLLER_AXIS_MAX && z != zone[e.caxis.axis])
                    {
                        zone[e.caxis.axis] = z;
                        std::fprintf(stderr, "[input] %u ms instance %d axis %s %d\n", e.caxis.timestamp, e.caxis.which,
                                     orUnknown(SDL_GameControllerGetStringForAxis(static_cast<SDL_GameControllerAxis>(e.caxis.axis))),
                                     e.caxis.value);
                    }
                }
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (g_gamepad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(g_gamepad)) == e.cdevice.which)
                {
                    std::fprintf(stderr, "[platform] gamepad removed (instance %d): neutral until another opens\n",
                                 e.cdevice.which);
                    SDL_GameControllerClose(g_gamepad);
                    g_gamepad = nullptr;
                    openFirstGamepad();
                }
                break;
            default:
                break;
            }
        }
        // no keyboard or mouse on the Switch: keys stay up (the control channel presses keys through ps2control)
        for (int k = 0; k < KEY_COUNT; ++k)
        {
            noteEdge(k, false, frameStart);
            g_keysPrev[k] = false;
            g_keys[k].store(false, std::memory_order_relaxed);
        }
        snapshotGamepad();
        if (g_imgui)
            pollTouch();

        const uint64_t now = nowNs();
        const uint32_t rumble = g_focused ? g_rumbleWanted.load(std::memory_order_relaxed) : 0u;
        if (g_gamepad && (rumble != g_rumbleSent || (rumble && now - g_rumbleSentNs > 500000000ull)))
        {
            SDL_GameControllerRumble(g_gamepad, static_cast<Uint16>(rumble >> 16), static_cast<Uint16>(rumble & 0xFFFF),
                                     rumble ? 1000 : 0);
            g_rumbleSent = rumble;
            g_rumbleSentNs = now;
        }
        g_pumps.fetch_add(1, std::memory_order_relaxed);
        g_snapshotNs.store(static_cast<int64_t>(now), std::memory_order_release);
    }

    int64_t inputSnapshotNs() { return g_snapshotNs.load(std::memory_order_acquire); }
    bool quitRequested() { return g_quit; }

    void beginFrame()
    {
        if (!g_imgui)
            return;
        ImGui_ImplSDL2_NewFrame();
        // the backend measures the whole window; Dear ImGui draws on its output region (switch/output.h)
        int w = 0, h = 0;
        outputSize(w, h);
        ImGuiIO &io = ImGui::GetIO();
        io.DisplaySize = ImVec2(static_cast<float>(w), static_cast<float>(h));
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        ImGui::NewFrame();
    }

    void endUiFrame()
    {
        if (g_imgui)
            ImGui::Render();
    }

    void setFrameRate(double hz)
    {
        g_framePeriodNs.store(static_cast<uint64_t>(1e9 / std::clamp(hz, 24.0, 1000.0)), std::memory_order_relaxed);
    }

    double displayRefreshHz()
    {
        SDL_DisplayMode mode{};
        const int display = g_window ? SDL_GetWindowDisplayIndex(g_window) : 0;
        return SDL_GetCurrentDisplayMode(display < 0 ? 0 : display, &mode) == 0 && mode.refresh_rate > 1 ? mode.refresh_rate : 60.0;
    }

    void delayPrecise(uint64_t ns) { sleepNs(ns); }

    void waitNextFrame()
    {
        const uint64_t kPeriodNs = g_framePeriodNs.load(std::memory_order_relaxed);
        const uint64_t now = nowNs();
        g_nextFrameNs = g_nextFrameNs ? g_nextFrameNs + kPeriodNs : now + kPeriodNs;
        g_leadRan = false;
        g_slept = false;
        if (g_nextFrameNs < now)
        {
            g_nextFrameNs = now; // fell behind: don't try to catch up
            return;
        }
        g_slept = true;
        const uint64_t lead = std::min(g_leadNs, kPeriodNs);
        if (g_atLead && lead && g_nextFrameNs - lead > now)
        {
            sleepNs(g_nextFrameNs - lead - now);
            g_atLead();
            g_leadRan = true;
            const uint64_t after = nowNs();
            if (g_nextFrameNs > after)
                sleepNs(g_nextFrameNs - after);
            return;
        }
        sleepNs(g_nextFrameNs - now);
    }

    void setFrameLead(uint64_t leadNs, FrameLead atLead)
    {
        g_leadNs = leadNs;
        g_atLead = atLead;
    }

    bool frameLeadRan() { return g_leadRan; }
    bool frameSlept() { return g_slept; }

    void windowSize(int &width, int &height) { outputSize(width, height); }

    bool windowFocused() { return g_window && !g_hiddenWindow && g_focused; }

    bool keyDown(int key) { return key >= 0 && key < KEY_COUNT && g_keys[key].load(std::memory_order_relaxed); }
    bool keyPressed(int key) { return key >= 0 && key < KEY_COUNT && g_keysPressed[key]; }
    bool mouseDown(MouseButton) { return false; }
    bool mousePosition(float &x, float &y)
    {
        x = y = 0.0f;
        return false;
    }
    bool gamepadConnected() { return g_padConnected.load(std::memory_order_relaxed); }
    bool gamepadButton(GamepadButton button) { return g_padButtons[button].load(std::memory_order_relaxed); }
    float gamepadAxis(GamepadAxis axis) { return g_padAxes[axis].load(std::memory_order_relaxed); }

    const char *gamepadName()
    {
        const char *name = g_gamepad ? SDL_GameControllerName(g_gamepad) : nullptr;
        return name ? name : "";
    }

    void gamepadRumble(float low, float high)
    {
        auto level = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 65535.0f + 0.5f); };
        g_rumbleWanted.store(level(low) << 16 | level(high), std::memory_order_relaxed);
    }

    void setMouseCaptured(bool on)
    {
        if (on)
            unsupportedOnce("mouse capture");
    }
    bool mouseCaptured() { return false; }
    void takeMouseMotion(float &dx, float &dy) { dx = dy = 0.0f; }

    const char *keyName(int key)
    {
        static thread_local char buffer[16];
        if ((key >= KEY_A && key <= KEY_Z) || (key >= KEY_ZERO && key <= KEY_NINE))
            return (buffer[0] = static_cast<char>(key), buffer[1] = 0, buffer);
        if (key >= KEY_F1 && key <= KEY_F12)
            return (std::snprintf(buffer, sizeof(buffer), "F%d", key - KEY_F1 + 1), buffer);
        switch (key)
        {
        case KEY_ESCAPE: return "Escape";
        case KEY_ENTER: return "Enter";
        case KEY_SPACE: return "Space";
        default: return "?";
        }
    }

    bool audioAvailable() { return (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) != 0 && SDL_GetNumAudioDevices(0) != 0; }

    bool audioOpen(uint32_t rate, uint32_t bufferFrames, AudioFill fill)
    {
        if (g_audio || (SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) == 0)
            return false;
        SDL_AudioSpec want{}, have{};
        want.freq = static_cast<int>(rate);
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = static_cast<Uint16>(std::clamp<uint32_t>(bufferFrames, 256u, 4096u));
        want.callback = audioCallback;
        g_audioFill = fill;
        // no allowed changes: SDL converts if audren wants another format, the callback always sees S16 stereo
        g_audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (!g_audio)
        {
            std::fprintf(stderr, "[platform] audio device: %s\n", SDL_GetError());
            g_audioFill = nullptr;
            return false;
        }
        std::fprintf(stderr, "[platform] audio device %u: %d Hz, %u channels, %u frames (%s)\n", g_audio, have.freq,
                     have.channels, have.samples, SDL_GetCurrentAudioDriver());
        g_audioWanted.store(true, std::memory_order_release);
        SDL_PauseAudioDevice(g_audio, g_focused ? 0 : 1);
        return true;
    }

    void audioClose()
    {
        if (!g_audio)
            return;
        g_audioWanted.store(false, std::memory_order_release);
        std::fprintf(stderr, "[platform] closing the audio device\n");
        // M10-10: switch-sdl2's close sometimes never returned (fault-texture-docked-2: the log ends at the line above, the
        // game frozen at exit). The device is paused first, then closed on a helper thread with a 1 s limit; if SDL does
        // not return, the exit goes on without it (main.c then skips SDL_Quit, which would close the device again and
        // hang) and the process's end stops SDL's audio thread.
        SDL_PauseAudioDevice(g_audio, 1);
        auto closed = std::make_shared<std::atomic<bool>>(false);
        const SDL_AudioDeviceID device = g_audio;
        std::thread closer([device, closed] {
            SDL_CloseAudioDevice(device); // waits for a callback in progress: AudioFill is not called after this
            closed->store(true, std::memory_order_release);
        });
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!closed->load(std::memory_order_acquire) && std::chrono::steady_clock::now() < limit)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        if (closed->load(std::memory_order_acquire))
            closer.join();
        else
        {
            closer.detach();
            g_audioCloseHung.store(true, std::memory_order_release);
            std::fprintf(stderr, "[platform] the audio device did not close within 1 s: left to the process's end\n");
        }
        std::fprintf(stderr, "[platform] audio closed after %llu callbacks, %llu frames\n",
                     static_cast<unsigned long long>(g_audioCallbacks.load()), static_cast<unsigned long long>(g_audioFrames.load()));
        g_audio = 0;
        g_audioFill = nullptr;
    }

    bool writePng(const char *path, const uint8_t *rgb, int width, int height)
    {
        // upstream's PNG writer: per-row filter by smallest signed sum, then zlib
        const size_t stride = static_cast<size_t>(width) * 3u;
        std::vector<uint8_t> raw(static_cast<size_t>(height) * (stride + 1u));
        std::vector<uint8_t> trial(stride);
        for (int y = 0; y < height; ++y)
        {
            const uint8_t *row = rgb + static_cast<size_t>(y) * stride;
            const uint8_t *above = y ? row - stride : nullptr;
            uint8_t *out = raw.data() + static_cast<size_t>(y) * (stride + 1u);
            uint64_t best = ~0ull;
            for (uint8_t filter = 0; filter < 5; ++filter)
            {
                if (filter == 3)
                    continue;
                uint64_t cost = 0;
                for (size_t i = 0; i < stride; ++i)
                {
                    const int a = i >= 3 ? row[i - 3] : 0, b = above ? above[i] : 0, c = (i >= 3 && above) ? above[i - 3] : 0;
                    int predicted = 0;
                    if (filter == 1)
                        predicted = a;
                    else if (filter == 2)
                        predicted = b;
                    else if (filter == 4)
                    {
                        const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                        predicted = (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
                    }
                    trial[i] = static_cast<uint8_t>(row[i] - predicted);
                    cost += static_cast<uint64_t>(std::abs(static_cast<int8_t>(trial[i])));
                }
                if (cost < best)
                {
                    best = cost;
                    out[0] = filter;
                    std::memcpy(out + 1, trial.data(), stride);
                }
            }
        }
        uLongf zSize = compressBound(static_cast<uLong>(raw.size()));
        std::vector<uint8_t> z(zSize);
        if (compress2(z.data(), &zSize, raw.data(), static_cast<uLong>(raw.size()), 6) != Z_OK)
            return false;
        z.resize(zSize);
        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        std::vector<uint8_t> ihdr;
        put32(ihdr, static_cast<uint32_t>(width));
        put32(ihdr, static_cast<uint32_t>(height));
        ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
        chunk(png, "IHDR", ihdr);
        chunk(png, "IDAT", z);
        chunk(png, "IEND", {});
        FILE *f = std::fopen(path, "wb");
        if (!f)
            return false;
        const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
        return std::fclose(f) == 0 && ok;
    }
}

namespace platform_switch
{
    void setFocus(bool focused)
    {
        using namespace platform;
        if (focused == g_focused)
            return;
        g_focused = focused;
        if (g_audio)
        {
            // HOME / sleep: the callback device stops pulling; the producer ring drops what it cannot hold (upstream
            // counts it) and nothing stale is queued on the SDL side (a callback device has no queue)
            SDL_PauseAudioDevice(g_audio, focused ? 0 : 1);
        }
        g_nextFrameNs = 0; // no catch-up burst after a suspension
        std::fprintf(stderr, "[platform] focus %s: audio %s, pad %s\n", focused ? "back" : "lost",
                     focused ? "resumed" : "paused", focused ? "live" : "neutral");
    }

    Stats stats()
    {
        using namespace platform;
        Stats s;
        s.pumps = g_pumps.load(std::memory_order_relaxed);
        s.audioCallbacks = g_audioCallbacks.load(std::memory_order_relaxed);
        s.audioFrames = g_audioFrames.load(std::memory_order_relaxed);
        s.gamepad = g_padConnected.load(std::memory_order_relaxed);
        s.chords = g_chords.load(std::memory_order_relaxed);
        s.stickPresses = g_stickPresses.load(std::memory_order_relaxed);
        s.stickDelivered = g_stickDelivered.load(std::memory_order_relaxed);
        s.maxWithheldNs = g_maxWithheldNs.load(std::memory_order_relaxed);
        s.chordWindowNs = g_chord.windowNs();
        s.touches = g_touches.load(std::memory_order_relaxed);
        return s;
    }

    bool takeMenuChord()
    {
        using namespace platform;
        const bool chord = g_menuChord;
        g_menuChord = false;
        return chord;
    }

    bool rawStickButton(int which)
    {
        using namespace platform;
        return which >= 0 && which < 2 && g_rawSticks[which];
    }
}

// M10-10: main.c skips SDL_Quit when the audio device could not be closed (it would close it again and hang)
extern "C" int gtavcs_audio_close_hung(void) { return platform::g_audioCloseHung.load(std::memory_order_acquire) ? 1 : 0; }
