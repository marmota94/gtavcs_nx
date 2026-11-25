/* M10-02: a sampling profiler for the process's own threads (diagnosis; nxrun.cfg env only).
 *
 *   VCS_NX_SAMPLE_THREAD=4,6     the threads: N, the Nth created thread (the t<N> of the [threads] lines;
 *                                thread_stack.c); up to 4. NOT the main thread: route-prof-docked (2026-11-25) paused it
 *                                500 times a second and the console went to Atmosphere's fatal screen (the controller
 *                                applet aborted; docs/milestones/M10/hardware/NOTES.md). "main" is refused.
 *   VCS_NX_SAMPLE_HZ=500         samples a second per thread (50-2000)
 *   VCS_NX_SAMPLE_FROM=80        seconds after the sampler starts (core boot) to begin, VCS_NX_SAMPLE_TO=280 to end
 *   VCS_NX_SAMPLE_OUT=path       where the counts go at shutdown (relative to the game folder)
 *
 * Horizon lets a process pause its own threads: svcSetThreadActivity(Paused), then svcGetThreadContext3 reads the
 * registers of the paused thread, svcSetThreadActivity(Runnable) lets it go. The sampler keeps a count per (thread,
 * pc, lr) and writes "count pc lr thread" lines plus the address of a known symbol (gtavcs_sampler_start) so a host
 * tool (tools/switch_m10_session.py profile) can resolve them against the run's ELF. A pause costs the sampled thread a
 * few microseconds; at 500 Hz well under 1% of its time. The upstream profiler (src/debug/profiler.cpp) samples with
 * SuspendThread / SIGPROF, which the Switch has not. */
#include <switch.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Handle gtavcs_thread_handle(unsigned index); /* thread_stack.c */

#define TABLE_BITS 16
#define TABLE_SIZE (1u << TABLE_BITS)
#define MAX_TARGETS 4
#define MAIN_THREAD 0u /* a target index: the main thread */

typedef struct
{
    u64 pc, lr;
    u32 count, thread;
} Slot;

static Slot *table;
static Thread sampler;
static atomic_int stop_flag;
static int running;
static unsigned targets[MAX_TARGETS], target_count, hz;
static double from_s, to_s;
static char out_path[256];
static u64 samples, failed, dropped;

static unsigned env_uint(const char *name, unsigned fallback)
{
    const char *v = getenv(name);
    return v && *v ? (unsigned)strtoul(v, NULL, 10) : fallback;
}

static void count(unsigned thread, u64 pc, u64 lr)
{
    u64 h = (pc * 0x9E3779B97F4A7C15ull) ^ (lr * 0xC2B2AE3D27D4EB4Full) ^ ((u64)thread << 59);
    for (unsigned i = 0; i < 64; ++i)
    {
        Slot *s = &table[(h + i) & (TABLE_SIZE - 1u)];
        if (s->count == 0)
        {
            s->pc = pc;
            s->lr = lr;
            s->thread = thread;
            s->count = 1;
            return;
        }
        if (s->pc == pc && s->lr == lr && s->thread == thread)
        {
            ++s->count;
            return;
        }
    }
    ++dropped;
}

static void sample(unsigned thread)
{
    const Handle h = thread == MAIN_THREAD ? envGetMainThreadHandle() : gtavcs_thread_handle(thread);
    if (h == INVALID_HANDLE) return;
    if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused)))
    {
        ++failed;
        return;
    }
    ThreadContext ctx;
    Result rc = 0;
    /* the pause takes effect when the thread next leaves its core; retry briefly */
    for (int i = 0; i < 50; ++i)
    {
        rc = svcGetThreadContext3(&ctx, h);
        if (R_SUCCEEDED(rc)) break;
        svcSleepThread(10000);
    }
    svcSetThreadActivity(h, ThreadActivity_Runnable);
    if (R_FAILED(rc))
    {
        ++failed;
        return;
    }
    ++samples;
    count(thread, ctx.pc.x, ctx.lr);
}

static void run(void *arg)
{
    (void)arg;
    const u64 start = armGetSystemTick();
    const u64 freq = armGetSystemTickFreq();
    const s64 period_ns = 1000000000ll / hz;
    while (!atomic_load(&stop_flag))
    {
        svcSleepThread(period_ns);
        const double t = (double)(armGetSystemTick() - start) / (double)freq;
        if (t < from_s) continue;
        if (t > to_s) break;
        for (unsigned i = 0; i < target_count; ++i)
            sample(targets[i]);
    }
}

void gtavcs_sampler_start(void)
{
    const char *list = getenv("VCS_NX_SAMPLE_THREAD");
    if (!list || !*list) return;
    char names[64] = "";
    for (const char *p = list; *p && target_count < MAX_TARGETS;)
    {
        unsigned t;
        if (strncmp(p, "main", 4) == 0)
        {
            fprintf(stderr, "[sampler] VCS_NX_SAMPLE_THREAD: the main thread is not sampled (it took the console down); "
                            "nothing sampled\n");
            return;
        }
        else
        {
            char *end = NULL;
            t = (unsigned)strtoul(p, &end, 10);
            if (end == p || !t) return; /* refused: nothing sampled */
            p = end;
        }
        targets[target_count++] = t;
        size_t n = strlen(names);
        if (t == MAIN_THREAD) snprintf(names + n, sizeof(names) - n, "%smain", n ? "," : "");
        else snprintf(names + n, sizeof(names) - n, "%st%u", n ? "," : "", t);
        if (*p == ',') ++p;
        else if (*p) return;
    }
    hz = env_uint("VCS_NX_SAMPLE_HZ", 500);
    if (hz < 50) hz = 50;
    if (hz > 2000) hz = 2000;
    from_s = env_uint("VCS_NX_SAMPLE_FROM", 0);
    to_s = env_uint("VCS_NX_SAMPLE_TO", 100000);
    const char *out = getenv("VCS_NX_SAMPLE_OUT");
    snprintf(out_path, sizeof(out_path), "%s", out && *out ? out : "userdata/samples.txt");
    table = calloc(TABLE_SIZE, sizeof(Slot));
    if (!table) return;
    /* priority above the runtime's threads (libnx's pthreads run at 0x3B), on any of the process's cores */
    if (R_FAILED(threadCreate(&sampler, run, NULL, NULL, 0x10000, 0x20, -2)))
        return;
    u64 mask = 0;
    svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    svcSetThreadCoreMask(sampler.handle, -1, (u32)mask);
    if (R_FAILED(threadStart(&sampler)))
    {
        threadClose(&sampler);
        return;
    }
    running = 1;
    fprintf(stderr, "[sampler] threads %s at %u Hz from %.0f s to %.0f s -> %s\n", names, hz, from_s, to_s, out_path);
}

void gtavcs_sampler_stop(void)
{
    if (!running) return;
    running = 0;
    atomic_store(&stop_flag, 1);
    threadWaitForExit(&sampler);
    threadClose(&sampler);
    FILE *f = fopen(out_path, "w");
    if (f)
    {
        fprintf(f, "# threads %u hz %u from %.0f to %.0f samples %llu failed %llu dropped %llu anchor %llx\n",
            target_count, hz, from_s, to_s, (unsigned long long)samples, (unsigned long long)failed,
            (unsigned long long)dropped, (unsigned long long)(uintptr_t)&gtavcs_sampler_start);
        for (unsigned i = 0; i < TABLE_SIZE; ++i)
            if (table[i].count)
                fprintf(f, "%u %llx %llx %s%u\n", table[i].count, (unsigned long long)table[i].pc,
                    (unsigned long long)table[i].lr, table[i].thread == MAIN_THREAD ? "main" : "t",
                    table[i].thread == MAIN_THREAD ? 0u : table[i].thread);
        fclose(f);
    }
    fprintf(stderr, "[sampler] %llu samples (%llu failed, %llu dropped) -> %s%s\n", (unsigned long long)samples,
        (unsigned long long)failed, (unsigned long long)dropped, out_path, f ? "" : ": cannot write");
    free(table);
    table = NULL;
}
