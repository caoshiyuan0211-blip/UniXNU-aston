// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef ASTON_CPU_FREQ_POLICY_H
#define ASTON_CPU_FREQ_POLICY_H
#include <stdint.h>

// UINT64_MAX is an unavailable/offline processor. Rebase after it comes online
// or its accounting regresses; neither case establishes a busy interval.
static inline uint32_t cpufreq_busy_per_mille(uint64_t idle, uint64_t previous, uint64_t elapsed)
{
    if (!elapsed || idle == UINT64_MAX || previous == UINT64_MAX || idle < previous) return 0;
    const uint64_t delta = idle - previous;
    if (delta >= elapsed) return 0;
    // Governor intervals are milliseconds, but avoid overflow even after a
    // delayed sample. Division first bounds the scaled remainder.
    const uint64_t busy = elapsed - delta;
    if (elapsed > UINT64_MAX / 1000) return (uint32_t)(busy / (elapsed / 1000 + 1));
    return (uint32_t)(busy * 1000 / elapsed);
}

// ---- Thermal limit (sheng-thermal) -----------------------------------------------------------------------------------------
// TSENS v2 (Linux drivers/thermal/qcom/tsens-v2.c): TM_Sn_STATUS at TM + 0xa0 + 4n, LAST_TEMP bits 11:0 signed in 0.1 C,
// VALID bit 21. Returns tenths of a degree, or CPUFREQ_NO_TEMP when the word is not valid or not plausible (-40 .. 150 C).
#define CPUFREQ_NO_TEMP INT32_MIN
static inline int32_t cpufreq_tsens_v2_dc(uint32_t status)
{
    if (!(status & (1u << 21))) return CPUFREQ_NO_TEMP;
    int32_t t = (int32_t)(status & 0xfff);
    if (t & 0x800) t -= 0x1000;
    if (t < -400 || t > 1500) return CPUFREQ_NO_TEMP;
    return t;
}

// One thermal tick (every 256 ms): the highest LUT step a domain may use, `limit` (0 = its lowest level, `top` = its highest).
// At or above `crit_dc` the domain drops to its lowest level at once. At or above `target_dc` it drops one level per tick. Once
// `hyst_dc` below the target it climbs one level per `up_ticks` cool ticks. In between it holds. No reading holds the limit.
static inline unsigned cpufreq_thermal_step(int32_t t_dc, unsigned limit, unsigned top, unsigned *cool,
                                            int32_t target_dc, int32_t crit_dc, int32_t hyst_dc, unsigned up_ticks)
{
    if (limit > top) limit = top;
    if (t_dc == CPUFREQ_NO_TEMP) return limit;
    if (t_dc >= crit_dc) {*cool = 0; return 0;}
    if (t_dc >= target_dc) {*cool = 0; return limit ? limit - 1 : 0;}
    if (t_dc < target_dc - hyst_dc) {
        if (++*cool >= up_ticks) {*cool = 0; return limit < top ? limit + 1 : top;}
        return limit;
    }
    *cool = 0;
    return limit;
}

// Stuck core (sheng-cpufreq-stuck-s): a CPU whose idle time has not moved for `windows` governor windows in a row (busy >= 99.0 %)
// is left out of its domain's busiest-CPU share, so one wedged or spinning core no longer holds its whole cluster at the top.
// It counts again from its first window with any idle time. Returns true while the CPU is excluded.
static inline int cpufreq_stuck_update(uint32_t busy_pm, uint32_t *run, uint32_t windows)
{
    if (!windows) return 0;
    if (busy_pm < 990) {*run = 0; return 0;}
    if (*run < windows) ++*run;
    return *run >= windows;
}
#endif
