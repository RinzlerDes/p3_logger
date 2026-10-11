/**
 ******************************************************************************
 * @file    vitals_bus.c
 * @brief   Sensor-bus binding — PROVIDED for P3 (copied from the P2
 *          solution, and extended with the VL53L0X ToF binding). Refer to
 *          vitals_bus.h.
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 *
 *  The STTS22H and ISM330DHCX binding is byte-for-byte the P2 code, because
 *  the silicon and the bus did not change. The one new part is the VL53L0X
 *  branch at the end of vitals_bus_init(). The two I2C-component sensors
 *  share the five IO hooks (bus_read, bus_write, bus_tick, and the others).
 *  The ToF driver does its own HAL I2C access, so it needs only the handle
 *  and the address.
 ******************************************************************************
 */
#include <stdio.h>
#include "main.h"
#include "vitals_bus.h"

extern I2C_HandleTypeDef hi2c3;          /* CubeMX-generated (main.c)         */

#define BUS_TMO_MS 100u

STTS22H_Object_t    temp_sensor;
ISM330DHCX_Object_t imu;
VL53L0X_Object_t    tof;

/* ================== IO binding: five small functions ==================== */

static int32_t bus_init(void)   { return 0; }   /* MX_I2C3_Init owns the bus  */
static int32_t bus_deinit(void) { return 0; }

static int32_t bus_read(uint16_t dev, uint16_t reg, uint8_t *p, uint16_t len)
{
    return (HAL_I2C_Mem_Read(&hi2c3, dev, reg, I2C_MEMADD_SIZE_8BIT,
                             p, len, BUS_TMO_MS) == HAL_OK) ? 0 : -1;
}

static int32_t bus_write(uint16_t dev, uint16_t reg, uint8_t *p, uint16_t len)
{
    return (HAL_I2C_Mem_Write(&hi2c3, dev, reg, I2C_MEMADD_SIZE_8BIT,
                              p, len, BUS_TMO_MS) == HAL_OK) ? 0 : -1;
}

static int32_t bus_tick(void)      { return (int32_t)HAL_GetTick(); }
static void    bus_delay_ms(uint32_t ms) { HAL_Delay(ms); }

/* ============================ the binding ================================ */

int32_t vitals_bus_init(void)
{
    uint8_t id;

    /* ---- STTS22H: temperature (REQUIRED) ------------------------------- */
    STTS22H_IO_t tio = {
        .Init = bus_init, .DeInit = bus_deinit, .BusType = STTS22H_I2C_BUS,
        .Address = STTS22H_I2C_ADD_H, .WriteReg = bus_write,
        .ReadReg = bus_read, .GetTick = bus_tick,
    };
    if (STTS22H_RegisterBusIO(&temp_sensor, &tio) != STTS22H_OK ||
        STTS22H_ReadID(&temp_sensor, &id) != STTS22H_OK || id != STTS22H_ID) {
        printf("POST: STTS22H     MISSING / wrong id\n");
        return -1;
    }
    if (STTS22H_Init(&temp_sensor) != STTS22H_OK ||
        STTS22H_TEMP_SetOutputDataRate(&temp_sensor, 1.0f) != STTS22H_OK ||
        STTS22H_TEMP_Enable(&temp_sensor) != STTS22H_OK) {
        printf("POST: STTS22H     config FAILED\n");
        return -1;
    }
    printf("POST: STTS22H     OK  WHO_AM_I=0x%02X\n", id);

    /* ---- ISM330DHCX: accelerometer (REQUIRED) -------------------------- */
    ISM330DHCX_IO_t mio = {
        .Init = bus_init, .DeInit = bus_deinit, .BusType = ISM330DHCX_I2C_BUS,
        .Address = ISM330DHCX_I2C_ADD_H, .WriteReg = bus_write,
        .ReadReg = bus_read, .GetTick = bus_tick, .Delay = bus_delay_ms,
    };
    if (ISM330DHCX_RegisterBusIO(&imu, &mio) != ISM330DHCX_OK ||
        ISM330DHCX_ReadID(&imu, &id) != ISM330DHCX_OK || id != ISM330DHCX_ID) {
        printf("POST: ISM330DHCX  MISSING / wrong id\n");
        return -2;
    }
    if (ISM330DHCX_Init(&imu) != ISM330DHCX_OK ||
        ISM330DHCX_ACC_SetFullScale(&imu, 2) != ISM330DHCX_OK ||
        ISM330DHCX_ACC_SetOutputDataRate(&imu, 104.0f) != ISM330DHCX_OK ||
        ISM330DHCX_ACC_Enable(&imu) != ISM330DHCX_OK) {
        printf("POST: ISM330DHCX  config FAILED\n");
        return -2;
    }
    printf("POST: ISM330DHCX  OK  WHO_AM_I=0x%02X\n", id);

    /* ---- VL53L0X: time-of-flight (NON-FATAL — see vitals_bus.h) --------- */
    VL53L0X_RegisterBus(&tof, &hi2c3, VL53L0X_I2C_ADDR_8BIT);
    if (VL53L0X_Init(&tof) == VL53L0X_OK) {
        printf("POST: VL53L0X     OK  MODEL_ID=0x%02X\n", VL53L0X_MODEL_ID);
    } else {
        printf("POST: VL53L0X     MISSING / unverified (proximity disabled)\n");
        /* tof.ready stays 0. The sampler skips the ToF channel. Not fatal. */
    }

    return 0;
}
