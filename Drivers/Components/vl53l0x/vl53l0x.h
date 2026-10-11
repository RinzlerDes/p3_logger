/**
 ******************************************************************************
 * @file    vl53l0x.h
 * @brief   PROVIDED driver — ST VL53L0X time-of-flight ranging sensor (U6 on
 *          the STM32WB5MM-DK, I2C3 @ 7-bit 0x29), the P3 presence/proximity
 *          channel.
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 *
 *  ┌────────────────────────────────────────────────────────────────────────┐
 *  │  PROVENANCE — READ THIS.                                                 │
 *  │  The VL53L0X datasheet (Collateral/05) documents the part but publishes  │
 *  │  NO register map. ST's official ranging code is a large middleware API   │
 *  │  (UM2039 / the VL53L0X API), which is NOT in this course's collateral.   │
 *  │  The register addresses below are the widely-published community /       │
 *  │  application-note values for a MINIMAL single-shot read — they are NOT   │
 *  │  vendor-verified from a datasheet on disk, and this driver performs      │
 *  │  NONE of ST's calibration/tuning sequence, so the reported millimetre    │
 *  │  value is uncalibrated (fine for "near / far / present" logging, not for │
 *  │  absolute-distance metrology).                                           │
 *  │                                                                          │
 *  │  INSTRUCTOR OPTION: drop in ST's official VL53L0X API as a black-box     │
 *  │  component (RegisterBusIO/Init/GetDistance) and re-point vitals_bus at   │
 *  │  it; the App layer calls only VL53L0X_ReadRangeSingle(), so nothing      │
 *  │  above this file changes. VALIDATE THIS DRIVER ON REAL HARDWARE before   │
 *  │  relying on it in a graded demo.                                         │
 *  └────────────────────────────────────────────────────────────────────────┘
 *
 *  Board notes (UM2825-verified): the VL53L0X shares I2C3 with the temp and
 *  IMU sensors, so every call here MUST be made under the same I2C3 mutex the
 *  sensor reads use (spec R7). Its XSHUT is hardwired high on the board (no
 *  MCU GPIO), so there is nothing to drive to power it up. Its GPIO1 interrupt
 *  is on PD9 but this driver POLLS to completion instead (see the 33 ms note
 *  on VL53L0X_ReadRangeSingle).
 ******************************************************************************
 */
#ifndef VL53L0X_H
#define VL53L0X_H

#include "main.h"          /* I2C_HandleTypeDef                              */
#include <stdint.h>

#define VL53L0X_I2C_ADDR_8BIT   0x52u    /* 7-bit 0x29 << 1                    */
#define VL53L0X_MODEL_ID        0xEEu    /* IDENTIFICATION_MODEL_ID expected   */

#define VL53L0X_OK               0
#define VL53L0X_ERR             -1
#define VL53L0X_ERR_TIMEOUT     -2

/** Small handle: which bus, which address, ready flag. */
typedef struct {
    I2C_HandleTypeDef *hi2c;
    uint16_t           addr8;      /* 8-bit form for HAL (VL53L0X_I2C_ADDR..)  */
    uint8_t            ready;
} VL53L0X_Object_t;

/**
 * @brief  Bind the driver to a bus handle + address (no I/O). Call once at
 *         bring-up, mirroring the sensors' RegisterBusIO step.
 * @retval VL53L0X_OK.
 */
int32_t VL53L0X_RegisterBus(VL53L0X_Object_t *obj,
                            I2C_HandleTypeDef *hi2c, uint16_t addr8);

/**
 * @brief  Read IDENTIFICATION_MODEL_ID (0xC0). POST compares to
 *         VL53L0X_MODEL_ID (0xEE).
 * @retval VL53L0X_OK / VL53L0X_ERR.
 */
int32_t VL53L0X_ReadId(VL53L0X_Object_t *obj, uint8_t *model_id);

/**
 * @brief  Minimal bring-up: confirm the model id and mark the object ready.
 *         (Deliberately does NOT run ST's tuning sequence — see the file
 *         banner.) Leaves the sensor idle, ready for single-shot ranging.
 * @retval VL53L0X_OK / VL53L0X_ERR.
 */
int32_t VL53L0X_Init(VL53L0X_Object_t *obj);

/**
 * @brief  Trigger one single-shot range measurement and return it in mm.
 * @param  range_mm [out] measured range (uncalibrated; see banner).
 * @retval VL53L0X_OK, VL53L0X_ERR_TIMEOUT (no data-ready in time), VL53L0X_ERR.
 * @note   BLOCKS the caller for one ranging budget (~33 ms) while it polls the
 *         data-ready flag. Because the call runs under the I2C3 mutex, that is
 *         a ~33 ms mutex hold — far longer than a sensor register read. For P3
 *         that is acceptable at 1 Hz, but the clean design (a documented
 *         enhancement) splits this into a non-blocking start + a read-if-ready
 *         so the bus is not held across the ranging budget.
 */
int32_t VL53L0X_ReadRangeSingle(VL53L0X_Object_t *obj, uint16_t *range_mm);

#endif /* VL53L0X_H */
