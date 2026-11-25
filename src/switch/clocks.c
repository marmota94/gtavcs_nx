/* M10-01 / M10-11: the clocks the console actually runs at, the performance mode and the power supply, for every
 * report line. Stock clocks are a measurement condition (docs/MILESTONES.md M10-01): docked a supply that cannot give
 * full power (PsmChargerType_LowPower / NotSupported) or an overclock tool changes them, and with them every number.
 * clkrst (8.0.0+) reads the rate each module runs at now; apm the performance mode and its configuration id; psm the
 * charger and the battery. Each service is opened once, on the first call, and a refusal is reported, not fatal. */
#include <switch.h>
#include <stdio.h>

static int opened;
static bool have_clkrst, have_apm, have_psm;
static ClkrstSession cpu, gpu, emc;

static void open_services(void)
{
    opened = 1;
    if (hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize()))
    {
        have_clkrst = R_SUCCEEDED(clkrstOpenSession(&cpu, PcvModuleId_CpuBus, 3)) &&
                      R_SUCCEEDED(clkrstOpenSession(&gpu, PcvModuleId_GPU, 3)) &&
                      R_SUCCEEDED(clkrstOpenSession(&emc, PcvModuleId_EMC, 3));
    }
    have_apm = R_SUCCEEDED(apmInitialize());
    have_psm = R_SUCCEEDED(psmInitialize());
}

void gtavcs_clocks_line(char *out, size_t size)
{
    if (!opened) open_services();
    u32 c = 0, g = 0, e = 0;
    if (have_clkrst)
    {
        clkrstGetClockRate(&cpu, &c);
        clkrstGetClockRate(&gpu, &g);
        clkrstGetClockRate(&emc, &e);
    }
    ApmPerformanceMode mode = ApmPerformanceMode_Invalid;
    u32 config = 0;
    if (have_apm && R_SUCCEEDED(apmGetPerformanceMode(&mode)))
        apmGetPerformanceConfiguration(mode, &config);
    PsmChargerType charger = PsmChargerType_Unconnected;
    u32 battery = 0;
    if (have_psm)
    {
        psmGetChargerType(&charger);
        psmGetBatteryChargePercentage(&battery);
    }
    static const char *const chargers[] = {"none", "enough-power", "low-power", "not-supported"};
    snprintf(out, size,
        "cpu_mhz=%u gpu_mhz=%u emc_mhz=%u perf_mode=%d perf_config=%08x charger=%s battery=%u%% operation_mode=%s%s",
        c / 1000000u, g / 1000000u, e / 1000000u, (int)mode, config,
        (unsigned)charger < 4u ? chargers[charger] : "other", battery,
        appletGetOperationMode() == AppletOperationMode_Console ? "docked" : "handheld",
        have_clkrst ? "" : " (clkrst unavailable)");
}
