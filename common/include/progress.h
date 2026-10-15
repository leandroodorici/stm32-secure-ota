#ifndef PROGRESS_H
#define PROGRESS_H

#include <stdint.h>

#include "flash_if.h"

// Progress of an image transfer, so an interrupted one can resume
// (docs/DESIGN.md section 9).
//
// Each time a page of the target slot has been completely written, the update
// agent appends a record naming the image and how far it got. After a reset,
// the newest valid record tells the agent where to resume, provided the
// gateway offers the same image again. The image is identified by its size
// and the first 8 bytes of its SHA-256. That is enough to tell images apart;
// it is not a security check, since the full image is verified at COMMIT.
//
// The storage pattern is the boot records' (section 6): append-only 32-byte
// records, a CRC over the contents, the commit word in the last double-word,
// and two pages used in turn. It is kept separate from boot_record.c rather
// than generalising it, because boot_record.c is part of the bootloader,
// which can never be updated, and its tested behaviour should not change to
// serve the application.

#define PG_MAGIC 0x31475250u // "PRG1"
#define PG_SIZE 32u
#define PG_PER_PAGE (FLASH_PAGE_SIZE / PG_SIZE)
#define PG_DIGEST_LEN 8u

typedef struct {
    uint32_t image_size;
    uint8_t digest[PG_DIGEST_LEN]; // first bytes of the image's SHA-256
    uint32_t offset;               // bytes durably written from the start
} progress_t;

typedef struct {
    const flash_if_t *flash;
    uint32_t page_addr[2];
} progress_log_t;

void progress_init(progress_log_t *log, const flash_if_t *flash, uint32_t page_a,
                   uint32_t page_b);

// Returns 1 and the newest valid record, or 0 if there is none
uint32_t progress_load(const progress_log_t *log, progress_t *out);

// Appends a record, moving to the other page when the current one is full
flash_status_t progress_save(const progress_log_t *log, const progress_t *p);

#endif // PROGRESS_H
