/* Every file open in the process passes through newlib's _open_r (fopen, freopen, open,
 * and libstdc++'s file streams on top of them). Linked with -Wl,--wrap=_open_r, this
 * counts the opens and records any whose path names a disc image (".iso", any case), so a
 * run can show on hardware that the folder disc replaced the ISO entirely (M04-13). */
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <reent.h>

int __real__open_r(struct _reent *reent, const char *path, int flags, int mode);

static atomic_uint total_opens, iso_opens;
static char first_iso[256];

static int names_iso(const char *path)
{
    const size_t n = path ? strlen(path) : 0;
    for (size_t i = 0; i + 4 <= n; ++i)
        if (path[i] == '.' && (path[i + 1] | 0x20) == 'i' && (path[i + 2] | 0x20) == 's' && (path[i + 3] | 0x20) == 'o' &&
            (i + 4 == n || path[i + 4] == ';' || path[i + 4] == '/'))
            return 1;
    return 0;
}

int __wrap__open_r(struct _reent *reent, const char *path, int flags, int mode)
{
    atomic_fetch_add(&total_opens, 1);
    if (names_iso(path) && atomic_fetch_add(&iso_opens, 1) == 0)
        snprintf(first_iso, sizeof(first_iso), "%s", path);
    const int fd = __real__open_r(reent, path, flags, mode);
    extern int gtavcs_track_card_open(const char *path, int flags, int fd); /* M10-09, below */
    return gtavcs_track_card_open(path, flags, fd);
}

unsigned gtavcs_open_count(void) { return atomic_load(&total_opens); }
unsigned gtavcs_iso_open_count(void) { return atomic_load(&iso_opens); }
const char *gtavcs_first_iso_open(void) { return first_iso; }

/* M10-09: an interrupted save. VCS_NX_FAULT_SAVE_BYTES=N (nxrun.cfg env): once N bytes have been written to files
 * under a memory card folder (".../mc0/" or ".../mc1/") opened for writing, the write that crosses N writes only up
 * to N and the process ends at once (svcExitProcess: no flush, no close, no destructors) - a power cut in the middle
 * of a save. The next start shows what the card and the game make of it. Linked with -Wl,--wrap=_write_r,_close_r. */
#include <switch.h>
#include <fcntl.h>
#include <stdlib.h>

int __real__write_r(struct _reent *reent, int fd, const void *buf, size_t count);
int __real__close_r(struct _reent *reent, int fd);

#define TRACKED_FDS 64
static char card_fd[TRACKED_FDS];
static char card_path[TRACKED_FDS][160];
static long long card_written;

static long long fault_limit(void)
{
    static long long limit = -2;
    if (limit == -2)
    {
        const char *v = getenv("VCS_NX_FAULT_SAVE_BYTES");
        limit = v && *v ? atoll(v) : -1;
    }
    return limit;
}

static int names_card(const char *path)
{
    return path && (strstr(path, "/mc0/") || strstr(path, "/mc1/"));
}

int gtavcs_track_card_open(const char *path, int flags, int fd)
{
    if (fd >= 0 && fd < TRACKED_FDS)
    {
        card_fd[fd] = (char)(names_card(path) && (flags & (O_WRONLY | O_RDWR)) && fault_limit() >= 0);
        if (card_fd[fd]) snprintf(card_path[fd], sizeof(card_path[fd]), "%s", path);
    }
    return fd;
}

int __wrap__write_r(struct _reent *reent, int fd, const void *buf, size_t count)
{
    if (fd >= 0 && fd < TRACKED_FDS && card_fd[fd])
    {
        const long long limit = fault_limit();
        if (card_written + (long long)count >= limit)
        {
            const size_t part = (size_t)(limit - card_written);
            if (part) __real__write_r(reent, fd, buf, part);
            fprintf(stderr, "[fault] save cut: %lld of the card's bytes written, the last write %zu of %zu bytes into %s; "
                            "the process ends now (VCS_NX_FAULT_SAVE_BYTES)\n", limit, part, count, card_path[fd]);
            fflush(stderr);
            svcExitProcess();
        }
        card_written += (long long)count;
    }
    return __real__write_r(reent, fd, buf, count);
}

int __wrap__close_r(struct _reent *reent, int fd)
{
    if (fd >= 0 && fd < TRACKED_FDS) card_fd[fd] = 0;
    return __real__close_r(reent, fd);
}
