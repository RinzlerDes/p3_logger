/**
 ******************************************************************************
 * @file    ui_pages.c
 * @brief   OLED page renderers — LIVE and LOG complete (provided), TASKS a
 *          stub (STUDENT TODO — the R5 evidence path).
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 *
 *  These renderers do FORMATTING ONLY. The D1 pass criterion reads the
 *  student-written record path: the values change at their configured rates,
 *  and the record count increments. The provided code displays the data.
 *  YOUR data flow (samplers → queue → logger → status) feeds it.
 ******************************************************************************
 */
#include <stdio.h>
#include "ui_pages.h"
#include "oled.h"
#include "log_store.h"

static ui_page_t s_page = UI_PAGE_LIVE;

const char *ui_posture_name(uint8_t posture)
{
    switch (posture) {
    case UI_POSTURE_UPRIGHT: return "UPRIGHT";
    case UI_POSTURE_SUPINE:  return "SUPINE";
    case UI_POSTURE_LEFT:    return "LEFT";
    case UI_POSTURE_RIGHT:   return "RIGHT";
    case UI_POSTURE_PRONE:   return "PRONE";
    default:                 return "----";
    }
}

ui_page_t ui_pages_current(void) { return s_page; }

ui_page_t ui_pages_next(void)
{
    s_page = (ui_page_t)((s_page + 1u) % UI_PAGE_COUNT);
    oled_clear();                   /* page switch repaints from scratch      */
    return s_page;
}

/* ---- LIVE: temp / posture / proximity / %CPU (PROVIDED) ------------------*/
static void render_live(const ui_live_t *v)
{
    int whole = v->temp_dC / 10;
    int frac  = (v->temp_dC < 0 ? -v->temp_dC : v->temp_dC) % 10;

    oled_printf(0, "AL-24  LIVE");
    if (v->temp_fault)
        oled_printf(2, "temp  FAULT");
    else
        oled_printf(2, "temp  %d.%d C", whole, frac);
    oled_printf(4, "pose  %s", ui_posture_name(v->posture));
    if (v->range_fault)
        oled_printf(5, "prox  FAULT");
    else
        oled_printf(5, "prox  %u mm", (unsigned)v->range_mm);
    oled_printf(7, "CPU   %u %%", (unsigned)v->cpu_percent);
}

/* ---- LOG: count / coverage / FULL-OVERFLOW state (PROVIDED) --------------*/
static void render_log(void)
{
    log_status_t st;
    log_store_status(&st);

    unsigned pct = (st.capacity_records != 0u)
                 ? (unsigned)((uint64_t)st.record_count * 100u
                              / st.capacity_records)
                 : 0u;

    oled_printf(0, "AL-24  LOG");
    oled_printf(2, "rec  %lu", (unsigned long)st.record_count);
    oled_printf(3, "cap  %lu", (unsigned long)st.capacity_records);
    oled_printf(4, "used %u %%", pct);
    if (st.log_full)
        oled_printf(6, "LOG FULL  ovf %lu", (unsigned long)st.overflow_count);
    else
        oled_printf(6, "logging");
}

/* ---- TASKS: STUDENT TODO (spec R5, demo D2) -------------------------------
 * This page is the bench record that MEASUREMENT right-sized your stacks and
 * that your queue behaves correctly.
 *   - One row for each task: the name and uxTaskGetStackHighWaterMark()
 *     (the free words at the worst point so far).
 *   - The queue: the current depth and its high-water mark.
 * Include "FreeRTOS.h" and "task.h" here, or give the numbers to this
 * function. That selection is yours, because you own the design. Only YOUR
 * code can satisfy the D2 pass criterion. The stub below deliberately
 * shows nothing that a grader can grade. */
static void render_tasks(void)
{
    oled_printf(0, "AL-24  TASKS");
    oled_printf(3, "STUDENT TODO");
    oled_printf(5, "stack hi-water/task");
    oled_printf(6, "queue depth/hi-water");
}

void ui_pages_render(const ui_live_t *live)
{
    switch (s_page) {
    case UI_PAGE_LIVE:  render_live(live); break;
    case UI_PAGE_LOG:   render_log();      break;
    case UI_PAGE_TASKS: render_tasks();    break;
    default:            s_page = UI_PAGE_LIVE; render_live(live); break;
    }
}
