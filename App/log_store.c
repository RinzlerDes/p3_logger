/**
 ******************************************************************************
 * @file    log_store.c
 * @brief   Append-only flash log — STARTER SKELETON. The starter gives the
 *          mechanical parts (CRC, geometry, FORMAT erase, mutex helper, and
 *          status), and they are correct. The CORE LOGIC (page-batched
 *          append, header persistence, overflow, and read-back) carries a
 *          STUDENT TODO marker. Complete that logic with the same discipline
 *          as the P2 solution, and against spec R15–R21.
 *
 *          SWEN 563 / CMPE 663 — P3 "AL-24 AmbuLog" starter.
 ******************************************************************************
 */
#include "log_store.h"

#include "cmsis_os2.h" /* osMutexAcquire/Release (available after the
                                 * FreeRTOS regen — see README)                */
#include <string.h>

#include "s25fl128s.h"

/* ---- Flash layout (demo-scaled small so LOG FULL is reachable — R21/§7.3) -*/
#define HEADER_ADDR 0x000000u /* one 4-KB parameter sector   */
// #define DATA_BASE   0x010000u /* first 64-KB uniform sector  */
#define DATA_BASE        0x020000u
#define DATA_SECTORS     2u /* 2 x 64 KB = 128 KB          */
#define DATA_SIZE        (DATA_SECTORS * S25FL128S_SECTOR_64K)
#define CAPACITY_RECORDS (DATA_SIZE / sizeof(log_record_t)) /* 8192 records  */

/* ---- Header sector: PROVIDED layout (spec R20) -----------------------------
 * The header keeps the log position across a reset, and it needs no erase
 * for each update. The code uses the 4-KB parameter sector as an array of
 * fixed 32-B SLOTS. It writes one slot at a time, in sequence, with a
 * page-program only. NOR flash lets you page-program erased (0xFF) bytes one
 * time, so each header update takes the NEXT free slot. Only FORMAT erases
 * the sector. That behavior spreads the wear across 128 slots in each erase
 * cycle (the "rotating slot").
 *
 * PROVIDED: the struct, the slot geometry, and the `magic` value.
 * STUDENT TODO: the recovery scan at boot, and the slot-rotation update. The
 * scan must find the LAST slot that has a correct CRC. An erased slot reads
 * as all-0xFF and thus cannot agree with the `magic` value. The update must
 * take the next free slot. A sector that holds only spent slots means that
 * it is FORMAT time anyway. */
#define LOG_HDR_MAGIC 0x414C3234u /* "AL24"                    */

typedef struct __attribute__((packed)) {
    uint32_t magic;        /* LOG_HDR_MAGIC — erased slots (0xFF…) disagree   */
    uint32_t session_id;   /* increments on each FORMAT (spec R20)             */
    uint32_t write_off;    /* byte offset of the next append in the data region*/
    uint32_t record_count; /* records committed to flash                       */
    uint8_t  log_full;     /* the R21 latch, persisted                         */
    uint8_t  pad[13];      /* reserved — keeps the slot at exactly 32 B        */
    uint16_t crc16;        /* log_crc16 over bytes 0..29 (everything above)    */
} log_header_t;            /* sizeof == 32 — 8 slots in a 256-B page           */

_Static_assert(sizeof(log_header_t) == 32u, "header slot must stay 32 B");

#define HDR_SLOT_SIZE    ((uint32_t)sizeof(log_header_t))      /* 32 B   */
#define HDR_SLOT_COUNT   (S25FL128S_SECTOR_4K / HDR_SLOT_SIZE) /* 128    */
#define HDR_SLOT_ADDR(i) (HEADER_ADDR + (i) * HDR_SLOT_SIZE)

#define RECORD_SIZE         sizeof(log_record_t)
#define HEADER_SIZE         sizeof(log_header_t)
#define CRC_SIZE            sizeof(uint16_t)
#define RECORD_SLOT_ADDR(i) (DATA_BASE + (i) * RECORD_SIZE)

/* tasks.c (app_start) makes flashMutex. log_store is the one caller that
 * takes it, and flash_lock()/flash_unlock() enclose each flash operation. */
extern osMutexId_t flashMutexHandle;

/* ---- module state ---------------------------------------------------------*/
static uint32_t s_write_off; /* byte offset into data region*/
static uint32_t s_count;     /* committed records           */
static uint8_t  s_full;
static uint32_t s_overflow;
static uint8_t  s_page[S25FL128S_PAGE_SIZE]; /* RAM staging page (256 B)    */
static uint32_t s_page_fill;                 /* bytes buffered in s_page    */
static uint32_t s_session_id;
static uint32_t s_header_count;

static inline void flash_lock(void) { osMutexAcquire(flashMutexHandle, osWaitForever); }
static inline void flash_unlock(void) { osMutexRelease(flashMutexHandle); }

/* ---- CRC-16/CCITT (poly 0x1021, init 0xFFFF) — GIVEN, do not change -------*/
uint16_t log_crc16(const uint8_t* data, uint32_t len) {
    uint16_t crc = 0xFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int b = 0; b < 8; b++)
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
    }
    return crc;
}

/* ---- init: get the position from the header (STUDENT TODO: full recovery) -*/
int32_t log_store_init(void) {
    /* STUDENT TODO (spec R20): scan the slots of the header sector for the
     * LAST slot that has a correct `magic` value and a correct crc16. The
     * starter gives that layout (log_header_t and HDR_SLOT_* above). Get
     * s_write_off, s_count, and s_full from that slot, so the log CONTINUES
     * across a reset and does not start again at 0. The starter boots as if
     * the log is empty. */
    // s_write_off = 0u;
    // s_count     = 0u;
    // s_full      = 0u;
    // s_overflow  = 0u;
    // s_page_fill = 0u;

    int32_t result = LOG_OK;

    // holding flash mutex for longer than necessary but this is init function, should only run once
    flash_lock();
    for (int32_t i = HDR_SLOT_COUNT - 1; i >= 0; i--) {
        log_header_t header;
        result = S25FL128S_Read(HDR_SLOT_ADDR(i), (uint8_t*)&header, HEADER_SIZE);
        if (result != S25FL128S_OK) {
            result = LOG_ERR;
            break;
        }

        if (header.magic != LOG_HDR_MAGIC) {
            continue;
        }

        uint16_t crc = log_crc16((const uint8_t*)&header, HEADER_SIZE - CRC_SIZE);
        if (crc != header.crc16) {
            continue;
        }

        s_session_id   = header.session_id;
        s_write_off    = header.write_off;
        s_count        = header.record_count;
        s_full         = header.log_full;
        s_header_count = i + 1;
        break;
    }
    flash_unlock();

    return result;
}

/* ---- FORMAT: the ONLY erase path (GIVEN — shows the discipline, spec R19) --*/
int32_t log_format(void) {
    int32_t rc = LOG_OK;
    flash_lock();
    /* Erase the header and all data sectors. Each erase blocks for up to
     * about 650 ms. That cost is the cause of the CONFIG-only,
     * operator-announced FORMAT command. */
    int32_t rc_header = LOG_OK;
    if (S25FL128S_EraseSector4K(HEADER_ADDR) != S25FL128S_OK) {
        rc_header = LOG_ERR;
    }
    for (uint32_t i = 0; rc_header == LOG_OK && i < DATA_SECTORS; i++) {
        if (S25FL128S_EraseSector64K(DATA_BASE + i * S25FL128S_SECTOR_64K) != S25FL128S_OK) {
            rc = LOG_ERR;
        }
    }
    flash_unlock();

    if (rc == LOG_OK) {
        s_write_off = 0u;
        s_count     = 0u;
        s_full      = 0u;
        s_overflow  = 0u;
        s_page_fill = 0u;
    }
    /* STUDENT TODO: page-program a new log_header_t into slot 0 (the `magic`
     * value, a new session_id, zero counts, and crc16). That is the first
     * turn of the rotating slot (R20). Each header refresh after that takes
     * the NEXT free slot. */

    if (rc != LOG_OK) {
        return LOG_ERR;
    }

    s_session_id++;
    s_header_count = 0;

    log_header_t header = { .magic        = LOG_HDR_MAGIC,
                            .session_id   = s_session_id,
                            .write_off    = s_write_off,
                            .record_count = s_count,
                            .log_full     = s_full };
    uint16_t     crc    = log_crc16((const uint8_t*)&header, HEADER_SIZE - CRC_SIZE);
    header.crc16        = crc;

    flash_lock();
    int32_t result = S25FL128S_PageProgram(HDR_SLOT_ADDR(s_header_count),
                                           (const uint8_t*)&header,
                                           HDR_SLOT_SIZE);
    flash_unlock();

    if (result != S25FL128S_OK) {
        return LOG_ERR;
    }

    s_header_count++;

    return rc;
}

int32_t log_flush() {
    if (s_page_fill == 0) {
        return LOG_OK;
    }
    if (s_full) {
        // s_overflow++;
        return LOG_FULL;
    }
    if (s_header_count >= HDR_SLOT_COUNT) {
        // goto cleanup_s_page_fill;
        return LOG_HEADER_FULL;
    }

    flash_lock();
    int32_t result = S25FL128S_PageProgram(DATA_BASE + s_write_off, s_page, s_page_fill);
    flash_unlock();

    if (result != S25FL128S_OK) {
        return LOG_ERR;
    }

    s_count += s_page_fill / RECORD_SIZE;
    s_write_off += s_page_fill;
    s_page_fill = 0;

    if (s_write_off == DATA_SIZE) {
        s_full = 1;
    }

    log_header_t header = { .magic        = LOG_HDR_MAGIC,
                            .session_id   = s_session_id,
                            .write_off    = s_write_off,
                            .record_count = s_count,
                            .log_full     = s_full };
    uint16_t     crc    = log_crc16((const uint8_t*)&header, HEADER_SIZE - CRC_SIZE);
    header.crc16        = crc;

    flash_lock();
    result = S25FL128S_PageProgram(HDR_SLOT_ADDR(s_header_count),
                                   (const uint8_t*)&header,
                                   HDR_SLOT_SIZE);
    flash_unlock();

    if (result != S25FL128S_OK) {
        return LOG_ERR;
    }

    s_header_count++;

    return LOG_OK;
}

/* ---- append: STUDENT TODO (the heart of the logger, spec R18/R21) ---------*/
int32_t log_append(const log_record_t* rec) {
    // (void)rec;
    // (void)s_page;
    // (void)s_page_fill;
    /* STUDENT TODO:
     *  1. If s_full, s_overflow++ and return LOG_FULL (do NOT overwrite — R21).
     *  2. Write rec->crc16 with log_crc16 over the first 14 bytes FIRST, then
     *     copy rec into s_page at s_page_fill, then add 16 to s_page_fill.
     *  3. When s_page_fill == 256, run this sequence:
     *       flash_lock()
     *       S25FL128S_PageProgram(DATA_BASE + s_write_off, s_page, 256)
     *       flash_unlock()
     *     Then s_write_off += 256, s_page_fill = 0, and refresh the header.
     *  4. If s_write_off gets to DATA_SIZE, set s_full.
     *  5. s_count++.
     * Hold the mutex ONLY around the PageProgram call (about 750 us maximum).
     * Do not hold it across the full function (spec R6/R9). */

    if (s_full) {
        s_overflow++;
        return LOG_FULL;
    }
    if (s_header_count >= HDR_SLOT_COUNT) {
        // goto cleanup_s_page_fill;
        s_overflow++;
        return LOG_HEADER_FULL;
    }

    log_record_t record = *rec;

    uint16_t crc = log_crc16((const uint8_t*)&record, RECORD_SIZE - CRC_SIZE);
    record.crc16 = crc;

    uint32_t remaining_page = S25FL128S_PAGE_SIZE - (s_write_off % S25FL128S_PAGE_SIZE);
    if (s_page_fill + RECORD_SIZE > remaining_page) {
        return LOG_ERR;
    }

    memcpy(&s_page[s_page_fill], &record, RECORD_SIZE);
    s_page_fill += RECORD_SIZE;

    if (s_page_fill == remaining_page) {
        return log_flush();
    }

    return LOG_OK;
}

/* ---- read-back for DOWNLOAD: STUDENT TODO --------------------------------*/
int32_t log_read_record(uint32_t index, log_record_t* out) {
    // (void)index;
    // (void)out;
    /* STUDENT TODO:
     *  1. Bounds-check that index is less than s_count.
     *  2. flash_lock(), then S25FL128S_Read(DATA_BASE + index*16, out, 16),
     *     then flash_unlock().
     *  3. Compare out->crc16 against log_crc16(out, 14). Flag a mismatch, and
     *     do not drop the record (spec R15).
     * Remember that DOWNLOAD reads in CHUNKS and releases the mutex between
     * them (spec R9). That release is what lets the logging continue during
     * a download. */
    if (index >= s_count) {
        return LOG_ERR;
    }

    int32_t result;
    flash_lock();
    result = S25FL128S_Read(RECORD_SLOT_ADDR(index), (uint8_t*)out, RECORD_SIZE);
    flash_unlock();

    if (result != S25FL128S_OK) {
        return LOG_ERR;
    }

    uint16_t crc = log_crc16((const uint8_t*)out, RECORD_SIZE - CRC_SIZE);
    if (crc != out->crc16) {
        // TODO flag mismatch
        return LOG_CRC_ERR;
    }

    return LOG_OK;
}

void log_store_status(log_status_t* out) {
    out->record_count     = s_count;
    out->capacity_records = CAPACITY_RECORDS;
    out->log_full         = s_full;
    out->overflow_count   = s_overflow;
}
