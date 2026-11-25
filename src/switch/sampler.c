/* M10-02: a sampling profiler for one of the process's own threads (diagnosis; nxrun.cfg env only).
 *
 *   NX_SAMPLE_THREAD=N       the Nth created thread (the t<N> of the [threads] lines; thread_stack.c)
 *   NX_SAMPLE_HZ=500         samples a second (50-2000)
 *   NX_SAMPLE_FROM=80        seconds after the sampler starts (core boot) to begin, NX_SAMPLE_TO=280 to end
 *   NX_SAMPLE_OUT=path       where the counts go at shutdown (relative to the game folder)
 *
 * Horizon lets a process pause its own threads: svcSetThreadActivity(Paused), then svcGetThreadContext3 reads the
 * registers of the paused thread, svcSetThreadActivity(Runnable) lets it go. The sampler keeps a count per (pc, lr)
 * pair and writes "count pc lr" lines plus the address of a known symbol (gtavcs_sampler_start) so a host tool
 * (tools/switch_m10_session.py profile) can resolve them against the run's ELF. A pause costs the sampled thread a few
 * microseconds; at 500 Hz well under 1% of its time. The upstream profiler (src/debug/profiler.cpp) samples with
 * SuspendThread / SIGPROF, which the Switch has not. */
#include <switch.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

Handle gtavcs_thread_handle(unsigned index); /* thread_stack.c */

#define TABLE_BITS 15
#define TABLE_SIZE (1u << TABLE_BITS)

typedef struct
{
    u64 pc, lr;
    u32 count;
} Slot;

static Slot *table;
static Thread sampler;
static atomic_int stop_flag;
static int running;
static unsigned target, hz;
static double from_s, to_s;
static char out_path[256];
static u64 samples, failed, dropped;

static unsigned env_uint(const char *name, unsigned fallback)
{
    const char *v = getenv(name);
    return v && *v ? (unsigned)strtoul(v, NULL, 10) : fallback;
}

static void count(u64 pc, u64 lr)
{
    u64 h = (pc * 0x9E3779B97F4A7C15ull) ^ (lr * 0xC2B2AE3D27D4EB4Full);
    for (unsigned i = 0; i < 64; ++i)
    {
        Slot *s = &table[(h + i) & (TABLE_SIZE - 1u)];
        if (s->count == 0)
        {
            s->pc = pc;
            s->lr = lr;
            s->count = 1;
            return;
        }
        if (s->pc == pc && s->lr == lr)
        {
            ++s->count;
            return;
        }
    }
    ++dropped;
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
        const Handle h = gtavcs_thread_handle(target);
        if (h == INVALID_HANDLE) continue;
        if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused)))
        {
            ++failed;
            continue;
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
            continue;
        }
        ++samples;
        count(ctx.pc.x, ctx.lr);
    }
}

void gtavcs_sampler_start(void)
{
    target = env_uint("NX_SAMPLE_THREAD", 0);
    if (!target) return;
    hz = env_uint("NX_SAMPLE_HZ", 500);
    if (hz < 50) hz = 50;
    if (hz > 2000) hz = 2000;
    from_s = env_uint("NX_SAMPLE_FROM", 0);
    to_s = env_uint("NX_SAMPLE_TO", 100000);
    const char *out = getenv("NX_SAMPLE_OUT");
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
    fprintf(stderr, "[sampler] thread t%u at %u Hz from %.0f s to %.0f s -> %s\n", target, hz, from_s, to_s, out_path);
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
        fprintf(f, "# thread t%u hz %u from %.0f to %.0f samples %llu failed %llu dropped %llu anchor %llx\n", target, hz,
            from_s, to_s, (unsigned long long)samples, (unsigned long long)failed, (unsigned long long)dropped,
            (unsigned long long)(uintptr_t)&gtavcs_sampler_start);
        for (unsigned i = 0; i < TABLE_SIZE; ++i)
            if (table[i].count)
                fprintf(f, "%u %llx %llx\n", table[i].count, (unsigned long long)table[i].pc,
                    (unsigned long long)table[i].lr);
        fclose(f);
    }
    fprintf(stderr, "[sampler] %llu samples (%llu failed, %llu dropped) -> %s%s\n", (unsigned long long)samples,
        (unsigned long long)failed, (unsigned long long)dropped, out_path, f ? "" : ": cannot write");
    free(table);
    table = NULL;
}
