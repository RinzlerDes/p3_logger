/**
 ******************************************************************************
 * @file    s25fl128s.c
 * @brief   PROVIDED driver — S25FL128S 128-Mbit QSPI NOR flash. See the
 *          header for geometry, timing, and the RTOS-blocking lesson.
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 *
 *  All commands are single-line (1-1-1) over the QUADSPI peripheral. Standard
 *  SPI-NOR opcodes (JESD216-class), verified against the S25FL128S datasheet
 *  in Collateral/05. Every mutating op follows the NOR contract:
 *      WREN (0x06) -> command -> poll WIP (RDSR1 0x05, bit0) until clear.
 ******************************************************************************
 */
#include "s25fl128s.h"

/* ---- Opcodes (S25FL128S datasheet §9 Command Set) -------------------------*/
#define CMD_RDID     0x9Fu   /* read JEDEC id (3 bytes)                        */
#define CMD_RDSR1    0x05u   /* read status register 1 (WIP=bit0, WEL=bit1)   */
#define CMD_WREN     0x06u   /* write enable (sets WEL)                        */
#define CMD_READ     0x03u   /* read data (1-1-1, 24-bit address)             */
#define CMD_PP       0x02u   /* page program (<=256 B)                        */
#define CMD_P4E      0x20u   /* parameter (4 KB) sector erase                 */
#define CMD_SE       0xD8u   /* 64 KB sector erase                            */
#define CMD_BE       0xC7u   /* bulk (chip) erase                             */

#define SR_WIP       0x01u   /* status reg 1: write in progress               */

#define CMD_TMO_MS   1000u   /* HAL_QSPI_Command/Transmit/Receive timeout     */

static QSPI_HandleTypeDef *s_qspi;   /* the CubeMX-owned handle, from Init    */

/* ---- small command builders ----------------------------------------------*/

/** Base command template: 1-line instruction, no address, no data. */
static QSPI_CommandTypeDef base_cmd(uint32_t instruction)
{
    QSPI_CommandTypeDef c = {0};
    c.InstructionMode   = QSPI_INSTRUCTION_1_LINE;
    c.Instruction       = instruction;
    c.AddressMode       = QSPI_ADDRESS_NONE;
    c.AddressSize       = QSPI_ADDRESS_24_BITS;
    c.AlternateByteMode = QSPI_ALTERNATE_BYTES_NONE;
    c.DataMode          = QSPI_DATA_NONE;
    c.DummyCycles       = 0;
    c.DdrMode           = QSPI_DDR_MODE_DISABLE;
    c.SIOOMode          = QSPI_SIOO_INST_EVERY_CMD;   /* WB55 has no DdrHoldHalfCycle field */
    return c;
}

/** Issue Write-Enable and confirm the peripheral accepted the command. */
static int32_t write_enable(void)
{
    QSPI_CommandTypeDef c = base_cmd(CMD_WREN);
    return (HAL_QSPI_Command(s_qspi, &c, CMD_TMO_MS) == HAL_OK)
               ? S25FL128S_OK : S25FL128S_ERR;
}

/* ---- public API -----------------------------------------------------------*/

int32_t S25FL128S_Init(QSPI_HandleTypeDef *hqspi)
{
    s_qspi = hqspi;                 /* CubeMX already configured the IP        */
    return S25FL128S_OK;
}

int32_t S25FL128S_ReadID(uint8_t *manuf, uint8_t *memtype, uint8_t *capacity)
{
    uint8_t id[3] = {0};
    QSPI_CommandTypeDef c = base_cmd(CMD_RDID);
    c.DataMode = QSPI_DATA_1_LINE;
    c.NbData   = 3u;

    if (HAL_QSPI_Command(s_qspi, &c, CMD_TMO_MS) != HAL_OK) return S25FL128S_ERR;
    if (HAL_QSPI_Receive(s_qspi, id, CMD_TMO_MS)  != HAL_OK) return S25FL128S_ERR;

    if (manuf)    *manuf    = id[0];
    if (memtype)  *memtype  = id[1];
    if (capacity) *capacity = id[2];
    return S25FL128S_OK;
}

int32_t S25FL128S_WaitReady(uint32_t timeout_ms)
{
    /* AutoPolling repeatedly issues RDSR1 and matches WIP==0 in hardware,
     * releasing the CPU from the poll loop but still blocking THIS task until
     * match-or-timeout (the whole point of confining erase to FORMAT). */
    QSPI_CommandTypeDef c = base_cmd(CMD_RDSR1);
    c.DataMode = QSPI_DATA_1_LINE;
    c.NbData   = 1u;

    QSPI_AutoPollingTypeDef cfg = {0};
    cfg.Match           = 0x00u;              /* want WIP = 0                   */
    cfg.Mask            = SR_WIP;             /* look only at bit0             */
    cfg.MatchMode       = QSPI_MATCH_MODE_AND;
    cfg.StatusBytesSize = 1u;
    cfg.Interval        = 0x10u;
    cfg.AutomaticStop   = QSPI_AUTOMATIC_STOP_ENABLE;

    return (HAL_QSPI_AutoPolling(s_qspi, &c, &cfg, timeout_ms) == HAL_OK)
               ? S25FL128S_OK : S25FL128S_ERR_TIMEOUT;
}

int32_t S25FL128S_Read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (len == 0u) return S25FL128S_OK;

    QSPI_CommandTypeDef c = base_cmd(CMD_READ);
    c.AddressMode = QSPI_ADDRESS_1_LINE;
    c.Address     = addr;
    c.DataMode    = QSPI_DATA_1_LINE;
    c.NbData      = len;

    if (HAL_QSPI_Command(s_qspi, &c, CMD_TMO_MS) != HAL_OK) return S25FL128S_ERR;
    return (HAL_QSPI_Receive(s_qspi, buf, CMD_TMO_MS) == HAL_OK)
               ? S25FL128S_OK : S25FL128S_ERR;
}

int32_t S25FL128S_PageProgram(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    if (len == 0u) return S25FL128S_OK;
    if (len > S25FL128S_PAGE_SIZE) return S25FL128S_ERR_ALIGN;
    /* PP wraps within a 256-B page in hardware; reject a cross-page write so a
     * caller bug corrupts nothing silently. */
    if ((addr / S25FL128S_PAGE_SIZE) != ((addr + len - 1u) / S25FL128S_PAGE_SIZE))
        return S25FL128S_ERR_ALIGN;

    if (write_enable() != S25FL128S_OK) return S25FL128S_ERR;

    QSPI_CommandTypeDef c = base_cmd(CMD_PP);
    c.AddressMode = QSPI_ADDRESS_1_LINE;
    c.Address     = addr;
    c.DataMode    = QSPI_DATA_1_LINE;
    c.NbData      = len;

    if (HAL_QSPI_Command(s_qspi, &c, CMD_TMO_MS) != HAL_OK) return S25FL128S_ERR;
    if (HAL_QSPI_Transmit(s_qspi, (uint8_t *)buf, CMD_TMO_MS) != HAL_OK)
        return S25FL128S_ERR;

    return S25FL128S_WaitReady(100u);        /* PP max ~750 us                 */
}

/** Shared erase helper: WREN -> {P4E|SE} with 24-bit address -> wait. */
static int32_t erase_at(uint32_t instruction, uint32_t addr, uint32_t tmo_ms)
{
    if (write_enable() != S25FL128S_OK) return S25FL128S_ERR;

    QSPI_CommandTypeDef c = base_cmd(instruction);
    c.AddressMode = QSPI_ADDRESS_1_LINE;
    c.Address     = addr;

    if (HAL_QSPI_Command(s_qspi, &c, CMD_TMO_MS) != HAL_OK) return S25FL128S_ERR;
    return S25FL128S_WaitReady(tmo_ms);
}

int32_t S25FL128S_EraseSector4K(uint32_t addr)
{
    return erase_at(CMD_P4E, addr, 1000u);   /* 4K/64K erase max ~650 ms       */
}

int32_t S25FL128S_EraseSector64K(uint32_t addr)
{
    return erase_at(CMD_SE, addr, 1000u);
}

int32_t S25FL128S_EraseChip(void)
{
    if (write_enable() != S25FL128S_OK) return S25FL128S_ERR;

    QSPI_CommandTypeDef c = base_cmd(CMD_BE);
    if (HAL_QSPI_Command(s_qspi, &c, CMD_TMO_MS) != HAL_OK) return S25FL128S_ERR;
    return S25FL128S_WaitReady(180000u);     /* bulk erase max ~165 s          */
}
