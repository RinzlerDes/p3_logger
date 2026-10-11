/**
 ******************************************************************************
 * @file    vitals_bus.h
 * @brief   Sensor-bus binding — connects the bus-agnostic ST component
 *          drivers to the CubeMX-owned I2C3 handle. CARRIED FROM P2, and
 *          extended for the P3 ToF sensor.
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 *
 *  Same interoperation lesson as P2 (refer to the P2 solution that P3 starts
 *  from). The component drivers know nothing about the board. They use the
 *  five IO function pointers that this module gives, and all five point at
 *  `hi2c3`. CubeMX owns MX_I2C3_Init(), and this binding owns the
 *  transactions. No code initializes a device two times.
 *
 *  P3 difference against P2: a THIRD device, the VL53L0X ToF sensor, shares
 *  I2C3 with the temperature sensor and the IMU. That is the cause of the P3
 *  I2C3 mutex. The single P2 super-loop caller made no contention on the bus.
 *  The concurrent P3 sampler tasks do make contention (spec R7). The
 *  transactions in this module do not change. In P3 the call sites do the
 *  *serialization* (refer to App/tasks.c).
 ******************************************************************************
 */
#ifndef VITALS_BUS_H
#define VITALS_BUS_H

#include "ism330dhcx.h"
#include "stts22h.h"
#include "vl53l0x.h"

/* The three bound driver objects, ready for component API calls after
 * vitals_bus_init() returns 0. vitals_bus.c owns them. After the scheduler
 * starts, all calls on these objects MUST hold the I2C3 mutex (R7). */
extern STTS22H_Object_t    temp_sensor;
extern ISM330DHCX_Object_t imu;
extern VL53L0X_Object_t    tof;

/**
 * @brief  Connect all three component drivers to hi2c3, compare their
 *         identity registers against the expected values, and enable
 *         measurement. The temperature sensor free-runs at 1 Hz, the
 *         accelerometer runs at 12.5 Hz at ±2 g, and the ToF sensor becomes
 *         ready for single-shot ranging. This function runs ONE TIME at
 *         bring-up, before the scheduler starts, thus it needs no mutex.
 * @retval  0   The temperature sensor and the IMU (the two required
 *              channels) answer with the correct id and give data. The
 *              function reports the ToF result, but that result is
 *              non-fatal. Refer to the remark below.
 * @retval -1   STTS22H missing, incorrect id, or config failure.
 * @retval -2   ISM330DHCX missing, incorrect id, or config failure.
 * @note   POST keeps the ToF sensor NON-FATAL. Its minimal driver is the
 *         "validate on hardware" part (refer to the vl53l0x.h banner), so a
 *         ToF failure prints a warning and keeps `tof.ready == 0` rather
 *         than blocking the full device. Read `tof.ready` before you trust
 *         a range value. An instructor who ships the official ST ToF API
 *         can make this condition a hard POST failure.
 */
int32_t vitals_bus_init(void);

#endif /* VITALS_BUS_H */
