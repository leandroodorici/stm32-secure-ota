#include "progress.h"

#include <string.h>

#include "boot_record.h"

// See progress.h. Record layout, 32 bytes = 4 double-words:
//
//    0  magic        4  seq          DW0
//    8  image size  12  offset       DW1
//   16  digest, 8 bytes              DW2
//   24  CRC-32 over bytes 0..23
//   28  commit word                  DW3, programmed last
//
// A record is valid only if magic, commit word and CRC all match. A double-
// word that failed to program reads as 0xFF or faults, so a torn record is
// always rejected, whichever double-word was interrupted.

#define PG_COMMIT 0x54494D43u // "CMIT"
#define PG_CRC_COVERAGE 24u

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void serialize(uint32_t seq, const progress_t *p, uint8_t buf[PG_SIZE]) {
    put_u32(buf + 0, PG_MAGIC);
    put_u32(buf + 4, seq);
    put_u32(buf + 8, p->image_size);
    put_u32(buf + 12, p->offset);
    memcpy(buf + 16, p->digest, PG_DIGEST_LEN);
    put_u32(buf + 24, br_crc32(buf, PG_CRC_COVERAGE));
    put_u32(buf + 28, PG_COMMIT);
}

static uint32_t deserialize(const uint8_t buf[PG_SIZE], uint32_t *seq, progress_t *p) {
    if (get_u32(buf + 0) != PG_MAGIC || get_u32(buf + 28) != PG_COMMIT ||
        get_u32(buf + 24) != br_crc32(buf, PG_CRC_COVERAGE)) {
        return 0;
    }
    *seq = get_u32(buf + 4);
    p->image_size = get_u32(buf + 8);
    p->offset = get_u32(buf + 12);
    memcpy(p->digest, buf + 16, PG_DIGEST_LEN);
    return 1;
}

static uint32_t is_erased(const uint8_t buf[PG_SIZE]) {
    for (uint32_t i = 0; i < PG_SIZE; i++) {
        if (buf[i] != FLASH_ERASED_BYTE) {
            return 0;
        }
    }
    return 1;
}

typedef struct {
    uint32_t found;
    uint32_t page; // where the newest record lives
    uint32_t seq;
    progress_t rec;
} newest_t;

// Scans both pages for the record with the highest seq. Entries that fail to
// read, such as a double-word left torn by a power cut, are skipped: they can
// never be rewritten, but valid records may still follow them.
static void find_newest(const progress_log_t *log, newest_t *n) {
    memset(n, 0, sizeof(*n));
    for (uint32_t page = 0; page < 2u; page++) {
        for (uint32_t i = 0; i < PG_PER_PAGE; i++) {
            uint8_t buf[PG_SIZE];
            if (flash_read(log->flash, log->page_addr[page] + i * PG_SIZE, buf,
                           sizeof(buf)) != FLASH_OK) {
                continue;
            }
            uint32_t seq;
            progress_t rec;
            if (deserialize(buf, &seq, &rec) && (!n->found || seq > n->seq)) {
                n->found = 1;
                n->page = page;
                n->seq = seq;
                n->rec = rec;
            }
        }
    }
}

// Returns the index of the first fully erased entry in a page, or
// PG_PER_PAGE if there is none
static uint32_t first_free(const progress_log_t *log, uint32_t page) {
    for (uint32_t i = 0; i < PG_PER_PAGE; i++) {
        uint8_t buf[PG_SIZE];
        if (flash_read(log->flash, log->page_addr[page] + i * PG_SIZE, buf,
                       sizeof(buf)) == FLASH_OK &&
            is_erased(buf)) {
            return i;
        }
    }
    return PG_PER_PAGE;
}

static flash_status_t write_entry(const progress_log_t *log, uint32_t page,
                                  uint32_t index, uint32_t seq, const progress_t *p) {
    uint8_t buf[PG_SIZE];
    serialize(seq, p, buf);
    uint32_t base = log->page_addr[page] + index * PG_SIZE;

    // In address order, so the commit word in the last double-word is
    // programmed last
    for (uint32_t dw = 0; dw < PG_SIZE / FLASH_DW_SIZE; dw++) {
        uint64_t v = (uint64_t)get_u32(buf + dw * 8u) |
                     ((uint64_t)get_u32(buf + dw * 8u + 4u) << 32);
        flash_status_t st = flash_program_dw(log->flash, base + dw * FLASH_DW_SIZE, v);
        if (st != FLASH_OK) {
            return st;
        }
    }
    return FLASH_OK;
}

void progress_init(progress_log_t *log, const flash_if_t *flash, uint32_t page_a,
                   uint32_t page_b) {
    log->flash = flash;
    log->page_addr[0] = page_a;
    log->page_addr[1] = page_b;
}

uint32_t progress_load(const progress_log_t *log, progress_t *out) {
    newest_t n;
    find_newest(log, &n);
    if (n.found) {
        *out = n.rec;
    }
    return n.found;
}

flash_status_t progress_save(const progress_log_t *log, const progress_t *p) {
    newest_t n;
    find_newest(log, &n);

    uint32_t seq = n.found ? n.seq + 1u : 1u;
    uint32_t page = n.found ? n.page : 0u;
    uint32_t index = first_free(log, page);
    if (index < PG_PER_PAGE) {
        return write_entry(log, page, index, seq, p);
    }

    // The page is full. Erase the other one and continue there. The newest
    // record stays where it is until the new one is complete, so a power cut
    // during the erase or the write leaves it readable, and the next save
    // simply repeats the move. With no valid record at all, the page in use
    // is page 0 holding only damage, and erasing it loses nothing.
    uint32_t other = n.found ? (page ^ 1u) : 0u;
    flash_status_t st = flash_erase_page(log->flash, log->page_addr[other]);
    if (st != FLASH_OK) {
        return st;
    }
    return write_entry(log, other, 0, seq, p);
}
