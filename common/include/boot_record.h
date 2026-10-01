#ifndef BOOT_RECORD_H
#define BOOT_RECORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "flash_if.h"

// Persistent boot state, stored as log-structured snapshots.
//
// See docs/DESIGN.md section 6. The G4 cannot reprogram a double-word that is
// not erased, so state cannot be updated in place. Each change appends a full
// 32-byte snapshot and the newest valid record wins. Recovery is a scan, with
// no event replay to get wrong.

#define BR_MAGIC 0x31524F42u // "BOR1"
#define BR_SIZE 32u
#define BR_PER_PAGE (FLASH_PAGE_SIZE / BR_SIZE)

// Written as the final double-word of a record. Its presence is a cheap early
// reject for wholly erased entries. The CRC is what actually guarantees a torn
// record is caught. This is a second line of defence.
#define BR_COMMIT_PATTERN 0xC0FFEE0042424242ull

#define BR_SLOT_COUNT 2
#define BR_MAX_TRIALS 3

typedef enum {
    SLOT_EMPTY = 0,     // no image, or erase in progress: never boot
    SLOT_PENDING = 1,   // image written and verified, awaiting trial
    SLOT_TRIAL = 2,     // booted but not yet confirmed healthy
    SLOT_CONFIRMED = 3, // proven healthy: safe fallback
    SLOT_REJECTED = 4   // failed verification or exhausted trials
} slot_state_t;

// Reason codes reported upstream and stored in last_result. Values are stable
// because they appear in logs and gateway status messages.
typedef enum {
    BR_RESULT_NONE = 0,
    BR_RESULT_OK = 1,
    BR_RESULT_BAD_MAGIC = 10,
    BR_RESULT_BAD_HEADER = 11,
    BR_RESULT_BAD_HASH = 12,
    BR_RESULT_BAD_SIGNATURE = 13,
    BR_RESULT_WRONG_SLOT = 14,
    BR_RESULT_ROLLBACK_BLOCKED = 15,
    BR_RESULT_NOT_CONFIRMED = 16,
    BR_RESULT_FLASH_ERROR = 17,
    // The crypto library failed for a reason other than a bad signature: a
    // fault in this device's own build, never a property of the image
    BR_RESULT_CRYPTO_ERROR = 18
} br_result_t;

typedef struct {
    uint32_t seq;
    uint8_t primary_slot;
    uint8_t slot_state[BR_SLOT_COUNT];
    uint8_t trial_attempts;
    uint32_t security_floor;
    uint32_t last_result;
} boot_record_t;

typedef struct {
    const flash_if_t *flash;
    uint32_t page_addr[2];
    int cur_page;      // -1 when no valid record exists
    uint32_t cur_slot; // index of the newest record within that page
} br_ctx_t;

void boot_record_init(br_ctx_t *ctx, const flash_if_t *flash, uint32_t page_a,
                      uint32_t page_b);

// Scans both pages for the highest-seq committed record passing CRC. Returns
// false on a device with no valid record: factory-fresh, or fully corrupted.
bool boot_record_load(br_ctx_t *ctx, boot_record_t *out);

// Appends a snapshot with seq = previous + 1, rotating pages when the current
// one is full.
flash_status_t boot_record_append(br_ctx_t *ctx, const boot_record_t *rec);

// Writes the first record on a blank device.
flash_status_t boot_record_factory_init(br_ctx_t *ctx, const boot_record_t *rec);

// Exposed for unit testing: the on-flash byte layout must be verifiable
// directly, not only through the flash interface.
void boot_record_serialize(const boot_record_t *rec, uint8_t buf[BR_SIZE]);
bool boot_record_deserialize(const uint8_t buf[BR_SIZE], boot_record_t *out);

uint32_t br_crc32(const uint8_t *data, size_t len);

#endif // BOOT_RECORD_H