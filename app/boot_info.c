#include "boot_info.h"

#include <string.h>

#include "FreeRTOS.h"

#include "boot_handoff.h"
#include "boot_record.h"
#include "flash_map.h"
#include "image_verify.h"

// The application's vector table, from startup.c. Its address is fixed when
// the image is linked for a slot, so it says which slot this image belongs
// to without trusting anything read at run time.
extern const void *const vector_table[];

void boot_info_load(const flash_if_t *flash, boot_info_t *out) {
    memset(out, 0, sizeof(*out));

    uint32_t vectors = (uint32_t)(uintptr_t)vector_table;
    // The bootloader only jumps to slot base + 0x200. Anything else means
    // the image was linked for a layout this code does not know.
    configASSERT(vectors == SLOT_A_BASE + IMG_HDR_SIZE ||
                 vectors == SLOT_B_BASE + IMG_HDR_SIZE);
    out->slot = (vectors == SLOT_B_BASE + IMG_HDR_SIZE) ? 1u : 0u;

    // The bootloader verified this header before jumping, so its fields can
    // be trusted. Version and counter sit at offsets 16 and 24.
    uint8_t hdr[32];
    uint32_t base = (out->slot == 1u) ? SLOT_B_BASE : SLOT_A_BASE;
    if (flash_read(flash, base, hdr, sizeof(hdr)) == FLASH_OK) {
        out->image_version = (uint32_t)hdr[16] | ((uint32_t)hdr[17] << 8) |
                             ((uint32_t)hdr[18] << 16) | ((uint32_t)hdr[19] << 24);
        out->image_counter = (uint32_t)hdr[24] | ((uint32_t)hdr[25] << 8) |
                             ((uint32_t)hdr[26] << 16) | ((uint32_t)hdr[27] << 24);
    }

    if (BOOT_HANDOFF->magic == BOOT_HANDOFF_MAGIC) {
        out->reset_flags = BOOT_HANDOFF->reset_flags;
    }

    br_ctx_t ctx;
    boot_record_t rec;
    boot_record_init(&ctx, flash, BOOT_STATE_PAGE_A, BOOT_STATE_PAGE_B);
    if (!boot_record_load(&ctx, &rec)) {
        // Possible only if the image was started some other way, such as a
        // debugger: the bootloader never jumps without a record
        return;
    }

    out->record_found = 1u;
    out->seq = rec.seq;
    out->state = rec.slot_state[out->slot];
    out->trial_attempts = rec.trial_attempts;
    out->primary_slot = rec.primary_slot;
    out->security_floor = rec.security_floor;
    out->last_result = rec.last_result;
    out->other_state = rec.slot_state[out->slot ^ 1u];
}
