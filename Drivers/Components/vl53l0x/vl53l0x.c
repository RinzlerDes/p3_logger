/**
 ******************************************************************************
 * @file    vl53l0x.c
 * @brief   PROVIDED driver — VL53L0X minimal single-shot ranging. See the
 *          header banner: the register addresses here are community/AN values
 *          for a minimal read, NOT vendor-verified from a datasheet on disk,
 *          and do NOT include ST's calibration sequence. Validate on hardware.
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 ******************************************************************************
 */
#include "vl53l0x.h"

/* ---- Registers (community/AN-documented minimal set — see header banner) --*/
#define REG_SYSRANGE_START           0x00u  /* write 0x01 = start single-shot */
#define REG_SYSTEM_INTERRUPT_CLEAR   0x0Bu  /* write 0x01 = clear int          */
#define REG_RESULT_INTERRUPT_STATUS  0x13u  /* bits[2:0] != 0 => data ready    */
#define REG_RESULT_RANGE_STATUS      0x14u  /* range (mm) is 16-bit at +10     */
#define REG_IDENTIFICATION_MODEL_ID  0xC0u  /* expect 0xEE                      */

#define RANGE_MM_OFFSET              10u    /* 0x14 + 10 = 0x1E                 */
#define IO_TMO_MS                    30u    /* per-transaction HAL timeout      */
#define RANGE_POLL_TMO_MS            60u    /* > one ~33 ms ranging budget      */

/* ---- tiny 8-bit-register helpers over HAL I2C -----------------------------*/

static int32_t rd(VL53L0X_Object_t *o, uint8_t reg, uint8_t *p, uint16_t n)
{
    return (HAL_I2C_Mem_Read(o->hi2c, o->addr8, reg, I2C_MEMADD_SIZE_8BIT,
                             p, n, IO_TMO_MS) == HAL_OK) ? VL53L0X_OK : VL53L0X_ERR;
}

static int32_t wr8(VL53L0X_Object_t *o, uint8_t reg, uint8_t v)
{
    return (HAL_I2C_Mem_Write(o->hi2c, o->addr8, reg, I2C_MEMADD_SIZE_8BIT,
                              &v, 1u, IO_TMO_MS) == HAL_OK) ? VL53L0X_OK : VL53L0X_ERR;
}

/* ---- public API -----------------------------------------------------------*/

int32_t VL53L0X_RegisterBus(VL53L0X_Object_t *obj,
                            I2C_HandleTypeDef *hi2c, uint16_t addr8)
{
    obj->hi2c  = hi2c;
    obj->addr8 = addr8;
    obj->ready = 0u;
    return VL53L0X_OK;
}

int32_t VL53L0X_ReadId(VL53L0X_Object_t *obj, uint8_t *model_id)
{
    return rd(obj, REG_IDENTIFICATION_MODEL_ID, model_id, 1u);
}

int32_t VL53L0X_Init(VL53L0X_Object_t *obj)
{
    uint8_t id = 0u;
    if (VL53L0X_ReadId(obj, &id) != VL53L0X_OK) return VL53L0X_ERR;
    if (id != VL53L0X_MODEL_ID)                 return VL53L0X_ERR;
    /* No ST tuning sequence here (see header banner). The part powers up in a
     * usable default state for a coarse single-shot read. */
    obj->ready = 1u;
    return VL53L0X_OK;
}

int32_t VL53L0X_ReadRangeSingle(VL53L0X_Object_t *obj, uint16_t *range_mm)
{
    uint8_t st = 0u;
    uint8_t raw[2] = {0};
    uint32_t t0;

    /* 1) kick off one measurement */
    if (wr8(obj, REG_SYSRANGE_START, 0x01u) != VL53L0X_OK) return VL53L0X_ERR;

    /* 2) poll data-ready (bits[2:0] of RESULT_INTERRUPT_STATUS) — this is the
     *    ~33 ms blocking wait the header warns about. */
    t0 = HAL_GetTick();
    do {
        if (rd(obj, REG_RESULT_INTERRUPT_STATUS, &st, 1u) != VL53L0X_OK)
            return VL53L0X_ERR;
        if ((HAL_GetTick() - t0) > RANGE_POLL_TMO_MS)
            return VL53L0X_ERR_TIMEOUT;
    } while ((st & 0x07u) == 0u);

    /* 3) read the 16-bit range (big-endian) at RESULT_RANGE_STATUS + 10 */
    if (rd(obj, (uint8_t)(REG_RESULT_RANGE_STATUS + RANGE_MM_OFFSET), raw, 2u)
            != VL53L0X_OK)
        return VL53L0X_ERR;
    *range_mm = (uint16_t)((raw[0] << 8) | raw[1]);

    /* 4) clear the interrupt so the next single-shot can assert it */
    (void)wr8(obj, REG_SYSTEM_INTERRUPT_CLEAR, 0x01u);
    return VL53L0X_OK;
}
