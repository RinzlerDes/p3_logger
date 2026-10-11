/**
 ******************************************************************************
 * @file    s25fl128s.h
 * @brief   PROVIDED driver — Cypress/Infineon S25FL128S 128-Mbit QSPI NOR
 *          flash (U8 on the STM32WB5MM-DK), the P3 log store.
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 *
 *  Thin, correct HAL-QSPI driver in the course "component" style (same model
 *  as stts22h/ism330dhcx: a small, board-agnostic driver you *call*, not the
 *  full ST BSP you must not pull in — see the README). Single-line (1-1-1)
 *  commands over the QUADSPI peripheral: unambiguously correct, and fast
 *  enough for a logger. (Quad-I/O read and memory-mapped mode are documented
 *  enhancements, left as an exercise — not needed to pass P3.)
 *
 *  It owns NO peripheral init: CubeMX's MX_QUADSPI_Init() built `hqspi`; you
 *  hand that handle to S25FL128S_Init() once, and every call here uses it.
 *  Same ownership split as the I2C sensors (spec R32).
 *
 *  Geometry (S25FL128SDSMFV001, datasheet Table 10.7 for timing):
 *    - 16 MByte total, 24-bit addressing (3 address bytes cover the array).
 *    - 256-byte program page — a page-program must not cross a page boundary.
 *    - Hybrid sectors: thirty-two 4-KB parameter sectors + 64-KB uniform
 *      sectors. Erase granularity is 4 KB (P4E) or 64 KB (SE).
 *    - Program: typ 250 us / max 750 us per page. Erase: typ 130 ms /
 *      max 650 ms per sector. Bulk erase: typ 33 s / max 165 s.
 *
 *  *** THE RTOS LESSON LIVES IN THESE NUMBERS ***  Every call that waits for
 *  the chip (program, and especially erase) BLOCKS the calling task until the
 *  Write-In-Progress bit clears — up to 650 ms for one 64-KB erase. That is
 *  why P3 confines erase to the operator FORMAT command and never holds a
 *  mutex across it on the logging hot path (spec R17/R19).
 ******************************************************************************
 */
#ifndef S25FL128S_H
#define S25FL128S_H

#include "main.h"          /* pulls in stm32wbxx_hal.h + QSPI_HandleTypeDef */
#include <stdint.h>

/* ---- Geometry -------------------------------------------------------------*/
#define S25FL128S_SIZE_BYTES     (16u * 1024u * 1024u)  /* 16 MB             */
#define S25FL128S_PAGE_SIZE      256u                    /* program page      */
#define S25FL128S_SECTOR_4K      4096u                   /* parameter sector  */
#define S25FL128S_SECTOR_64K     65536u                  /* uniform sector    */

/* ---- Expected JEDEC RDID (0x9F) bytes, for POST verification --------------*/
#define S25FL128S_MANUF_ID       0x01u   /* Spansion / Cypress / Infineon     */
#define S25FL128S_MEMTYPE_ID     0x20u   /* FL-S family                       */
#define S25FL128S_CAPACITY_ID    0x18u   /* 128 Mbit (2^24 bytes)             */

/* ---- Return codes ---------------------------------------------------------*/
#define S25FL128S_OK              0
#define S25FL128S_ERR            -1
#define S25FL128S_ERR_ALIGN      -2      /* program crossed a page boundary   */
#define S25FL128S_ERR_TIMEOUT    -3      /* chip stayed busy past the budget  */

/**
 * @brief  Bind the driver to the CubeMX-owned QUADSPI handle. Call once,
 *         after MX_QUADSPI_Init(), before any other function here.
 * @param  hqspi  &hqspi from main.c (CubeMX-generated).
 * @retval S25FL128S_OK always (no device I/O — the handle is just stored).
 */
int32_t S25FL128S_Init(QSPI_HandleTypeDef *hqspi);

/**
 * @brief  Read the JEDEC device identification (RDID, 0x9F).
 * @param  manuf     [out] manufacturer id  (expect S25FL128S_MANUF_ID).
 * @param  memtype   [out] memory type       (expect S25FL128S_MEMTYPE_ID).
 * @param  capacity  [out] capacity code      (expect S25FL128S_CAPACITY_ID).
 * @retval S25FL128S_OK on a clean 3-byte read, else S25FL128S_ERR.
 * @note   POST uses this to confirm the flash is alive and is the expected part.
 */
int32_t S25FL128S_ReadID(uint8_t *manuf, uint8_t *memtype, uint8_t *capacity);

/**
 * @brief  Read `len` bytes starting at flash `addr` (READ, 0x03, 1-1-1).
 * @param  addr  24-bit start address (0 .. 16 MB-1).
 * @param  buf   [out] destination.
 * @param  len   byte count (no page/sector alignment required for reads).
 * @retval S25FL128S_OK / S25FL128S_ERR. Does not block on WIP (reads are
 *         valid any time the chip is idle).
 */
int32_t S25FL128S_Read(uint32_t addr, uint8_t *buf, uint32_t len);

/**
 * @brief  Program up to one page (<=256 B) that does NOT cross a 256-B page
 *         boundary (PP, 0x02). Issues WREN, programs, then blocks on WIP.
 * @param  addr  destination; addr & (addr+len-1) must lie in the same page.
 * @param  buf   source bytes.
 * @param  len   1 .. 256; caller guarantees the target page is erased (0xFF).
 * @retval S25FL128S_OK, S25FL128S_ERR_ALIGN (page-cross), S25FL128S_ERR_TIMEOUT,
 *         or S25FL128S_ERR.
 * @note   Blocks ~<=750 us worst case. The log store batches 16 records into a
 *         256-B page and calls this exactly once per full page (spec R18).
 */
int32_t S25FL128S_PageProgram(uint32_t addr, const uint8_t *buf, uint32_t len);

/**
 * @brief  Erase the 4-KB parameter sector containing `addr` (P4E, 0x20).
 *         WREN, erase, then block on WIP.
 * @retval S25FL128S_OK / _TIMEOUT / _ERR. BLOCKS up to ~650 ms — CONFIG/FORMAT
 *         path only (spec R19), never on the logging hot path.
 */
int32_t S25FL128S_EraseSector4K(uint32_t addr);

/**
 * @brief  Erase the 64-KB uniform sector containing `addr` (SE, 0xD8).
 * @retval S25FL128S_OK / _TIMEOUT / _ERR. BLOCKS up to ~650 ms — FORMAT only.
 */
int32_t S25FL128S_EraseSector64K(uint32_t addr);

/**
 * @brief  Bulk-erase the whole chip (BE, 0xC7). BLOCKS up to ~165 s — only
 *         meaningful from a bench/manufacturing utility, never a demo path.
 * @retval S25FL128S_OK / _TIMEOUT / _ERR.
 */
int32_t S25FL128S_EraseChip(void);

/**
 * @brief  Block until the Write-In-Progress (WIP) status bit clears.
 * @param  timeout_ms  give up after this long (use a value >= the operation's
 *                     max: 100 for program, 1000 for 4K/64K erase, 180000 chip).
 * @retval S25FL128S_OK when ready, S25FL128S_ERR_TIMEOUT otherwise.
 * @note   Implemented with HAL_QSPI_AutoPolling on RDSR1 (0x05, WIP=bit0).
 */
int32_t S25FL128S_WaitReady(uint32_t timeout_ms);

#endif /* S25FL128S_H */
