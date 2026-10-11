/**
 ******************************************************************************
 * @file    perf_mon.c
 * @brief   PROVIDED — idle-hook CPU-utilization meter. See perf_mon.h for the
 *          one-line generated-file touchpoint and the method.
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 ******************************************************************************
 */
#include "perf_mon.h"

volatile uint32_t g_idle_count;        /* ++ by vApplicationIdleHook (freertos.c) */

static uint32_t s_baseline;            /* idle iterations/sec at 0 % load       */
static uint32_t s_last;                /* g_idle_count at the previous 1 s tick */
static uint32_t s_per_sec;             /* idle iterations in the last window     */

void perf_mon_init(void)
{
    g_idle_count = 0u;
    s_baseline = 0u;
    s_last = 0u;
    s_per_sec = 0u;
}

void perf_mon_tick_1s(void)
{
    uint32_t now = g_idle_count;
    s_per_sec = now - s_last;          /* unsigned overflow gives the delta     */
    s_last = now;

    /* The first window has the system up with no load, so it gives the
     * 0 %-load baseline. LIMITATION (document it in the report, spec R28):
     * this code assumes that the first second is quiet, and the baseline is
     * invalid after any clock change or optimizer change. Calibrate again
     * in that condition. */
    if (s_baseline == 0u)
        s_baseline = s_per_sec;
}

uint8_t perf_cpu_percent(void)
{
    if (s_baseline == 0u || s_per_sec >= s_baseline)
        return 0u;
    return (uint8_t)(100u - (100u * s_per_sec) / s_baseline);
}

uint32_t perf_idle_per_sec(void) { return s_per_sec; }
uint32_t perf_baseline(void)     { return s_baseline; }
