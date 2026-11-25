// The Switch port's core: upstream's launcher steps (src/launcher.h, the same order as upstream main.cpp) and
// PS2Runtime driven one host frame at a time behind the C ABI in src/core.h.
//
// Differences from upstream main.cpp, all for the Switch target:
//   - No ISO discovery or ELF staging: the disc is the extracted gamefolder (FolderDisc, M03) and the ELF is read
//     from it, its SHA-1 checked here because stageElf (which checked it) is not used.
//   - Settings, cache, both memory cards (with their order files) and card backups live under
//     gamefolder/userdata through GamePaths; a run's own test profile puts them under userdata/tests/<name>.
//   - run() is split (PS2Runtime::runBegin / runFrame / runEnd) so src/main.c keeps the main loop and the applet
//     lifecycle; the runtime's own window/presenter code adopts main.c's window (platform_switch_sdl2.cpp).
//   - std::_Exit is replaced by joined cleanup: the game thread, the render worker, the stream reader, audio and
//     the platform layer stop before the disc is unmapped and destroyed.
//   - The desktop watchdog and sampling profiler (detached threads, Windows/Linux facilities) are not started; the
//     crash handler is upstream's portable part only (terminate / abort messages).
//   - The developer menu (src/devmenu/dev_menu.h, M08) replaces the desktop's F-keys and Back chords: L3 + R3.
#include "core.h"

#include "core/run_config.h"
#include "devmenu/dev_menu.h"
#include "platform/platform_switch.h"
#include "vfs/folder_disc.h"
#include "vfs/game_paths.h"

#include "ps2_runtime.h"
#include "runtime/ee_scheduler.h"
#include "runtime/ps2_control.h"
#include "runtime/ps2_disc_image.h"
#include "runtime/ps2_hle_survey.h"
#include "runtime/ps2_perf.h"

#include "debug/crash.h"
#include "debug/rig.h"
#include "debug/selftest.h"
#include "disc/vcs_disc_id.h"
#include "game/vcs_stream.h"
#include "game/vcs_trainer.h"
#include "launcher.h"
#include "render/presenter.h"
#include "rhi/rhi.h"
#include "settings/settings.h"
#include "util/sha1.h"

#include <malloc.h>
#include <switch.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <thread>
#include <future>
#include <memory>
#include <string>
#include <vector>

// src/switch/thread_stack.c and open_audit.c (linked into the NRO with --wrap)
extern "C" unsigned gtavcs_threads_created(void);
extern "C" size_t gtavcs_default_thread_stack(void);
extern "C" void gtavcs_thread_cpu_line(char *out, size_t size); // M10-02, src/switch/thread_stack.c
extern "C" void gtavcs_clocks_line(char *out, size_t size);     // M10-01, src/switch/clocks.c
extern "C" void gtavcs_sampler_start(void);                     // M10-02, src/switch/sampler.c (VCS_NX_SAMPLE_*)
extern "C" void gtavcs_sampler_stop(void);
extern "C" unsigned gtavcs_open_count(void);
extern "C" unsigned gtavcs_iso_open_count(void);
extern "C" void gtavcs_branch_trace_report(void);
extern "C" const char *gtavcs_first_iso_open(void);

namespace
{
    using Clock = std::chrono::steady_clock;

    struct Pending
    {
        double at;
        std::string line;
        std::future<std::string> reply;
        Clock::time_point sent;
    };

    struct Core
    {
        GamePaths paths;
        RunConfig config;
        std::unique_ptr<FolderDisc> disc;
        std::unique_ptr<PS2Runtime> runtime;
        bool discMapped = false;
        bool running = false;    // runBegin succeeded, runEnd not yet called
        bool focused = true;
        Clock::time_point firstFrame{};
        bool started = false;
        double activeSeconds = 0.0; // frames' time in focus, for the run's schedule
        Clock::time_point lastFrame{};
        double nextReport = 0.0;
        size_t nextCommand = 0;
        std::deque<Pending> pending;
        std::FILE *results = nullptr;
        uint64_t frames = 0;
        std::string error;
    };

    Core *g_core = nullptr;
    std::string g_error;

    void fail(const std::string &message)
    {
        g_error = message;
        std::fprintf(stderr, "[core] %s\n", message.c_str());
    }

    // one machine-readable line per event: <kind> t=<seconds> key=value ... (fields never contain newlines)
    void result(Core &core, const char *kind, const std::string &fields)
    {
        std::fprintf(stderr, "[nxrun] %s t=%.3f %s\n", kind, core.activeSeconds, fields.c_str());
        if (core.results)
        {
            std::fprintf(core.results, "%s t=%.3f %s\n", kind, core.activeSeconds, fields.c_str());
            std::fflush(core.results);
        }
    }

    std::string quote(const std::string &text)
    {
        std::string out = "\"";
        for (char c : text)
        {
            if (c == '"' || c == '\\')
                out += '\\';
            out += (c == '\n' || c == '\r') ? ' ' : c;
        }
        return out + "\"";
    }

    bool readFile(const std::string &path, std::vector<uint8_t> &out)
    {
        std::FILE *f = std::fopen(path.c_str(), "rb");
        if (!f)
            return false;
        out.clear();
        uint8_t buffer[65536];
        size_t n;
        while ((n = std::fread(buffer, 1, sizeof(buffer), f)) > 0)
            out.insert(out.end(), buffer, buffer + n);
        const bool ok = std::ferror(f) == 0;
        std::fclose(f);
        return ok;
    }

    void logResources(const char *stage)
    {
        u64 total = 0, used = 0;
        svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
        svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
        const struct mallinfo m = mallinfo();
        // live threads of this process (the kernel's list): boot and shutdown compare to show no worker outlived the
        // core
        // The kernel refuses the CUR_PROCESS_HANDLE pseudo-handle here (0xE401, invalid handle; first M04 Switch boot),
        // so ask with the real process handle the homebrew loader passes (envGetOwnProcessHandle) first.
        s32 live = -1;
        u64 ids[256];
        const Handle own = envGetOwnProcessHandle();
        const char *via = "own";
        Result listed = own != INVALID_HANDLE ? svcGetThreadList(&live, ids, 256, own) : MAKERESULT(Module_Libnx, LibnxError_NotFound);
        if (R_FAILED(listed))
        {
            via = "pseudo";
            listed = svcGetThreadList(&live, ids, 256, CUR_PROCESS_HANDLE);
        }
        std::fprintf(stderr, "[resources] %s live_threads=%d (svc %08x via %s)\n", stage, R_SUCCEEDED(listed) ? live : -1,
                     listed, via);
        std::fprintf(stderr,
                     "[resources] %s mapped_total=%llu mapped_used=%llu heap_in_use=%zu heap_obtained=%zu "
                     "threads_created=%u default_thread_stack=%zu file_opens=%u iso_opens=%u\n",
                     stage, static_cast<unsigned long long>(total), static_cast<unsigned long long>(used),
                     static_cast<size_t>(m.uordblks), static_cast<size_t>(m.arena), gtavcs_threads_created(),
                     gtavcs_default_thread_stack(), gtavcs_open_count(), gtavcs_iso_open_count());
    }

    void report(Core &core)
    {
        PS2Runtime &rt = *core.runtime;
        const EeKernelSnapshot snap = rt.eeScheduler().snapshot();
        size_t ready = 0, waiting = 0, dormant = 0;
        for (const EeThreadSnapshot &t : snap.threads)
        {
            if (t.status == EeThreadStatus::Running || t.status == EeThreadStatus::Ready)
                ++ready;
            else if (t.status == EeThreadStatus::Dormant)
                ++dormant;
            else
                ++waiting;
        }
        const platform_switch::Stats ps = platform_switch::stats();
        const struct mallinfo heap = mallinfo(); // M06-12: the heap during play, not only at boot and shutdown
        char line[512];
        std::snprintf(line, sizeof(line),
                      "host_frames=%llu game_frames=%llu ee_cycle=%llu ee_threads=%zu ready=%zu waiting=%zu dormant=%zu running=%d "
                      "pc=%08x ra=%08x pumps=%llu audio_callbacks=%llu audio_frames=%llu gamepad=%d heap_in_use=%zu heap_obtained=%zu",
                      static_cast<unsigned long long>(core.frames), static_cast<unsigned long long>(vcs::rig::gameFrames()),
                      static_cast<unsigned long long>(snap.eeCycle), snap.threads.size(), ready, waiting, dormant, snap.runningThreadId,
                      rt.m_debugPc.load(std::memory_order_relaxed), rt.m_debugRa.load(std::memory_order_relaxed),
                      static_cast<unsigned long long>(ps.pumps), static_cast<unsigned long long>(ps.audioCallbacks),
                      static_cast<unsigned long long>(ps.audioFrames), ps.gamepad ? 1 : 0,
                      static_cast<size_t>(heap.uordblks), static_cast<size_t>(heap.arena));
        result(core, "progress", line);
        // M10-02: each thread's share of a core since the last report, its ideal core / affinity mask
        char cpu[768];
        gtavcs_thread_cpu_line(cpu, sizeof(cpu));
        result(core, "threads", cpu);
        char clocks[256];
        gtavcs_clocks_line(clocks, sizeof(clocks));
        result(core, "clocks", clocks);
        result(core, "perf", quote(ps2perf::summaryLine()));
        result(core, "devmenu", devmenu::reportLine());
    }

    // M08-04: what the settings file gave this run: every value that is not the default (the rest are defaults), so a
    // relaunch shows the saved settings came back and a damaged file fell back
    void logSettings(const std::string &path)
    {
        std::string changed;
        int count = 0;
        for (const settings::Spec &s : settings::all())
            if (s.type != settings::Type::Action && settings::value(s.key) != s.defaultValue)
            {
                changed += " " + s.key + "=" + std::to_string(settings::value(s.key));
                ++count;
            }
        std::fprintf(stderr, "[settings] %s: %zu settings, %d not at their default:%s\n", path.c_str(), settings::all().size(),
                     count, count ? changed.c_str() : " (none)");
    }

    // M07-03: deliberate stalls for a run schedule only. `stall main <ms>` blocks the main thread (events, GL, present:
    // the frame) inside its command handler; `stall game <ms>` blocks the game thread (the guest, which produces the
    // sound) from a job run there. The audio consumer (SDL's thread) is never blocked; the ring's underruns and fill
    // (PS2X_AUDIO_STATS) show what each stall costs. Capped at 5 s.
    void registerStall()
    {
        ps2control::addCommand("stall", [](const std::string &args, ps2control::Reply reply) {
            char which[8] = {};
            unsigned ms = 0;
            if (std::sscanf(args.c_str(), "%7s %u", which, &ms) != 2 || ms == 0 || ms > 5000 ||
                (std::strcmp(which, "main") != 0 && std::strcmp(which, "game") != 0))
                return reply("error usage: stall main|game <ms 1-5000>");
            const auto start = Clock::now();
            if (std::strcmp(which, "main") == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                return reply("ok main " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count()));
            }
            vcs::rig::runOnGameThread([ms, start, reply](uint8_t *, R5900Context *, PS2Runtime *) {
                const auto begun = Clock::now();
                std::this_thread::sleep_for(std::chrono::milliseconds(ms));
                reply("ok game " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begun).count()) +
                      " after " + std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(begun - start).count()));
            });
        });
    }

    // control-channel commands of the run: sent when due, answers collected without blocking the frame
    void runCommands(Core &core)
    {
        const std::vector<RunCommand> &commands = core.config.commands;
        while (core.nextCommand < commands.size() && commands[core.nextCommand].at <= core.activeSeconds)
        {
            const RunCommand &c = commands[core.nextCommand++];
            core.pending.push_back({c.at, c.line, ps2control::submit(c.line), Clock::now()});
        }
        for (auto it = core.pending.begin(); it != core.pending.end();)
        {
            if (it->reply.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                const std::string answer = it->reply.get();
                result(core, "command",
                       "at=" + std::to_string(it->at) + " command=" + quote(it->line) +
                           " status=" + (answer.rfind("ok", 0) == 0 ? "ok" : "error") + " reply=" + quote(answer));
                it = core.pending.erase(it);
            }
            else if (Clock::now() - it->sent > std::chrono::seconds(60))
            {
                result(core, "command", "at=" + std::to_string(it->at) + " command=" + quote(it->line) +
                                            " status=timeout reply=\"\"");
                it = core.pending.erase(it);
            }
            else
                ++it;
        }
    }

    // Everything boot started, in reverse; safe at any point of a failed boot and after a run.
    void teardown(Core &core)
    {
        gtavcs_sampler_stop(); // writes the samples before the threads it sampled go
        if (core.runtime)
        {
            core.runtime->requestStop();
            if (core.running)
            {
                // joins the game thread, finishes the render worker, shuts down the presenter and the platform layer
                core.runtime->runEnd();
                core.running = false;
                std::fprintf(stderr, "[core] game thread joined, render worker and platform stopped\n");
                gtavcs_branch_trace_report(); // VCS_NX_BRANCH_TRACE=1 only (src/switch/branch_trace.cpp)
            }
        }
        for (Pending &p : core.pending)
            result(core, "command", "at=" + std::to_string(p.at) + " command=" + quote(p.line) +
                                        " status=unanswered reply=\"\"");
        core.pending.clear();
        // the stream reader copies from the disc into EE RAM: stop it before the runtime (and RAM) or the disc go
        vcs::stopStreamReader();
        if (core.discMapped)
        {
            ps2_disc::setImage(nullptr);
            core.discMapped = false;
        }
        if (core.runtime)
        {
            core.runtime.reset(); // IOP, audio output; the platform layer was shut down by runEnd (or never started)
            std::fprintf(stderr, "[core] runtime destroyed\n");
        }
        if (core.disc)
        {
            core.disc.reset();
            std::fprintf(stderr, "[core] folder disc closed\n");
        }
        // after every worker has stopped: no file can be opened behind this line's back
        result(core, "audit", "file_opens=" + std::to_string(gtavcs_open_count()) +
                                  " iso_opens=" + std::to_string(gtavcs_iso_open_count()) +
                                  " first_iso=" + quote(gtavcs_first_iso_open()) +
                                  " threads_created=" + std::to_string(gtavcs_threads_created()));
        logResources("shutdown");
        if (core.results)
        {
            std::fclose(core.results);
            core.results = nullptr;
        }
    }

    int boot(Core &core, const char *assetRoot)
    {
        logResources("boot");
        // the run description is read from the default userdata before the profile is known
        {
            GamePaths base;
            std::string error;
            if (!resolveGamePaths(assetRoot, base, error))
                return fail(error), GTAVCS_EXIT_ERROR;
            if (!loadRunConfig(base.userdata + "/nxrun.cfg", core.config, error))
                return fail(error), GTAVCS_EXIT_ERROR;
        }
        std::string error;
        if (!resolveGamePaths(assetRoot, core.paths, error, core.config.profile))
            return fail(error), GTAVCS_EXIT_ERROR;
        const GamePaths &p = core.paths;
        std::fprintf(stderr, "[core] root=%s userdata=%s settings=%s mc0=%s mc1=%s backups=%s cache=%s elf=%s%s%s\n",
                     p.root.c_str(), p.userdata.c_str(), p.settings.c_str(), p.mc0.c_str(), p.mc1.c_str(),
                     p.backups.c_str(), p.cache.c_str(), p.elf.c_str(), core.config.profile.empty() ? "" : " profile=",
                     core.config.profile.c_str());
        if (core.config.present)
        {
            core.results = std::fopen((p.userdata + "/nxrun-results.txt").c_str(), "w");
            if (!core.results)
                return fail("cannot write " + p.userdata + "/nxrun-results.txt"), GTAVCS_EXIT_ERROR;
            result(core, "config", "profile=" + quote(core.config.profile) + " seconds=" + std::to_string(core.config.seconds) +
                                       " commands=" + std::to_string(core.config.commands.size()) +
                                       " env=" + std::to_string(core.config.env.size()) +
                                       " verify_files=" + (core.config.verifyFiles ? "1" : "0"));
            registerStall();
        }
        for (const auto &[name, value] : core.config.env)
        {
            setenv(name.c_str(), value.c_str(), 1);
            std::fprintf(stderr, "[core] env %s=%s\n", name.c_str(), value.c_str());
        }
        gtavcs_sampler_start(); // VCS_NX_SAMPLE_THREAD set only: a run's sampling profile of one thread
        // vcs_hle_world.cpp chooses the world path in a static initializer, before this run's environment exists: on
        // the desktop PS2X_HLE comes from the process environment, here from nxrun.cfg (M06-01, reference-handheld
        // drew natively with PS2X_HLE=0). Apply the reference path's choice again: no MSCAL hooks = the recompiled
        // VU1 programs draw the world. PS2X_HLE_VERIFY's hooks are private to that file: not selectable here.
        if (const char *hle = std::getenv("PS2X_HLE"); hle && *hle == '0' && !std::getenv("PS2X_HLE_VERIFY"))
        {
            ps2hle::setMscalHooks(nullptr, nullptr);
            std::fprintf(stderr, "[core] PS2X_HLE=0: the world through the VU1 programs (reference path)\n");
        }
        else if (std::getenv("PS2X_HLE_VERIFY"))
            std::fprintf(stderr, "[core] PS2X_HLE_VERIFY from nxrun.cfg is only partly applied: the RSL world hooks stay "
                                 "off, the native MSCAL hooks chosen at process start stay on (no verification)\n");
        // upstream main: the hardware GS renderer unless PS2X_GS says otherwise; it draws through the OpenGL RHI
        // (VCS_RHI=null keeps the null backend for CPU-side bring-up)
        if (const char *gs = std::getenv("PS2X_GS"); !gs || !*gs)
            setenv("PS2X_GS", "hw", 1);
        // M10-03: the game's trails drawn as recorded (its own GS draws), bloom and frame copies native. Upstream's native
        // trails (gs_hw.cpp nativeTrails: its buffer widened at 16:9) cost ~16 ms of GPU time a present at 16:9 on the
        // Switch's Mesa nouveau: docked 11 -> 21 game fps standing at Fort Baxter with them recorded, the picture alike
        // (docs/milestones/M10/hardware/NOTES.md, postfx-*-docked). At 4:3 both are the same draws. A run's
        // PS2X_HW_POSTFX still wins.
        if (const char *post = std::getenv("PS2X_HW_POSTFX"); !post || !*post)
            setenv("PS2X_HW_POSTFX", "bc", 1);
        std::fprintf(stderr, "[core] post effects native: %s (PS2X_HW_POSTFX; trails recorded by default, M10-03)\n",
                     std::getenv("PS2X_HW_POSTFX"));

        vcs::launcher::registerSettings();
        devmenu::registerSettings();
        settings::load(p.settings);
        if (ps2control::hiddenMode())
            settings::keepFile();
        settings::applyAll();
        logSettings(p.settings);
        vcs::launcher::restoreWindowPlacement();

        // the ELF from the extracted tree, checked as stageElf checked the disc's copy
        std::vector<uint8_t> elf;
        if (!readFile(p.elf, elf))
            return fail("cannot read " + p.elf + ": extract the disc into the game folder (tools/extract_disc.py)"),
                   GTAVCS_EXIT_WRONG_DISC;
        const std::string sha1 = vcs::sha1Hex(elf.data(), elf.size());
        if (sha1 != vcs::disc::kElfSha1)
            return fail(p.elf + " has SHA-1 " + sha1 + ", not " + vcs::disc::kElfSha1 + " (" + vcs::disc::kTitle + ")"),
                   GTAVCS_EXIT_WRONG_DISC;
        std::fprintf(stderr, "[core] %s sha1 %s: %s\n", p.elf.c_str(), sha1.c_str(), vcs::disc::kTitle);
        elf.clear();
        elf.shrink_to_fit();

        core.disc = FolderDisc::load(p.root, error);
        if (!core.disc)
            return fail("folder disc: " + error), GTAVCS_EXIT_DISC_UNREADABLE;
        for (const std::string &warning : core.disc->warnings())
            std::fprintf(stderr, "[core] folder disc: %s\n", warning.c_str());
        std::fprintf(stderr, "[core] folder disc %u sectors, %zu mods applied; no disc image opened\n",
                     core.disc->totalSectors(), core.disc->modsApplied());
        if (core.config.verifyFiles)
        {
            const auto start = Clock::now();
            const std::vector<std::string> bad = core.disc->verifyFiles();
            for (const std::string &b : bad)
                result(core, "verify", "problem=" + quote(b));
            result(core, "verify", "problems=" + std::to_string(bad.size()) + " seconds=" +
                                       std::to_string(std::chrono::duration<double>(Clock::now() - start).count()));
            if (!bad.empty())
                return fail(std::to_string(bad.size()) + " disc files differ from the manifest (first: " + bad.front() + ")"),
                       GTAVCS_EXIT_DISC_UNREADABLE;
        }
        ps2_disc::setImage(core.disc.get());
        core.discMapped = true;

        // the trainer's mission list names each mission from MAIN.SCM (game/vcs_trainer.h)
        {
            uint32_t lsn = 0, size = 0;
            bool directory = false;
            if (core.disc->findFile("MAIN.SCM", lsn, size, directory) && !directory)
            {
                std::vector<uint8_t> bytes(size);
                if (core.disc->readBytes(static_cast<uint64_t>(lsn) * FolderDisc::kSectorSize, size, bytes.data()))
                    vcs::trainer::setMissionScript(bytes);
            }
        }

        // the card in our data folder from the start (upstream main: before loadELF set the paths)
        {
            PS2Runtime::IoPaths early = PS2Runtime::getIoPaths();
            early.mcRoot = p.mc0;
            PS2Runtime::setIoPaths(early);
        }

        core.runtime = std::make_unique<PS2Runtime>();
        vcs::launcher::setScreenshotDirectory(p.userdata + "/screenshots");
        devmenu::install(p.userdata);
        vcs::launcher::setUiCallbacks(*core.runtime);
        if (!core.runtime->initialize("GTA: Vice City Stories"))
            return fail("runtime initialize failed (platform, presenter or audio)"), GTAVCS_EXIT_INIT_FAILED;
        if (!core.runtime->loadELF(p.elf))
            return fail("loadELF failed: " + p.elf), GTAVCS_EXIT_ERROR;
        // loadELF points every root at the ELF's folder (the game folder: cdRoot / hostRoot are the disc tree); saves
        // go to userdata, and there is no disc image path - cdrom0: is the mapped FolderDisc
        PS2Runtime::IoPaths io = PS2Runtime::getIoPaths();
        io.cdImage.clear();
        io.mcRoot = p.mc0;
        PS2Runtime::setIoPaths(io);
        const PS2Runtime::IoPaths &set = PS2Runtime::getIoPaths();
        std::fprintf(stderr, "[core] io elf=%s host=%s cd=%s mc=%s cdImage=%s\n", set.elfPath.string().c_str(),
                     set.hostRoot.string().c_str(), set.cdRoot.string().c_str(), set.mcRoot.string().c_str(),
                     set.cdImage.empty() ? "(none)" : set.cdImage.string().c_str());

        vcs::launcher::installHooks(*core.runtime);
        devmenu::installCommands(core.runtime->memory().getRDRAM());
        vcs::runSelfTest(*core.runtime);
        std::fprintf(stderr, "[core] desktop watchdog and sampling profiler not started (detached desktop threads; "
                             "progress comes from the run's report lines)\n");
        if (!core.runtime->runBegin())
            return fail("runBegin failed"), GTAVCS_EXIT_INIT_FAILED;
        core.running = true;
        logResources("running");
        return 0;
    }
}

extern "C" int gtavcs_core_boot(const char *asset_root)
{
    g_error.clear();
    if (g_core)
        return fail("core booted twice"), GTAVCS_EXIT_ERROR;
    vcs::installCrashHandlers(); // portable part: std::terminate / abort messages
    g_core = new Core();
    int code = GTAVCS_EXIT_ERROR;
    try
    {
        code = boot(*g_core, asset_root ? asset_root : "");
    }
    catch (const std::exception &e)
    {
        fail(std::string("boot exception: ") + e.what());
    }
    catch (...)
    {
        fail("boot exception (unknown)");
    }
    if (code != 0)
    {
        try
        {
            teardown(*g_core);
        }
        catch (...)
        {
            std::fprintf(stderr, "[core] exception while unwinding a failed boot\n");
        }
        delete g_core;
        g_core = nullptr;
    }
    return code;
}

extern "C" int gtavcs_core_frame(void)
{
    if (!g_core || !g_core->running)
        return fail("frame without a running core"), -1;
    Core &core = *g_core;
    try
    {
        const auto now = Clock::now();
        if (!core.started)
        {
            core.started = true;
            core.firstFrame = core.lastFrame = now;
        }
        // M10-09: a game file the folder disc could not read after its retries: stop with it named (the system's error
        // dialog shows it) instead of the game waiting on a black screen for data that never comes
        {
            std::string unreadable;
            if (folderDiscUnreadable(unreadable))
                return fail("cannot read " + unreadable + " (the SD card or the extracted game files): stopped"), -1;
        }
        // the schedule counts time in focus only (HOME and sleep pause it)
        core.activeSeconds += std::min(0.25, std::chrono::duration<double>(now - core.lastFrame).count());
        core.lastFrame = now;
        if (!core.runtime->runFrame())
        {
            result(core, "stop", "reason=runtime frames=" + std::to_string(core.frames));
            return 0;
        }
        ++core.frames;
        runCommands(core);
        if (core.activeSeconds >= core.nextReport)
        {
            report(core);
            core.nextReport = core.activeSeconds + core.config.report;
        }
        if (core.config.seconds > 0.0 && core.activeSeconds >= core.config.seconds)
        {
            report(core);
            result(core, "stop", "reason=seconds frames=" + std::to_string(core.frames));
            return 0;
        }
        return 1;
    }
    catch (const std::exception &e)
    {
        fail(std::string("frame exception: ") + e.what());
    }
    catch (...)
    {
        fail("frame exception (unknown)");
    }
    return -1;
}

extern "C" void gtavcs_core_set_focus(int focused)
{
    platform_switch::setFocus(focused != 0);
    if (!g_core)
        return;
    g_core->focused = focused != 0;
    g_core->lastFrame = Clock::now(); // the suspension does not count as frame time
    result(*g_core, "focus", focused ? "state=in" : "state=out");
}

extern "C" void gtavcs_core_shutdown(void)
{
    if (!g_core)
        return;
    try
    {
        result(*g_core, "shutdown", "frames=" + std::to_string(g_core->frames));
        teardown(*g_core);
    }
    catch (const std::exception &e)
    {
        std::fprintf(stderr, "[core] shutdown exception: %s\n", e.what());
    }
    catch (...)
    {
        std::fprintf(stderr, "[core] shutdown exception (unknown)\n");
    }
    delete g_core;
    g_core = nullptr;
}

extern "C" const char *gtavcs_core_error(void) { return g_error.c_str(); }

extern "C" int gtavcs_core_presents(void)
{
    rhi::Device *device = present::device();
    return device && device->backend() == rhi::Backend::OpenGL ? 1 : 0;
}
