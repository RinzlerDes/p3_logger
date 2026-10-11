/**
 ******************************************************************************
 * @file    ui_pages.h
 * @brief   OLED page renderers for the AL-24 UI — LIVE and LOG PROVIDED,
 *          TASKS is YOURS (spec §3 Buttons, §6, demo D1/D2).
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 *
 *  Provided (formatting plumbing over oled.c, not the lesson):
 *    - the page registry and B1 cycling (LIVE → LOG → TASKS → LIVE).
 *    - the LIVE renderer (temp / posture / proximity / %CPU).
 *    - the LOG renderer (record count, coverage %, LOG-FULL/OVERFLOW).
 *
 *  STUDENT TODO (the R5 evidence path — graded at D2):
 *    - the TASKS page: the stack high-water mark of each task, from
 *      uxTaskGetStackHighWaterMark(), and the queue depth and high-water
 *      mark. The renderers only DISPLAY. Your data flow feeds them. D1
 *      grades the student-written record path, and not this formatting.
 ******************************************************************************
 */
#ifndef UI_PAGES_H
#define UI_PAGES_H

#include <stdint.h>

/* ---- page registry (B1 cycles these in order — spec §3) ------------------*/
typedef enum {
    UI_PAGE_LIVE = 0,
    UI_PAGE_LOG,
    UI_PAGE_TASKS,
    UI_PAGE_COUNT
} ui_page_t;

/* ---- posture classes (spec R14) — the IMU sampler derives these ----------*/
typedef enum {
    UI_POSTURE_UNKNOWN = 0,
    UI_POSTURE_UPRIGHT,
    UI_POSTURE_SUPINE,
    UI_POSTURE_LEFT,
    UI_POSTURE_RIGHT,
    UI_POSTURE_PRONE
} ui_posture_t;

/** Short display name ("UPRIGHT", "SUPINE", …) for a ui_posture_t. */
const char *ui_posture_name(uint8_t posture);

/* ---- LIVE page inputs — your tasks fill this snapshot --------------------*/
typedef struct {
    int16_t  temp_dC;      /* temperature, tenths of a degC (integer — §5.2) */
    uint8_t  temp_fault;   /* nonzero = sensor fault flagged (R15)           */
    uint8_t  posture;      /* ui_posture_t from the IMU sampler (R14)        */
    uint16_t range_mm;     /* ToF proximity                                  */
    uint8_t  range_fault;  /* nonzero = sensor fault flagged (R15)           */
    uint8_t  cpu_percent;  /* idle-hook %CPU (R28)                           */
} ui_live_t;

/** Current page. */
ui_page_t ui_pages_current(void);

/** Move to the next page (call this from your B1 handling) and return it. */
ui_page_t ui_pages_next(void);

/** Draw the CURRENT page. `live` feeds the LIVE page and must not be NULL.
 *  The LOG page reads log_store_status() itself. Call this function from
 *  your UI task only, because the OLED belongs to that task (§5.3). */
void ui_pages_render(const ui_live_t *live);

#endif /* UI_PAGES_H */
