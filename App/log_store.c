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

#include "cmsis_os2.h"
#include <string.h>

#include "s25fl128s.h"
#include "stm32wbxx_hal.h"

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
 * the sector. A checkpoint is written every four data pages; on boot the
 * records after the last checkpoint are scanned and recovered. This lets the
 * 128 header slots describe all 512 data pages without a hot-path erase.
 *
 * PROVIDED: the struct, the slot geometry, and the `magic` value.
 * STUDENT TODO: the recovery scan at boot, and the slot-rotation update. The
 * scan must find the LAST slot that has a correct CRC. An erased slot reads
 * as all-0xFF and thus cannot agree with the `magic` value. The update must
 * take the next free slot. */
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

/* One checkpoint per four 256-byte data pages makes the 4-KB header sector
 * cover the complete 128-KB data region. Slot 0 describes the freshly
 * formatted log and the final slot is reserved for the LOG FULL checkpoint. */
#define HDR_CHECKPOINT_PAGES   4u
#define HDR_CHECKPOINT_RECORDS \
    (HDR_CHECKPOINT_PAGES * S25FL128S_PAGE_SIZE / sizeof(log_record_t))

#define RECORD_SIZE         sizeof(log_record_t)
#define HEADER_SIZE         sizeof(log_header_t)
#define CRC_SIZE            sizeof(uint16_t)
#define RECORD_SLOT_ADDR(i) (DATA_BASE + (i) * RECORD_SIZE)

/* tasks.c makes both GR-A guards. A CMSIS mutex inherits priority in this
 * FreeRTOS adapter; a plain binary semaphore intentionally does not. */
extern osMutexId_t     flashMutexHandle;
extern osSemaphoreId_t flashSemaphoreHandle;
extern osThreadId_t    s_logger_thread_id;

/* ---- module state ---------------------------------------------------------*/
static uint32_t s_write_off; /* byte offset into data region*/
static uint32_t s_count;     /* committed records           */
static uint8_t  s_full;
static uint32_t s_overflow;
static uint8_t  s_page[S25FL128S_PAGE_SIZE]; /* RAM staging page (256 B)    */
static uint32_t s_page_fill;                 /* bytes buffered in s_page    */
static uint32_t s_session_id;
static uint32_t s_header_count;
static uint32_t s_persisted_count;
static volatile uint8_t s_gr_inheritance_enabled = 1u;
static log_gr_stats_t    s_gr_stats = { .inheritance_enabled = 1u };

typedef enum {
    FLASH_LOCK_INVALID = 0,
    FLASH_LOCK_MUTEX,
    FLASH_LOCK_SEMAPHORE,
} flash_lock_kind_t;

static void gr_record_wait(uint32_t wait_ms) {
    uint32_t bin;

    if (wait_ms == 0u)
        bin = 0u;
    else if (wait_ms == 1u)
        bin = 1u;
    else if (wait_ms < 5u)
        bin = 2u;
    else if (wait_ms < 10u)
        bin = 3u;
    else if (wait_ms < 20u)
        bin = 4u;
    else if (wait_ms < 50u)
        bin = 5u;
    else if (wait_ms < 100u)
        bin = 6u;
    else
        bin = 7u;

    s_gr_stats.bins[bin]++;
    s_gr_stats.sample_count++;
    s_gr_stats.total_wait_ms += wait_ms;
    if (wait_ms > s_gr_stats.max_wait_ms) s_gr_stats.max_wait_ms = wait_ms;
}

/* Return the exact object acquired so release remains correct even if a bad
 * caller attempts to change mode concurrently. tasks.c only permits a mode
 * change while CONFIG is quiescent, so the two guards never overlap. */
static flash_lock_kind_t flash_lock(void) {
    flash_lock_kind_t kind = s_gr_inheritance_enabled ? FLASH_LOCK_MUTEX
                                                      : FLASH_LOCK_SEMAPHORE;
    uint8_t measure = (osKernelGetState() == osKernelRunning) &&
                      (osThreadGetId() == s_logger_thread_id);
    uint32_t requested_ms = measure ? HAL_GetTick() : 0u;
    osStatus_t status;

    if (kind == FLASH_LOCK_MUTEX)
        status = osMutexAcquire(flashMutexHandle, osWaitForever);
    else
        status = osSemaphoreAcquire(flashSemaphoreHandle, osWaitForever);

    if (status != osOK) return FLASH_LOCK_INVALID;
    if (measure) gr_record_wait(HAL_GetTick() - requested_ms);
    return kind;
}

static osStatus_t flash_unlock(flash_lock_kind_t kind) {
    if (kind == FLASH_LOCK_MUTEX) return osMutexRelease(flashMutexHandle);
    if (kind == FLASH_LOCK_SEMAPHORE) return osSemaphoreRelease(flashSemaphoreHandle);
    return osErrorParameter;
}

void log_gr_set_inheritance(uint8_t enabled) {
    s_gr_inheritance_enabled       = enabled ? 1u : 0u;
    s_gr_stats.inheritance_enabled = s_gr_inheritance_enabled;
}

void log_gr_reset(void) {
    uint8_t enabled = s_gr_inheritance_enabled;
    memset(&s_gr_stats, 0, sizeof(s_gr_stats));
    s_gr_stats.inheritance_enabled = enabled;
}

void log_gr_get_stats(log_gr_stats_t* out) {
    if (out != NULL) *out = s_gr_stats;
}

static int32_t header_write_checkpoint(void) {
    if (s_header_count >= HDR_SLOT_COUNT) return LOG_HEADER_FULL;

    log_header_t header = { .magic        = LOG_HDR_MAGIC,
                            .session_id   = s_session_id,
                            .write_off    = s_write_off,
                            .record_count = s_count,
                            .log_full     = s_full };
    header.crc16 = log_crc16((const uint8_t*)&header, HEADER_SIZE - CRC_SIZE);

    flash_lock_kind_t lock = flash_lock();
    if (lock == FLASH_LOCK_INVALID) return LOG_ERR;

    int32_t result = S25FL128S_PageProgram(HDR_SLOT_ADDR(s_header_count),
                                           (const uint8_t*)&header,
                                           HDR_SLOT_SIZE);
    if (flash_unlock(lock) != osOK) return LOG_ERR;
    if (result != S25FL128S_OK) return LOG_ERR;

    s_header_count++;
    s_persisted_count = s_count;
    return LOG_OK;
}

static uint8_t record_is_erased(const log_record_t* record) {
    const uint8_t* bytes = (const uint8_t*)record;
    for (uint32_t i = 0u; i < RECORD_SIZE; i++) {
        if (bytes[i] != 0xFFu) return 0u;
    }
    return 1u;
}

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

    s_write_off       = 0u;
    s_count           = 0u;
    s_full            = 0u;
    s_overflow        = 0u;
    s_page_fill       = 0u;
    s_session_id      = 0u;
    s_header_count    = 0u;
    s_persisted_count = 0u;

    /* POST runs before the kernel and before the guards exist. It is the only
     * thread, so this recovery scan is inherently serialized. */
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
        if (crc != header.crc16 || header.write_off > DATA_SIZE ||
            header.record_count > CAPACITY_RECORDS ||
            header.write_off != header.record_count * RECORD_SIZE) {
            continue;
        }

        s_session_id   = header.session_id;
        s_write_off    = header.write_off;
        s_count        = header.record_count;
        s_full         = header.log_full;
        s_header_count = i + 1;
        s_persisted_count = s_count;
        break;
    }

    if (result != S25FL128S_OK) return LOG_ERR;

    /* Checkpoints deliberately lag the data by at most a few pages. Recover
     * complete records until the first erased or CRC-invalid slot. This also
     * lets logs made by the old per-page-checkpoint firmware continue past
     * its 2,032-record header limit. */
    while (!s_full && s_count < CAPACITY_RECORDS) {
        log_record_t record;
        result = S25FL128S_Read(RECORD_SLOT_ADDR(s_count),
                                (uint8_t*)&record,
                                RECORD_SIZE);
        if (result != S25FL128S_OK) return LOG_ERR;
        if (record_is_erased(&record)) break;
        if (record.crc16 != log_crc16((const uint8_t*)&record,
                                       RECORD_SIZE - CRC_SIZE)) {
            /* A torn/non-erased record cannot safely be programmed again on
             * NOR flash. Preserve the valid prefix and stop appending until
             * the operator performs FORMAT. */
            s_full = 1u;
            break;
        }

        s_count++;
        s_write_off += RECORD_SIZE;
    }

    if (s_count == CAPACITY_RECORDS) s_full = 1u;
    return result;
}

/* ---- FORMAT: the ONLY erase path (GIVEN — shows the discipline, spec R19) --*/
int32_t log_format(void) {
    int32_t rc = LOG_OK;
    flash_lock_kind_t lock = flash_lock();
    if (lock == FLASH_LOCK_INVALID) return LOG_ERR;
    /* Erase the header and all data sectors. Each erase blocks for up to
     * about 650 ms. That cost is the cause of the CONFIG-only,
     * operator-announced FORMAT command. */
    int32_t rc_header = LOG_OK;
    if (S25FL128S_EraseSector4K(HEADER_ADDR) != S25FL128S_OK) {
        rc_header = LOG_ERR;
        rc = LOG_ERR;
    }
    for (uint32_t i = 0; rc_header == LOG_OK && i < DATA_SECTORS; i++) {
        if (S25FL128S_EraseSector64K(DATA_BASE + i * S25FL128S_SECTOR_64K) != S25FL128S_OK) {
            rc = LOG_ERR;
        }
    }
    if (flash_unlock(lock) != osOK) rc = LOG_ERR;

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
    s_header_count    = 0u;
    s_persisted_count = 0u;

    return header_write_checkpoint();
}

int32_t log_flush() {
    if (s_page_fill == 0) {
        return LOG_OK;
    }
    if (s_full) {
        // s_overflow++;
        return LOG_FULL;
    }
    flash_lock_kind_t lock = flash_lock();
    if (lock == FLASH_LOCK_INVALID) return LOG_ERR;
    int32_t result = S25FL128S_PageProgram(DATA_BASE + s_write_off, s_page, s_page_fill);
    if (flash_unlock(lock) != osOK) return LOG_ERR;

    if (result != S25FL128S_OK) {
        return LOG_ERR;
    }

    s_count += s_page_fill / RECORD_SIZE;
    s_write_off += s_page_fill;
    s_page_fill = 0;

    if (s_write_off == DATA_SIZE) {
        s_full = 1;
    }

    /* Keep the last header slot available for the persisted LOG FULL state.
     * If an older firmware already consumed every slot, logging may still
     * continue: boot recovery scans forward from its final checkpoint. */
    uint8_t checkpoint_due =
        (s_count - s_persisted_count) >= HDR_CHECKPOINT_RECORDS;
    if (s_full || (checkpoint_due && s_header_count < (HDR_SLOT_COUNT - 1u))) {
        int32_t header_result = header_write_checkpoint();
        if (header_result != LOG_OK) {
            /* A legacy log can arrive here with all header slots already used.
             * Its data remains valid and boot scanning still finds the tail. */
            if (header_result != LOG_HEADER_FULL) return header_result;
        }
    }

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
    if (log_read_records(index, 1u, out) != LOG_OK) return LOG_ERR;

    uint16_t crc = log_crc16((const uint8_t*)out, RECORD_SIZE - CRC_SIZE);
    if (crc != out->crc16) {
        // TODO flag mismatch
        return LOG_CRC_ERR;
    }

    return LOG_OK;
}

int32_t log_read_records(uint32_t first, uint32_t count, log_record_t* out) {
    if (count == 0u) return LOG_OK;
    if (out == NULL || count > LOG_DOWNLOAD_CHUNK_RECORDS || first >= s_count ||
        count > (s_count - first)) {
        return LOG_ERR;
    }

    flash_lock_kind_t lock = flash_lock();
    if (lock == FLASH_LOCK_INVALID) return LOG_ERR;

    int32_t result = S25FL128S_Read(RECORD_SLOT_ADDR(first),
                                    (uint8_t*)out,
                                    count * RECORD_SIZE);
    if (flash_unlock(lock) != osOK) return LOG_ERR;

    return result == S25FL128S_OK ? LOG_OK : LOG_ERR;
}

void log_store_status(log_status_t* out) {
    out->record_count     = s_count;
    out->capacity_records = CAPACITY_RECORDS;
    out->log_full         = s_full;
    out->overflow_count   = s_overflow;
}
