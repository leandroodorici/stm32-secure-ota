#include "update_agent.h"

#include <string.h>

#include "flash_map.h"

// See update_agent.h

static upd_reply_t reply(uint8_t type, uint32_t value) {
    upd_reply_t r = {type, value};
    return r;
}

static uint32_t slot_base(uint8_t slot) {
    return slot == 0u ? SLOT_A_BASE : SLOT_B_BASE;
}

static flash_status_t save_progress(const upd_agent_t *a, uint32_t offset) {
    progress_t p;
    p.image_size = a->image_size;
    memcpy(p.digest, a->digest, PG_DIGEST_LEN);
    p.offset = offset;
    return progress_save(&a->progress, &p);
}

void upd_init(upd_agent_t *a, const flash_if_t *flash,
              const uint8_t pubkey[IMG_PUBKEY_LEN], uint8_t running_slot) {
    memset(a, 0, sizeof(*a));
    a->flash = flash;
    a->pubkey = pubkey;
    a->running_slot = running_slot;
    boot_record_init(&a->boot, flash, BOOT_STATE_PAGE_A, BOOT_STATE_PAGE_B);
    progress_init(&a->progress, flash, PROGRESS_PAGE_A, PROGRESS_PAGE_B);
}

upd_reply_t upd_offer(upd_agent_t *a, const proto_offer_t *o) {
    // A new offer replaces any transfer in progress
    a->active = 0;

    boot_record_t rec;
    if (!boot_record_load(&a->boot, &rec)) {
        return reply(MSG_REJECT, BR_RESULT_FLASH_ERROR);
    }
    if (rec.slot_state[a->running_slot] == SLOT_TRIAL) {
        return reply(MSG_REJECT, UPD_ERR_BUSY_TRIAL);
    }
    if (o->target_slot >= BR_SLOT_COUNT || o->target_slot == a->running_slot ||
        o->target_slot == rec.primary_slot) {
        return reply(MSG_REJECT, BR_RESULT_WRONG_SLOT);
    }
    if (o->image_size > SLOT_SIZE) {
        return reply(MSG_REJECT, UPD_ERR_TOO_LARGE);
    }
    if (o->image_size < IMG_HDR_SIZE + IMG_TRAILER_LEN) {
        return reply(MSG_REJECT, BR_RESULT_BAD_HEADER);
    }
    // Every chunk then starts on a double-word, the unit flash is programmed in
    if (o->chunk_size == 0u || o->chunk_size > PROTO_CHUNK_MAX ||
        o->chunk_size % FLASH_DW_SIZE != 0u) {
        return reply(MSG_REJECT, UPD_ERR_CHUNK_SIZE);
    }
    if (o->security_counter < rec.security_floor) {
        return reply(MSG_REJECT, BR_RESULT_ROLLBACK_BLOCKED);
    }

    a->target_slot = o->target_slot;
    a->chunk_size = o->chunk_size;
    a->image_size = o->image_size;
    memcpy(a->digest, o->digest, PG_DIGEST_LEN);

    // Resume if the log names this same image, and the slot has stayed EMPTY
    // since: the log only ever records pages that were completely written
    progress_t p;
    if (rec.slot_state[o->target_slot] == SLOT_EMPTY && progress_load(&a->progress, &p) &&
        p.image_size == o->image_size &&
        memcmp(p.digest, o->digest, PG_DIGEST_LEN) == 0 && p.offset <= o->image_size &&
        p.offset % FLASH_PAGE_SIZE == 0u) {
        a->next_offset = p.offset;
        a->active = 1;
        return reply(MSG_ACCEPT, p.offset);
    }

    // Start from zero. The slot is marked EMPTY before anything in it is
    // erased. The log is then pointed at this image, so that whatever it held
    // before can no longer be resumed into the pages about to be overwritten.
    if (rec.slot_state[o->target_slot] != SLOT_EMPTY) {
        boot_record_t next = rec;
        next.slot_state[o->target_slot] = SLOT_EMPTY;
        if (boot_record_append(&a->boot, &next) != FLASH_OK) {
            return reply(MSG_REJECT, BR_RESULT_FLASH_ERROR);
        }
    }
    if (save_progress(a, 0) != FLASH_OK) {
        return reply(MSG_REJECT, BR_RESULT_FLASH_ERROR);
    }
    a->next_offset = 0;
    a->active = 1;
    return reply(MSG_ACCEPT, 0);
}

// Erases every page that starts inside the chunk, then programs the chunk.
// Pages are erased as the transfer reaches them, rather than all at OFFER,
// so no single message takes the time to erase a whole slot.
static flash_status_t write_chunk(const upd_agent_t *a, const proto_chunk_t *c) {
    uint32_t base = slot_base(a->target_slot) + c->offset;
    uint32_t end = base + c->length;

    uint32_t page = (base + FLASH_PAGE_SIZE - 1u) / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE;
    for (; page < end; page += FLASH_PAGE_SIZE) {
        flash_status_t st = flash_erase_page(a->flash, page);
        if (st != FLASH_OK) {
            return st;
        }
    }

    // Only the image's last chunk can end partway through a double-word. The
    // rest of that double-word is programmed as erased bytes.
    for (uint32_t i = 0; i < c->length; i += FLASH_DW_SIZE) {
        uint8_t dw[FLASH_DW_SIZE];
        uint32_t n = c->length - i < FLASH_DW_SIZE ? c->length - i : FLASH_DW_SIZE;
        memset(dw, FLASH_ERASED_BYTE, sizeof(dw));
        memcpy(dw, c->data + i, n);
        uint64_t v = 0;
        for (uint32_t b = 0; b < FLASH_DW_SIZE; b++) {
            v |= (uint64_t)dw[b] << (8u * b);
        }
        flash_status_t st = flash_program_dw(a->flash, base + i, v);
        if (st != FLASH_OK) {
            return st;
        }
    }
    return FLASH_OK;
}

upd_reply_t upd_chunk(upd_agent_t *a, const proto_chunk_t *c) {
    if (!a->active) {
        return reply(MSG_REJECT, UPD_ERR_NO_SESSION);
    }
    if (c->offset != a->next_offset) {
        return reply(MSG_CHUNK_ACK, a->next_offset);
    }
    uint32_t left = a->image_size - a->next_offset;
    uint32_t expected = left < a->chunk_size ? left : a->chunk_size;
    if (expected == 0u || c->length != expected) {
        return reply(MSG_REJECT, UPD_ERR_CHUNK_SIZE);
    }
    if (br_crc32(c->data, c->length) != c->crc) {
        return reply(MSG_REJECT, UPD_ERR_CHUNK_CRC);
    }

    if (write_chunk(a, c) != FLASH_OK) {
        a->active = 0;
        return reply(MSG_ABORT, BR_RESULT_FLASH_ERROR);
    }
    uint32_t done = a->next_offset + c->length;

    // Recorded once per completed page, so the log is written about once
    // for every 2 KB received
    if (done / FLASH_PAGE_SIZE != a->next_offset / FLASH_PAGE_SIZE &&
        save_progress(a, done / FLASH_PAGE_SIZE * FLASH_PAGE_SIZE) != FLASH_OK) {
        a->active = 0;
        return reply(MSG_ABORT, BR_RESULT_FLASH_ERROR);
    }
    a->next_offset = done;
    return reply(MSG_CHUNK_ACK, done);
}

upd_reply_t upd_commit(upd_agent_t *a) {
    if (!a->active) {
        return reply(MSG_REJECT, UPD_ERR_NO_SESSION);
    }
    if (a->next_offset != a->image_size) {
        return reply(MSG_REJECT, UPD_ERR_INCOMPLETE);
    }
    a->active = 0;

    boot_record_t rec;
    if (!boot_record_load(&a->boot, &rec)) {
        return reply(MSG_VERIFY_RESULT, BR_RESULT_FLASH_ERROR);
    }

    image_header_t hdr;
    br_result_t result = image_verify(a->flash, slot_base(a->target_slot), SLOT_SIZE,
                                      a->pubkey, rec.security_floor, &hdr);

    // The signed image must be exactly what was sent. A header claiming more
    // would have been verified partly over bytes this transfer never wrote.
    if (result == BR_RESULT_OK &&
        IMG_HDR_SIZE + hdr.payload_size + IMG_TRAILER_LEN != a->image_size) {
        result = BR_RESULT_BAD_HEADER;
    }

    if (result != BR_RESULT_OK) {
        // Forget the progress, so a retry starts from zero instead of
        // resuming into the same bad data
        (void)save_progress(a, 0);
        return reply(MSG_VERIFY_RESULT, result);
    }

    boot_record_t next = rec;
    next.slot_state[a->target_slot] = SLOT_PENDING;
    next.last_result = BR_RESULT_OK;
    if (boot_record_append(&a->boot, &next) != FLASH_OK) {
        return reply(MSG_VERIFY_RESULT, BR_RESULT_FLASH_ERROR);
    }
    return reply(MSG_VERIFY_RESULT, BR_RESULT_OK);
}

void upd_abort(upd_agent_t *a) {
    a->active = 0;
}

br_result_t upd_confirm(upd_agent_t *a, uint32_t image_counter) {
    boot_record_t rec;
    if (!boot_record_load(&a->boot, &rec)) {
        return BR_RESULT_FLASH_ERROR;
    }
    if (rec.slot_state[a->running_slot] == SLOT_CONFIRMED) {
        return BR_RESULT_OK;
    }
    if (rec.slot_state[a->running_slot] != SLOT_TRIAL) {
        return BR_RESULT_NONE;
    }

    // One record, so a power cut leaves either the trial or all of this. The
    // other slot keeps its state: a confirmed image there stays a fallback,
    // and from now on the floor decides whether it can still boot.
    boot_record_t next = rec;
    next.slot_state[a->running_slot] = SLOT_CONFIRMED;
    next.primary_slot = a->running_slot;
    if (image_counter > next.security_floor) {
        next.security_floor = image_counter;
    }
    next.last_result = BR_RESULT_OK;
    if (boot_record_append(&a->boot, &next) != FLASH_OK) {
        return BR_RESULT_FLASH_ERROR;
    }
    return BR_RESULT_OK;
}
