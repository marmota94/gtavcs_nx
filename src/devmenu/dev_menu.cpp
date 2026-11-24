// See dev_menu.h.
#include "devmenu/dev_menu.h"

#include "launcher.h"
#include "platform/platform.h"
#include "platform/platform_switch.h"
#include "switch/output.h"

#include "game/vcs_trainer.h"
#include "runtime/ps2_audio_out.h"
#include "runtime/ps2_control.h"
#include "runtime/ps2_perf.h"
#include "settings/settings.h"
#include "ui/settings_menu.h"
#include "ui/trainer_menu.h"

#include "imgui.h"

#include <malloc.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <vector>

namespace devmenu
{
    namespace
    {
        bool g_open = false;
        bool g_releaseWait = false; // closed, but the buttons that closed it are still down
        bool g_focus = false;       // focus the window (and its first row) at the next draw
        std::string g_userdata;
        uint64_t g_opens = 0, g_closes = 0;
        uint64_t g_trainerUsedLogged = 0;
        std::string g_lastAction;
        const uint8_t *g_rdram = nullptr; // installCommands
        bool g_gameFreeAim = false;
        uint64_t g_gameFreeAims = 0;

        // the diagnostics, refreshed once a second (mallinfo walks the heap)
        std::chrono::steady_clock::time_point g_nextDiagnostics{};
        std::vector<std::string> g_diagnostics;

        // Dear ImGui's cancel: B (the bottom button) with the Nintendo layout platform_switch_sdl2.cpp sets
        ImGuiKey cancelKey() { return ImGui::GetIO().ConfigNavSwapGamepadButtons ? ImGuiKey_GamepadFaceDown : ImGuiKey_GamepadFaceRight; }

        bool anyPadButtonHeld()
        {
            for (int b = 0; b < platform::GAMEPAD_BUTTON_COUNT; ++b)
                if (platform::gamepadButton(static_cast<platform::GamepadButton>(b)))
                    return true;
            return platform_switch::rawStickButton(0) || platform_switch::rawStickButton(1);
        }

        void log(const char *what)
        {
            const platform_switch::Stats s = platform_switch::stats();
            std::fprintf(stderr, "[devmenu] %s: menu=%d settings=%d trainer=%d chords=%llu\n", what, g_open ? 1 : 0,
                         ui::settingsMenuOpen() ? 1 : 0, ui::trainerMenuOpen() ? 1 : 0, static_cast<unsigned long long>(s.chords));
        }

        void openMenu(const char *why)
        {
            g_open = true;
            g_focus = true;
            ++g_opens;
            ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
            log(why);
        }

        // `other`: the settings menu takes over (it turns Dear ImGui's gamepad navigation on, and off when it closes);
        // the trainer reads the pad itself, so navigation is turned off for it (review 2)
        void closeMenu(const char *why, bool other = false)
        {
            if (!g_open)
                return;
            g_open = false;
            g_releaseWait = true; // the game would otherwise see the button that closed it held
            ++g_closes;
            if (!other)
                ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NavEnableGamepad;
            log(why);
        }

        void act(const std::string &what)
        {
            g_lastAction = what;
            std::fprintf(stderr, "[devmenu] chose: %s\n", what.c_str());
        }

        // A save made after the trainer changed the game can hold what it did: say so in the log and leave a note in
        // the run's userdata, which save tests read (M08-09). Free camera and hidden HUD are not counted (the view only).
        void noteTrainerUse()
        {
            const uint64_t used = vcs::trainer::usedCount();
            if (used == g_trainerUsedLogged)
                return;
            const bool first = g_trainerUsedLogged == 0;
            g_trainerUsedLogged = used;
            std::fprintf(stderr, "[devmenu] trainer used: %llu action(s) this run; a save made from now on may hold trainer "
                                 "changes (noted in %s/trainer-used.txt)\n",
                         static_cast<unsigned long long>(used), g_userdata.c_str());
            if (!first || g_userdata.empty())
                return;
            std::FILE *f = std::fopen((g_userdata + "/trainer-used.txt").c_str(), "a");
            if (!f)
                std::fprintf(stderr, "[devmenu] could not write %s/trainer-used.txt: save tests cannot see this run's trainer use\n",
                             g_userdata.c_str());
            else
            {
                const std::time_t now = std::time(nullptr);
                char when[32] = "";
                std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
                std::fprintf(f, "%s trainer used in this run: saves from then on may hold its changes\n", when);
                std::fclose(f);
            }
        }

        std::string recordingFolder() { return g_userdata + "/perf"; }

        void setRecording(bool on)
        {
            if (on)
                ::mkdir(recordingFolder().c_str(), 0777); // exists already: fine
            ps2perf::setRecording(on, recordingFolder());
        }

        void refreshDiagnostics()
        {
            const auto now = std::chrono::steady_clock::now();
            if (now < g_nextDiagnostics && !g_diagnostics.empty())
                return;
            g_nextDiagnostics = now + std::chrono::seconds(1);
            const platform_switch::Stats s = platform_switch::stats();
            int ow = 0, oh = 0;
            gtavcs_output_size(&ow, &oh);
            const ImGuiIO &io = ImGui::GetIO();
            const struct mallinfo heap = mallinfo();
            char line[256];
            g_diagnostics.clear();
            std::snprintf(line, sizeof(line), "Output %dx%d, menu display %.0fx%.0f, internal scale %dx (restart to change)", ow,
                          oh, io.DisplaySize.x, io.DisplaySize.y, settings::value("display.scale"));
            g_diagnostics.push_back(line);
            std::snprintf(line, sizeof(line), "Controller: %s%s", s.gamepad ? "" : "none ",
                          s.gamepad ? platform::gamepadName() : "(connect one)");
            g_diagnostics.push_back(line);
            std::snprintf(line, sizeof(line), "L3+R3: %llu chord(s); stick presses %llu, given to the game %llu, wait %.0f ms (longest %.1f ms)",
                          static_cast<unsigned long long>(s.chords), static_cast<unsigned long long>(s.stickPresses),
                          static_cast<unsigned long long>(s.stickDelivered), s.chordWindowNs / 1e6, s.maxWithheldNs / 1e6);
            g_diagnostics.push_back(line);
            std::snprintf(line, sizeof(line), "Audio: %.0f ms queued, %llu underruns since start", ps2AudioQueuedMs(),
                          static_cast<unsigned long long>(ps2AudioUnderrunCount()));
            g_diagnostics.push_back(line);
            std::snprintf(line, sizeof(line), "Heap in use %.1f MiB of %.1f MiB obtained", heap.uordblks / 1048576.0,
                          heap.arena / 1048576.0);
            g_diagnostics.push_back(line);
            const uint64_t used = vcs::trainer::usedCount();
            std::snprintf(line, sizeof(line), "Trainer: %s", used ? "USED this run - saves may hold its changes" : "not used");
            g_diagnostics.push_back(line);
            const std::string rec = ps2perf::recordingName();
            g_diagnostics.push_back(rec.empty() ? "Frame-time recording: off" : "Recording: " + rec);
            g_diagnostics.push_back(ps2perf::summaryLine());
        }

        // One row: a full-width button; the first gets the gamepad's focus when the menu opens.
        bool row(const char *label, bool first = false)
        {
            const bool pressed = ImGui::Button(label, ImVec2(-1.0f, 0.0f));
            if (first && g_focus)
            {
                ImGui::SetItemDefaultFocus();
                ImGui::SetKeyboardFocusHere(-1);
            }
            return pressed;
        }

        void drawMenu()
        {
            const ImGuiViewport *vp = ImGui::GetMainViewport();
            const float scale = std::max(1.0f, vp->Size.y / 720.0f); // the same size on the panel at 720p and 1080p
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale * 1.5f);
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + vp->Size.y * 0.5f), ImGuiCond_Always,
                                    ImVec2(0.5f, 0.5f));
            ImGui::SetNextWindowSize(ImVec2(std::min(vp->Size.x * 0.9f, 820.0f * scale), 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.92f);
            if (g_focus)
                ImGui::SetNextWindowFocus();
            const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
            if (ImGui::Begin("Developer menu (L3 + R3)", nullptr, flags))
            {
                const bool freecam = vcs::trainer::toggle(vcs::trainer::FreeCamera);
                const bool overlay = ps2perf::overlayOn();
                const bool recording = ps2perf::recording();
                const bool diagnostics = settings::value("switch.diagnostics") != 0;
                if (row("Settings...", true))
                {
                    act("settings");
                    ui::openSettingsMenu(""); // the page it was on last
                    closeMenu("to the settings menu", true);
                }
                else if (row("Trainer..."))
                {
                    act("trainer");
                    ui::openTrainerMenu();
                    closeMenu("to the trainer");
                }
                else if (row(freecam ? "Free camera: ON (fly: sticks, ZL / ZR, D-pad zoom)" : "Free camera: off"))
                {
                    act(freecam ? "free camera off" : "free camera on");
                    vcs::trainer::setToggle(vcs::trainer::FreeCamera, !freecam);
                    if (!freecam)
                        closeMenu("free camera on: the sticks fly it");
                }
                else if (row(overlay ? "Performance overlay: ON" : "Performance overlay: off"))
                {
                    act(overlay ? "performance overlay off" : "performance overlay on");
                    settings::set("graphics.perf_overlay", overlay ? 0 : 1);
                    ps2perf::setOverlay(!overlay);
                }
                else if (row(recording ? "Frame-time recording: ON (stop and save)" : "Frame-time recording: off (start)"))
                {
                    act(recording ? "recording stop" : "recording start");
                    setRecording(!recording);
                }
                else if (row(diagnostics ? "Diagnostics on screen: ON" : "Diagnostics on screen: off"))
                {
                    act(diagnostics ? "diagnostics off" : "diagnostics on");
                    settings::set("switch.diagnostics", diagnostics ? 0 : 1);
                }
                else if (row("Close"))
                {
                    act("close");
                    closeMenu("closed from its row");
                }
                if (g_open)
                {
                    ImGui::Separator();
                    refreshDiagnostics();
                    for (const std::string &line : g_diagnostics)
                        ImGui::TextWrapped("%s", line.c_str());
                    ImGui::Separator();
                    ImGui::TextDisabled("D-pad / stick: move   A: choose   B or L3 + R3: close");
                }
            }
            ImGui::End();
            ImGui::PopFont();
            g_focus = false;
        }

        // Settings > Advanced > Diagnostics on screen: a small panel at the bottom left while the menu is closed.
        void drawDiagnosticsPanel()
        {
            refreshDiagnostics();
            const ImGuiViewport *vp = ImGui::GetMainViewport();
            const float scale = std::max(1.0f, vp->Size.y / 720.0f);
            ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale);
            ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + 8.0f * scale, vp->Pos.y + vp->Size.y - 8.0f * scale), ImGuiCond_Always,
                                    ImVec2(0.0f, 1.0f));
            ImGui::SetNextWindowBgAlpha(0.6f);
            const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing |
                                           ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize;
            if (ImGui::Begin("##switch-diagnostics", nullptr, flags))
                for (const std::string &line : g_diagnostics)
                    ImGui::TextUnformatted(line.c_str());
            ImGui::End();
            ImGui::PopFont();
        }

        // The game's own free aim (R1 held, then L3; game-internals.md 12c): bit 51 of the player ped's flags at +0xCC8,
        // the word at +0xCCC bit 19 (as src/game/vcs_aim.cpp reads it). Read on the main thread while the game thread
        // runs: a diagnostic, one aligned word, logged on every change.
        void noteGameFreeAim()
        {
            if (!g_rdram)
                return;
            constexpr uint32_t kRamMask = 0x01FFFFFFu, kPlayerPed = 0x004E4910u, kFlags = 0xCCCu, kFreeAimBit = 0x00080000u;
            auto rd32 = [](uint32_t a) {
                uint32_t v = 0;
                std::memcpy(&v, g_rdram + (a & kRamMask & ~3u), 4);
                return v;
            };
            const uint32_t ped = rd32(kPlayerPed);
            const bool on = ped && (rd32(ped + kFlags) & kFreeAimBit);
            if (on == g_gameFreeAim)
                return;
            g_gameFreeAim = on;
            g_gameFreeAims += on ? 1 : 0;
            std::fprintf(stderr, "[devmenu] the game's free aim %s (player ped %08x, flags +0xCCC bit 19)\n", on ? "on" : "off",
                         ped);
        }

        // upstream's UI hook, first each host frame (vcs::launcher::setHostUi): true while the game's input is ours
        bool hostUi()
        {
            noteTrainerUse();
            noteGameFreeAim();
            if (platform_switch::takeMenuChord())
            {
                if (g_open)
                    closeMenu("closed by L3 + R3");
                else if (ui::settingsMenuOpen() || ui::trainerMenuOpen())
                {
                    ui::closeSettingsMenu();
                    ui::closeTrainerMenu();
                    log("settings / trainer closed by L3 + R3");
                }
                else
                    openMenu("opened by L3 + R3");
            }
            else if (g_open && ImGui::IsKeyPressed(cancelKey(), false))
                closeMenu("closed by B");
            if (g_releaseWait && !anyPadButtonHeld())
                g_releaseWait = false;
            if (g_open)
                drawMenu();
            else if (settings::value("switch.diagnostics") != 0)
                drawDiagnosticsPanel();
            return g_open || g_releaseWait;
        }
    }

    void registerSettings()
    {
        using settings::Type;
        settings::Spec s;
        s.key = "switch.diagnostics";
        s.page = "Advanced";
        s.section = "Diagnostics";
        s.label = "Switch diagnostics";
        s.help = "Output size, controller, the L3 + R3 chord's counts and wait, audio queue and underruns, heap, trainer use "
                 "and the frame times in a corner of the screen.";
        s.type = Type::Toggle;
        s.defaultValue = 0;
        settings::add(s);

        s = {};
        s.key = "switch.frame_recording";
        s.page = "Advanced";
        s.section = "Diagnostics";
        s.label = "Frame-time recording";
        s.help = "Starts or stops recording every frame's times to a CSV file in userdata/perf (the developer menu shows the "
                 "file).";
        s.type = Type::Action;
        s.button = "Start / stop";
        s.onChange = [](int) {
            act(ps2perf::recording() ? "recording stop (settings)" : "recording start (settings)");
            setRecording(!ps2perf::recording());
        };
        settings::add(s);

        // nothing on the Switch for these: no keyboard or mouse (the Keyboard page), no desktop window, no other window
        int hidden = 0;
        for (const settings::Spec &each : settings::all())
            if (each.page == "Keyboard" || each.key == "display.window" || each.key == "game.background")
                hidden += settings::setHidden(each.key, true) ? 1 : 0;
        std::fprintf(stderr, "[devmenu] %d desktop-only settings hidden (Keyboard page, window, background)\n", hidden);

        // M09: the Switch's screens are 16:9 (the 1280x720 panel, a 1920x1080 TV): Aspect ratio offers 4:3 (the PS2's
        // picture, pillarboxed) and 16:9 (Hor+), 16:9 by default; the other ratios would only letterbox inside 16:9
        const bool aspect = settings::setChoices(
            "display.aspect", {{"4:3 (original)", 43}, {"16:9", 169}}, 169,
            "16:9 shows more to the sides (Hor+) and fills the screen; 4:3 is the original picture, with black bars at the "
            "sides.");
        std::fprintf(stderr, "[devmenu] aspect ratio choices 4:3 / 16:9, default 16:9: %s\n", aspect ? "set" : "FAILED");
    }

    void install(const std::string &userdata)
    {
        g_userdata = userdata;
        ui::setDesktopGamepadChords(false);
        vcs::launcher::setHostUi(&hostUi);
        std::fprintf(stderr, "[devmenu] L3 + R3 opens the developer menu; Back + Start / RB / LB chords off\n");
    }

    void installCommands(const uint8_t *rdram)
    {
        g_rdram = rdram;
        // devmenu [open|close|settings|trainer] -> ok menu=<0|1> settings=<0|1> trainer=<0|1> blocked=<0|1> chords=<n>
        // opens=<n> closes=<n> trainer_used=<n>
        ps2control::addCommand("devmenu", [](const std::string &args, ps2control::Reply reply) {
            if (args == "open" && !g_open)
                openMenu("opened by the control channel");
            else if (args == "close")
            {
                closeMenu("closed by the control channel");
                ui::closeSettingsMenu();
                ui::closeTrainerMenu();
            }
            else if (args == "settings")
            {
                ui::openSettingsMenu("");
                closeMenu("to the settings menu (control channel)", true);
            }
            else if (args == "trainer")
            {
                ui::openTrainerMenu();
                closeMenu("to the trainer (control channel)");
            }
            else if (!args.empty() && args != "open")
                return reply("error usage: devmenu [open|close|settings|trainer]");
            reply("ok " + reportLine());
        });
    }

    bool open() { return g_open; }

    std::string reportLine()
    {
        const platform_switch::Stats s = platform_switch::stats();
        char line[400];
        std::snprintf(line, sizeof(line),
                      "menu=%d settings=%d trainer=%d blocked=%d chords=%llu opens=%llu closes=%llu stick_presses=%llu "
                      "stick_delivered=%llu max_withheld_ms=%.1f touches=%llu trainer_used=%llu game_free_aim=%d "
                      "game_free_aims=%llu nav=%d last=\"%s\"",
                      g_open ? 1 : 0, ui::settingsMenuOpen() ? 1 : 0, ui::trainerMenuOpen() ? 1 : 0,
                      ps2control::gameInputBlocked() ? 1 : 0, static_cast<unsigned long long>(s.chords),
                      static_cast<unsigned long long>(g_opens), static_cast<unsigned long long>(g_closes),
                      static_cast<unsigned long long>(s.stickPresses), static_cast<unsigned long long>(s.stickDelivered),
                      s.maxWithheldNs / 1e6, static_cast<unsigned long long>(s.touches),
                      static_cast<unsigned long long>(vcs::trainer::usedCount()), g_gameFreeAim ? 1 : 0,
                      static_cast<unsigned long long>(g_gameFreeAims),
                      (ImGui::GetIO().ConfigFlags & ImGuiConfigFlags_NavEnableGamepad) ? 1 : 0, g_lastAction.c_str());
        return line;
    }
}
