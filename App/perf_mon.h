/**
 ******************************************************************************
 * @file    perf_mon.h
 * @brief   PROVIDED — CPU-utilization meter built on the FreeRTOS idle hook.
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 *
 *  How it works (spec R27/R28): the kernel calls vApplicationIdleHook() only
 *  when nothing else is ready to run, so counting how many times it runs in a
 *  fixed 1-second window measures spare CPU. Calibrate the count with no load
 *  (that is 0 % CPU / 100 % idle), then each second:
 *        %CPU = 100 * (1 - idle_this_second / idle_baseline)
 *
 *  ONE GENERATED-FILE TOUCHPOINT (see README): CubeMX generates the empty
 *  vApplicationIdleHook() in Core/Src/freertos.c when you enable USE_IDLE_HOOK.
 *  Add exactly one line inside its USER CODE section:
 *        g_idle_count++;                 // (with  #include "perf_mon.h"  above)
 *  The hook MUST do nothing else — no OS calls, no printf (spec R27).
 ******************************************************************************
 */
#ifndef PERF_MON_H
#define PERF_MON_H

#include <stdint.h>

/* vApplicationIdleHook() in the generated freertos.c increments this counter.
 * It is the only shared variable in P3 that is deliberately UNLOCKED. A
 * 32-bit aligned read or write is atomic on Cortex-M4. The only reader is
 * the UI task, and its priority is above the idle task priority, so a race
 * is not possible. The report must justify this missing lock (spec R11). */
extern volatile uint32_t g_idle_count;

/** Reset the meter. Call one time before the scheduler starts. */
void perf_mon_init(void);

/** Call this exactly one time each second, from the UI task. It snapshots the
 *  idle count and calculates the idle total of this window. On the FIRST call
 *  it latches that total as the 0 %-load baseline, so the first second after
 *  start must be quiet. */
void perf_mon_tick_1s(void);

/** @return the current CPU load, 0..100 %. It gives 0 until perf_mon_tick_1s()
 *  latches the baseline. */
uint8_t perf_cpu_percent(void);

/** @return idle-hook iterations counted in the most recent 1 s window. */
uint32_t perf_idle_per_sec(void);

/** @return the calibrated 0 %-load baseline (idle iterations/second). POST
 *  prints it, so the report can cite the value that the meter used
 *  (spec R28). */
uint32_t perf_baseline(void);

#endif /* PERF_MON_H */
