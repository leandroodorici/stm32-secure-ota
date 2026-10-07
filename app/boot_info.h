#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include <stdint.h>

#include "flash_if.h"

// What the application knows about how it was booted: the slot it runs from,
// and that slot's entry in the newest boot record. Read once at start-up,
// before the scheduler runs.
//
// Every field is a full word so the structure can be read over SWD with a
// single mdw.
typedef struct {
    uint32_t slot;         // 0 = slot A, 1 = slot B
    uint32_t record_found; // 0 if no valid boot record exists
    uint32_t seq;          // of the newest record
    uint32_t state;        // slot_state_t of this slot
    uint32_t trial_attempts;
    uint32_t primary_slot;
    uint32_t security_floor;
    uint32_t last_result;
} boot_info_t;

void boot_info_load(const flash_if_t *flash, boot_info_t *out);

#endif // BOOT_INFO_H
