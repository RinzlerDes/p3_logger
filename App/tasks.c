/**
 ******************************************************************************
 * @file    tasks.c
 * @brief   P3 application — WORKING SMOKE TEST and the SKELETON of the full
 *          logger that you build. The large comment block at the bottom
 *          gives the target 4-task architecture (spec §1–§2) and tells you
 *          how to extend this code into it.
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 *
 *  The smoke test proves that the full stack is alive before you write one
 *  line of logger code. FreeRTOS schedules two tasks. The OLED and the
 *  console work from a task. The QSPI flash answers with its JEDEC id. The
 *  idle-hook %CPU meter gives a reading. Flash the board and open the VCP at
 *  115200 8N1. The console then shows the POST report, a heartbeat, and a
 *  1 Hz UI line with a live CPU number. A HELP command at the console
 *  exercises the provided command-table parser.
 ******************************************************************************
 */
#include "tasks.h"

#include <stdint.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "cmd_table.h"
#include "cmsis_os2.h"
#include "console.h"
#include "log_store.h"
#include "main.h"
#include "math.h"
#include "oled.h"
#include "perf_mon.h"
#include "queue.h"
#include "rgb_led.h"
#include "s25fl128s.h"
#include "stm32wbxx_hal.h"
#include "stts22h.h"
#include "ui_pages.h"
#include "vitals_bus.h"
#include "vl53l0x.h"

typedef struct {
    float    temp;
    uint16_t distance_mm;
} vitals_t;

extern QSPI_HandleTypeDef hqspi; /* CubeMX-generated once QUADSPI is on   */

/* ---- shared RTOS handles (declared extern in tasks.h / log_store.c) --------*/
osMutexId_t        flashMutexHandle;
osMutexId_t        i2c3MutexHandle;
osMessageQueueId_t vitalsQueueHandle;
osSemaphoreId_t    imuDrdySemHandle;

// vitals task
static vitals_t g_vitals;
static uint32_t g_vitals_queue_drops;

// imu task
typedef enum {
    POSTURE_UNKNOWN = 0,
    POSTURE_UPRIGHT,
    POSTURE_SUPINE,
    POSTURE_LEFT,
    POSTURE_RIGHT,
    POSTURE_PRONE
} posture_t;
static int32_t incline_ddeg, roll_ddeg;
static int32_t cal_incline_ddeg, cal_roll_ddeg; /* CAL offsets (R6)   */
// static uint8_t  imu_valid;
static uint32_t g_axes_failed_count;

// logger task
static uint32_t g_logger_errors;

/* ============================ POST (pre-scheduler) ======================== */
void app_post(void) {
    console_init();
    printf("\n=== AL-24 \"AmbuLog\" — P3 starter smoke test ===\n");
    printf("FW P3.0-starter   build %s %s\n", __DATE__, __TIME__);

    oled_init();
    oled_clear();
    oled_write_line(0, "AL-24 AmbuLog");
    oled_write_line(2, "POST...");
    rgb_init();
    rgb_set(0u, 0u, 8u); /* dim blue = booting                 */

    if (vitals_bus_init() < 0) printf("POST: sensor bus FAILED (temp + IMU are required)\n");

    /* QSPI flash: prove it is the expected part by its JEDEC id (spec D0). */
    S25FL128S_Init(&hqspi);
    uint8_t m = 0, t = 0, c = 0;
    if (S25FL128S_ReadID(&m, &t, &c) == S25FL128S_OK && m == S25FL128S_MANUF_ID)
        printf("POST: S25FL128S   OK  JEDEC=%02X %02X %02X (16 MB QSPI NOR)\n", m, t, c);
    else
        printf("POST: S25FL128S   MISSING / unexpected  JEDEC=%02X %02X %02X\n", m, t, c);

    log_store_init();
    perf_mon_init();
    printf("POST complete — starting the scheduler.\n\n");
}

/* ==================== console commands (smoke-test table) ================ */
/* The command-table PARSER is a provided module (cmd_table.c: line assembly,
 * tokenize, and dispatch). The smoke test registers exactly one verb, so you
 * can see the pattern work. Type HELP at the console.
 *
 * STUDENT TODO (spec R25 — the graded surface): extend this table into the
 * full command set — HELP, STATUS, MODE RUN|CONFIG, SET PERIOD <ms>,
 * SET THRESH TEMP <cC>|DIST <mm>, FORMAT (which SHALL still require its
 * confirmation token), DOWNLOAD [since <ts>], and CPU. Write the HANDLERS,
 * and select the position of the 50 ms non-blocking console tick in your
 * task set. The device SHALL echo all accepted parameters as a logged EVENT
 * record. */
static void cmd_help(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    printf("AL-24 commands:\n");
    cmd_table_help();
}

static const cmd_entry_t s_cmds[] = {
    { "HELP", cmd_help, "list commands" },
    /* STUDENT TODO: STATUS, MODE, SET, FORMAT, DOWNLOAD, CPU ... (R25) */
};

/* ============================ smoke-test tasks ============================ */

/* Task 1: a heartbeat. It proves that a second task runs and that the RGB
 * LED works. Priority osPriorityLow, and it blinks green each 500 ms.
 * rgb_set blocks about 11 ms. That cost causes no problem at a low priority
 * for a blink. In the full logger the RGB LED belongs to the UI task. This
 * blocking cost is the cause of the rule that keeps ALERT LED writes off the
 * sample path. */
static void HeartbeatTask(void* arg) {
    (void)arg;
    uint32_t on = 0u;
    for (;;) {
        on ^= 1u;
        rgb_set(0u, on ? 12u : 0u, 0u);
        osDelay(500u);
    }
}

/* Task 2: the UI — OLED pages + console commands + the idle-hook %CPU
 * readout. Paces at 50 ms (console stays responsive) with a 1 s sub-tick for
 * the display and the CPU meter. Demonstrates osDelayUntil (periodic,
 * jitter-free — spec R3), taking i2c3Mutex around a sensor read (spec R7),
 * the provided command-table parser (spec R25), and the provided LIVE page
 * renderer (spec §3 — the LOG/TASKS pages come alive as YOUR logger does). */
static void UITask(void* arg) {
    (void)arg;
    uint32_t tick = osKernelGetTickCount();
    uint32_t sub  = 0u;
    for (;;) {
        tick += 50u;
        osDelayUntil(tick);

        cmd_table_poll(); /* non-blocking console service   */

        if (++sub >= 20u) { /* ~1 s                           */
            sub = 0u;
            perf_mon_tick_1s();

            float tc = 0.0f; /* live temp under the bus mutex  */
            // osMutexAcquire(i2c3MutexHandle, osWaitForever);
            // (void)STTS22H_TEMP_GetTemperature(&temp_sensor, &tc);
            // osMutexRelease(i2c3MutexHandle);
            // int td = (int)(tc * 10.0f);

            int td = (int)(g_vitals.temp * 10.0f);

            ui_live_t live = {
                .temp_dC     = (int16_t)td,
                .temp_fault  = 0u,
                .posture     = UI_POSTURE_UNKNOWN, /* your IMU sampler fills */
                .range_mm    = 0u,                 /* your ToF read fills    */
                .range_fault = 0u,
                .cpu_percent = (uint8_t)perf_cpu_percent(),
            };
            ui_pages_render(&live); /* LIVE page (B1 cycling: connect
                                     * ui_pages_next() to the button
                                     * in your full design)           */

            printf("[ui] cpu=%u%%  idle/s=%lu  base=%lu  temp=%d.%dC\n",
                   perf_cpu_percent(),
                   (unsigned long)perf_idle_per_sec(),
                   (unsigned long)perf_baseline(),
                   td / 10,
                   (td < 0 ? -td : td) % 10);
        }
    }
}

static void vitals_sample_task() {
    uint32_t       tick   = osKernelGetTickCount();
    const uint32_t period = osKernelGetTickFreq();

    for (;;) {
        tick += period;
        osDelayUntil(tick);

        uint16_t distance    = 0u;
        int32_t  tof_result  = VL53L0X_ERR;
        int32_t  temp_result = STTS22H_ERROR;
        float    temp;

        osMutexAcquire(i2c3MutexHandle, osWaitForever);
        uint32_t now = HAL_GetTick();
        temp_result  = STTS22H_TEMP_GetTemperature(&temp_sensor, &temp);
        // always times out
        // tof_result = VL53L0X_ReadRangeSingle(&tof, &distance);
        osMutexRelease(i2c3MutexHandle);

        // if (tof_result == VL53L0X_OK) {
        //     g_vitals.distance_mm = distance;
        //     // printf("distance: %u mm\r\n", (unsigned)distance);
        // } else {
        //     // printf("ToF failed: %ld\r\n", (long)result);
        // }
        // // printf("distance: %u mm\r\n", (unsigned)distance);

        log_record_t record = { .timestamp_ms = now, .channel = LOG_CH_TEMP };

        if (temp_result == STTS22H_OK) {
            g_vitals.temp = temp;

            int16_t  temp_deci         = (int16_t)lroundf(g_vitals.temp * 10);
            uint16_t temp_deci_encoded = (uint16_t)temp_deci;

            record.payload[0] = temp_deci_encoded & 0xffu;
            record.payload[1] = (temp_deci_encoded >> 8) & 0xffu;
            record.flags      = LOG_F_VALID;
        } else {
            record.flags = LOG_F_FAULT;
        }
        osStatus_t status = osMessageQueuePut(vitalsQueueHandle, &record, 0, 0);
        if (status != osOK) {
            g_vitals_queue_drops++;
        }
    }
}

static void imu_task() {
    for (;;) {
        osSemaphoreAcquire(imuDrdySemHandle, osWaitForever);
        ISM330DHCX_Axes_t a;

        osMutexAcquire(i2c3MutexHandle, osWaitForever);
        uint32_t now    = HAL_GetTick();
        int32_t  result = ISM330DHCX_ACC_GetAxes(&imu, &a);
        osMutexRelease(i2c3MutexHandle);

        log_record_t record = { .timestamp_ms = now, .channel = LOG_CH_IMU, .flags = LOG_F_FAULT };

        if (result != ISM330DHCX_OK) {
            g_axes_failed_count++;
            goto cleanup;
        } else if (a.x > INT16_MAX || a.x < INT16_MIN || a.y > INT16_MAX || a.y < INT16_MIN ||
                   a.z > INT16_MAX || a.z < INT16_MIN) {
            goto cleanup;
        }

        float     inc          = atan2f(-a.x, sqrtf(a.y * a.y + a.z * a.z)) * 573.0f; /* 0.1° */
        float     roll         = atan2f(a.y, a.z) * 573.0f;
        int32_t   incline_ddeg = (int32_t)inc - cal_incline_ddeg;
        int32_t   roll_ddeg    = (int32_t)roll - cal_roll_ddeg;
        posture_t posture      = POSTURE_UNKNOWN;

        if (incline_ddeg > 300) {
            posture = POSTURE_UPRIGHT;
        } else if (roll_ddeg > 300) {
            posture = POSTURE_RIGHT;
        } else if (roll_ddeg < -300) {
            posture = POSTURE_LEFT;
        } else {
            posture = POSTURE_SUPINE;
        }

        record.payload[0] = (uint8_t)(a.x & 0xffu);
        record.payload[1] = (uint8_t)(a.x >> 8);
        record.payload[2] = (uint8_t)(a.y & 0xffu);
        record.payload[3] = (uint8_t)(a.y >> 8);
        record.payload[4] = (uint8_t)(a.z & 0xffu);
        record.payload[5] = (uint8_t)(a.z >> 8);
        record.payload[6] = posture;
        record.payload[7] = 0;

        record.flags = LOG_F_VALID;

    cleanup:
        int32_t status = osMessageQueuePut(vitalsQueueHandle, &record, 0, 0);
        if (status != osOK) {
            g_vitals_queue_drops++;
        }
    }
}

static void logger_task() {
    const uint32_t timeout = osKernelGetTickFreq() * 2;
    for (;;) {
        log_record_t record;
        int32_t      result;
        osStatus_t   status = osMessageQueueGet(vitalsQueueHandle, &record, NULL, timeout);

        if (status == osOK) {
            result = log_append(&record);
            if (result != LOG_OK && result != LOG_FULL) {
                // TODO
                g_logger_errors++;
                continue;
            }
        } else if (status == osErrorTimeout) {
            result = log_flush();
            if (result != LOG_OK && result != LOG_FULL) {
                g_logger_errors++;
                continue;
            }
        } else {
            // TODO
            g_logger_errors++;
            continue;
        }
    }
}

/* ============================ app_start ================================== */

void app_start(void) {
    /* Priority-inheriting mutexes (spec R6/R7). In CMSIS-RTOS2, osMutexNew
     * makes an inheriting mutex. The attribute bit makes the intent clear.
     * Refer to GR-A in the spec for a NON-inheriting variant. */
    const osMutexAttr_t mtx_attr = { .attr_bits = osMutexPrioInherit };
    flashMutexHandle             = osMutexNew(&mtx_attr);
    i2c3MutexHandle              = osMutexNew(&mtx_attr);

    /* Producer->consumer queue: 16 records of 16 bytes (spec R8). */
    vitalsQueueHandle = osMessageQueueNew(16u, sizeof(log_record_t), NULL);

    /* Binary semaphore for the IMU DRDY ISR->task handoff (spec R4). */
    imuDrdySemHandle = osSemaphoreNew(1u, 0u, NULL);

    // vitals mailbox
    // vitals_mailbox = xQueueCreate(1u, sizeof(vitals_t));

    /* Register the console command table. The parser is a provided module
     * (spec R25). The handlers and the tick position are yours (refer to
     * s_cmds above). */
    cmd_table_init(s_cmds, sizeof(s_cmds) / sizeof(s_cmds[0]));

    /* --- smoke-test tasks (REPLACE with the logger — see block below) --- */
    const osThreadAttr_t hb_attr = { .name = "hb", .stack_size = 512u, .priority = osPriorityLow };
    const osThreadAttr_t ui_attr = { .name = "ui", .stack_size = 2048u, .priority = osPriorityLow };
    const osThreadAttr_t vitals_sample_attr = { .name       = "vitals_sample",
                                                .stack_size = 2048u,
                                                .priority   = osPriorityNormal };
    const osThreadAttr_t imu_attr           = { .name       = "imu",
                                                .stack_size = 2048u,
                                                .priority   = osPriorityAboveNormal };
    const osThreadAttr_t logger_attr        = { .name       = "logger",
                                                .stack_size = 2048u,
                                                .priority   = osPriorityNormal };

    osThreadId_t hb = osThreadNew(HeartbeatTask, NULL, &hb_attr);
    osThreadId_t ui = osThreadNew(UITask, NULL, &ui_attr);
    osThreadId_t vitals_sample_thread_id =
        osThreadNew(vitals_sample_task, NULL, &vitals_sample_attr);
    osThreadId_t imu_thread_id    = osThreadNew(imu_task, NULL, &imu_attr);
    osThreadId_t logger_thread_id = osThreadNew(logger_task, NULL, &logger_attr);

    // printf("Heartbeat task: %s\n", hb ? "OK" : "FAILED");
    // printf("UI task: %s\n", ui ? "OK" : "FAILED");
}

/* ==================== ISR -> task handoff (spec R4/R10) =================== */
/* The IMU data-ready line (PD2/EXTI2) causes this HAL callback. RTOS-correct
 * discipline: release the semaphore and return. Do no I2C, no logging, and no
 * printf in interrupt context. osSemaphoreRelease is ISR-safe in CMSIS-RTOS2.
 * This callback is dormant in the smoke test, because the IMU DRDY output
 * stays disabled. Connect it when you build ImuSampleTask. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin) {
    if (GPIO_Pin == GPIO_PIN_2) { /* PD2 = ISM330DHCX INT1 (DRDY)     */
        osSemaphoreRelease(imuDrdySemHandle);
    }
    /* PC12/PC13 (buttons) also come here in the full design. Dispatch by pin. */
}

/*=============================================================================
  THE FULL AL-24 — what you build FROM here (spec §1–§2, and your P2 solution).

  Replace the two smoke-test tasks with FOUR tasks. The queue, the mutexes, and
  the semaphore that app_start() makes connect them:

  | Task              | Priority              | Trigger                | Work                          |
  |-------------------|-----------------------|------------------------|-------------------------------|
  | ImuSampleTask     | osPriorityAboveNormal | imuDrdySem (PD2 EXTI)  | read accel (i2c3Mutex),       |
  |                   |                       |                        | classify posture, PUT record. |
  | VitalsSampleTask  | osPriorityNormal      | osDelayUntil 1 s       | read temp + ToF (i2c3Mutex),  |
  |                   |                       |                        | PUT temp & tof records.       |
  | LoggerTask        | osPriorityNormal      | osMessageQueueGet      | GET record -> log_append()    |
  |                   |                       |  (1 s flush timeout)   | (flashMutex, in log_store).   |
  | UITask            | osPriorityLow         | osDelayUntil 50 ms     | OLED pages + cmd_table_poll   |
  |                   |                       |                        | + %CPU + RGB. DOWNLOAD and    |
  |                   |                       |                        | FORMAT handlers run here.     |

  Data flow:
     PD2 EXTI --give--> imuDrdySem --> ImuSampleTask --+
                                                       +--(i2c3Mutex)--> hi2c3
                          VitalsSampleTask ------------+
     ImuSampleTask, VitalsSampleTask --PUT--> vitalsQueue --GET--> LoggerTask
                                                       --(flashMutex)--> log_store -> QSPI
     UITask DOWNLOAD/FORMAT ----------------------------(flashMutex)--> log_store -> QSPI.

  The two showpiece behaviors to make work (spec D5/D7):
    * DOWNLOAD streams the log over the UART while LoggerTask continues to
      append. The flash mutex arbitrates the race between the reader and the
      writer. The %CPU value increases, and you can see that change.
    * FORMAT is the only sector-erase path. Steady-state logging only
      page-programs (spec R18/R19).

  The idle-hook %CPU meter works in this starter (perf_mon and the freertos.c
  one-line hook). Keep g_latest (the snapshot of the UI) and g_idle_count
  UNLOCKED, and justify that decision in the report (spec R11). Start from the
  sensor-read logic and the mode logic of your P2 solution. The RTOS structure
  above is the new part.
=============================================================================*/
