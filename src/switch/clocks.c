/* M10-01 / M10-11: the measurement conditions on every report line: the performance mode and its configuration
 * (applet), the operation mode, and once the program ID the process runs under.
 * Stock clocks are a measurement condition (docs/MILESTONES.md M10-01). The clock rates themselves are NOT read: the
 * first version opened clkrst sessions to pcv for them, and on 2026-11-24 (wide0-docked, the second run with it) pcv
 * aborted inside that first call, nvservices followed and the console went to Atmosphere's fatal screen
 * (docs/milestones/M10/hardware/NOTES.md). On this console sys-clk also holds pcv sessions. Overclocks are checked
 * instead by the program ID (psm's charger and battery read none / 0% throughout route-prof-docked, so it is not
 * asked either): sys-clk applies the profile of the title the homebrew runs under
 * (/config/sys-clk/config.ini). apm is not opened either (it drives pcv); the applet reports the mode. */
#include <switch.h>
#include <stdio.h>

static int opened;
static u64 program_id;

static void open_services(void)
{
    opened = 1;
    svcGetInfo(&program_id, InfoType_ProgramId, CUR_PROCESS_HANDLE, 0);
}

void gtavcs_clocks_line(char *out, size_t size)
{
    if (!opened) open_services();
    const ApmPerformanceMode mode = appletGetPerformanceMode();
    snprintf(out, size,
        "perf_mode=%d operation_mode=%s program_id=%016llx",
        (int)mode,
        appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld",
        (unsigned long long)program_id);
}
