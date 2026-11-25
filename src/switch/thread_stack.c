/* Default stack for threads created without an explicit size (std::thread, pthread_create
 * with default attributes). libnx's __syscall_thread_create picks 128 KiB for those; the
 * runtime's game thread runs the recompiled EE code and its host helpers on such a
 * std::thread, which on the desktop reference gets 8 MiB (Linux) or 1 MiB (Windows).
 * Linked with -Wl,--wrap=__syscall_thread_create so every default-sized thread in the
 * process gets the Linux reference size; explicit sizes pass through unchanged.
 *
 * M10-02: the wrap also keeps each created thread's handle (libnx's struct __pthread_t starts
 * with its Thread, whose first member is the kernel handle) so the core can report each
 * thread's CPU time and core mask (gtavcs_thread_cpu_line). libnx creates every such thread
 * with the process's default core as its ideal core and the process's core mask as its
 * affinity: where it runs is the kernel's choice. M10-03: NX_THREAD_CORES (nxrun.cfg env, a
 * diagnosis) sets the ideal core of created thread N: "1:1,2:2" puts the 1st created thread
 * on core 1, the 2nd on core 2 (the affinity mask stays the process's). */
#include <switch.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GTAVCS_DEFAULT_THREAD_STACK (8u * 1024u * 1024u)
#define GTAVCS_TRACKED_THREADS 32

struct __pthread_t;
int __real___syscall_thread_create(struct __pthread_t **thread, void *(*func)(void *), void *arg,
    void *stack_addr, size_t stack_size);

static atomic_uint created_threads;
static Handle tracked[GTAVCS_TRACKED_THREADS];
static u64 last_ticks[GTAVCS_TRACKED_THREADS + 1];
static u64 last_wall;

/* NX_THREAD_CORES: the ideal core asked for created thread `index` (1-based), or -1 */
static int wanted_core(unsigned index)
{
    const char *spec = getenv("NX_THREAD_CORES");
    while (spec && *spec)
    {
        char *end = NULL;
        const unsigned long n = strtoul(spec, &end, 10);
        if (end == spec || *end != ':') return -1;
        const long core = strtol(end + 1, &end, 10);
        if (n == index) return core >= 0 && core <= 2 ? (int)core : -1;
        spec = *end == ',' ? end + 1 : NULL;
    }
    return -1;
}

int __wrap___syscall_thread_create(struct __pthread_t **thread, void *(*func)(void *), void *arg,
    void *stack_addr, size_t stack_size)
{
    if (!stack_addr && !stack_size) stack_size = GTAVCS_DEFAULT_THREAD_STACK;
    const int result = __real___syscall_thread_create(thread, func, arg, stack_addr, stack_size);
    if (result) return result;
    const unsigned index = atomic_fetch_add(&created_threads, 1) + 1;
    const Handle handle = *(const Handle *)*thread;
    if (index <= GTAVCS_TRACKED_THREADS) tracked[index - 1] = handle;
    const int core = wanted_core(index);
    u64 mask = 0;
    svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0);
    const Result rc = core >= 0 ? svcSetThreadCoreMask(handle, core, (u32)mask) : 0;
    s32 ideal = -1;
    u64 affinity = 0;
    svcGetThreadCoreMask(&ideal, &affinity, handle);
    /* the creation order names the threads in the [threads] lines (t1, t2, ...); the log's neighbouring lines say
     * which runtime thread each is */
    fprintf(stderr, "[threads] created t%u: ideal core %d, mask %x%s (%08x)\n", index, (int)ideal, (unsigned)affinity,
        core >= 0 ? " (NX_THREAD_CORES)" : "", rc);
    return result;
}

unsigned gtavcs_threads_created(void) { return atomic_load(&created_threads); }
size_t gtavcs_default_thread_stack(void) { return GTAVCS_DEFAULT_THREAD_STACK; }

/* One line: for the main thread (the caller) and each created thread, its share of one core since the last call
 * (CPU ticks / wall ticks, %) and its ideal core / affinity mask; then the sum (300% = the three application cores
 * busy). Main thread only. */
void gtavcs_thread_cpu_line(char *out, size_t size)
{
    const u64 wall = armGetSystemTick();
    const u64 span = last_wall ? wall - last_wall : 0;
    last_wall = wall;
    size_t used = 0;
    unsigned total_pct = 0;
    const unsigned count = atomic_load(&created_threads);
    for (unsigned i = 0; i <= count && i <= GTAVCS_TRACKED_THREADS && used + 48 < size; ++i)
    {
        const Handle h = i == 0 ? CUR_THREAD_HANDLE : tracked[i - 1];
        u64 ticks = 0;
        if (R_FAILED(svcGetInfo(&ticks, InfoType_ThreadTickCount, h, (u64)-1))) continue; /* exited */
        s32 ideal = -1;
        u64 mask = 0;
        svcGetThreadCoreMask(&ideal, &mask, h);
        const unsigned pct = span && last_ticks[i] ? (unsigned)((ticks - last_ticks[i]) * 100 / span) : 0;
        last_ticks[i] = ticks;
        total_pct += pct;
        /* created threads are named by their creation index: t1, t2, ... */
        char label[8];
        if (i == 0) snprintf(label, sizeof(label), "main");
        else snprintf(label, sizeof(label), "t%u", i);
        used += (size_t)snprintf(out + used, size - used, "%s%s=%u%%@%d/%x", used ? " " : "", label, pct, (int)ideal,
            (unsigned)mask);
    }
    if (used + 16 < size) snprintf(out + used, size - used, " sum=%u%%", total_pct);
}
