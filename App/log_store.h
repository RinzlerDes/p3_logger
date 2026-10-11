/**
 ******************************************************************************
 * @file    log_store.h
 * @brief   Append-only, timestamped flash log over the QSPI NOR — the P3
 *          audit record. STARTER SKELETON. The starter gives the record
 *          format and the geometry (spec R15–R20). The append, read, and
 *          format logic is YOURS.
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 *
 *  This module is the ONE module that touches the QSPI flash, so it is the
 *  one module that takes the flash mutex. All other code calls the functions
 *  below, and these functions hold `flashMutex` around each S25FL128S_* call
 *  (spec R6). All flash access stays behind this interface, which makes the
 *  concurrency between LoggerTask and DOWNLOAD provable and not scattered.
 ******************************************************************************
 */
#ifndef LOG_STORE_H
#define LOG_STORE_H

#include <stdint.h>

/* --- Record: fixed 16 bytes => 16 records in a 256-B flash page (spec R17) -*/
typedef enum {
    LOG_CH_TEMP  = 0,   /* payload: int16 centi-degC                          */
    LOG_CH_IMU   = 1,   /* payload: int16 ax,ay,az + uint8 posture + pad      */
    LOG_CH_TOF   = 2,   /* payload: uint16 range_mm + uint8 status + pad       */
    LOG_CH_EVENT = 3,   /* payload: uint32 code + uint32 arg                   */
} log_channel_t;

/* flags bits */
#define LOG_F_VALID     0x01u
#define LOG_F_ALERT     0x02u
#define LOG_F_FAULT     0x04u   /* sensor read error — value not trustworthy   */
#define LOG_F_MARK      0x08u   /* operator/session boundary marker            */

typedef struct __attribute__((packed)) {
    uint32_t timestamp_ms;      /* HAL_GetTick() at ACQUISITION (spec R13)     */
    uint8_t  channel;           /* log_channel_t                               */
    uint8_t  flags;             /* LOG_F_*                                     */
    uint8_t  payload[8];        /* per-channel, see enum                       */
    uint16_t crc16;             /* CRC-16/CCITT over bytes 0..13 (spec R15)    */
} log_record_t;                 /* sizeof == 16                                */

typedef struct {
    uint32_t record_count;      /* records committed to flash                  */
    uint32_t capacity_records;  /* data-region bytes / 16                      */
    uint8_t  log_full;          /* 1 after the region becomes full (spec R21)  */
    uint32_t overflow_count;    /* records refused after LOG FULL              */
} log_status_t;

/* ---- Return codes ---------------------------------------------------------*/
#define LOG_OK          0
#define LOG_ERR        -1
#define LOG_FULL       -2

/** CRC-16/CCITT (poly 0x1021, init 0xFFFF) over len bytes. Provided so your
 *  writer and the DOWNLOAD reader agree bit-for-bit. */
uint16_t log_crc16(const uint8_t *data, uint32_t len);

/** Connect the log to the flash driver and the mutex. Get the write pointer
 *  and the record count from the header sector (spec R20). Call this function
 *  one time, after S25FL128S_Init(), before the scheduler starts.
 *  @retval LOG_OK / LOG_ERR. */
int32_t log_store_init(void);

/** Bulk pre-erase the data region and reset the header to empty (spec R19).
 *  This is the ONLY function that runs a sector erase. It belongs to the
 *  CONFIG and FORMAT path only, and it blocks for hundreds of ms for each
 *  sector. @retval LOG_OK / LOG_ERR. */
int32_t log_format(void);

/** Append one record (stamps CRC, batches into the 256-B page, page-programs
 *  on a full page — spec R18). @retval LOG_OK, LOG_FULL (spec R21), LOG_ERR. */
int32_t log_append(const log_record_t *rec);

/** Read committed record #index (0 .. record_count-1) for DOWNLOAD.
 *  @retval LOG_OK / LOG_ERR. */
int32_t log_read_record(uint32_t index, log_record_t *out);

/** Snapshot the log status for STATUS / the OLED LOG page. */
void log_store_status(log_status_t *out);

#endif /* LOG_STORE_H */
