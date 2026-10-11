/**
 ******************************************************************************
 * @file    tasks.h
 * @brief   P3 application entry points and the shared RTOS handles.
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 *
 *  Two calls, wired into the CubeMX-generated main.c (refer to the README
 *  section "Generated-file touchpoints"):
 *    - app_post()   runs BEFORE the scheduler. It runs POST on the sensors
 *                   and the flash, and sets up the perf baseline. This
 *                   function can use blocking HAL calls.
 *    - app_start()  makes the mutexes, the queue, the semaphore, and the
 *                   tasks. Call it immediately before osKernelStart().
 ******************************************************************************
 */
#ifndef TASKS_H
#define TASKS_H

#include "cmsis_os2.h"

/* Shared synchronization handles (app_start makes them, and the application
 * and log_store.c use them). */
extern osMutexId_t       flashMutexHandle;   /* PRIMARY guarded resource (R6) */
extern osMutexId_t       i2c3MutexHandle;    /* SECONDARY: the sensor bus (R7) */
extern osMessageQueueId_t vitalsQueueHandle; /* samplers -> logger (R8)        */
extern osSemaphoreId_t   imuDrdySemHandle;   /* IMU DRDY ISR -> task (R4)      */

void app_post(void);
void app_start(void);

#endif /* TASKS_H */
