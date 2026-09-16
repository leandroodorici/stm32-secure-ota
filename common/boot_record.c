#include "boot_record.h"

#include <string.h>

// See boot_record.h and docs/DESIGN.md section 6.
//
// ---- CRC-32 ----
//
// Implemented here rather than taken from a library: the bootloader links no
// hosted libc, and the STM32 CRC peripheral is not available before clock
// setup. Table-free because this runs on 20 bytes at a time, so the bitwise
// version costs nothing that matters and saves 1 KB of table in code that has
// a size budget.
uint32_t br_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

// ---- little-endian helpers ----
//
// Explicit byte packing keeps the on-flash layout independent of compiler
// padding and host endianness, so host tests exercise the same bytes the target
// will see.
static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t get_u64(const uint8_t *p) {
    return (uint64_t)get_u32(p) | ((uint64_t)get_u32(p + 4) << 32);
}

// ---- serialization ----
//
// Byte layout, 32 bytes = 4 double-words, matching docs/DESIGN.md section 6:
//
//    0  magic            DW0
//    4  seq
//    8  primary_slot     DW1
//    9  slot_state[0]
//   10  slot_state[1]
//   11  trial_attempts
//   12  security_floor
//   16  last_result      DW2
//   20  crc32            covers bytes 0..19
//   24  commit           DW3, programmed last
#define BR_CRC_COVERAGE 20u

void boot_record_serialize(const boot_record_t *rec, uint8_t buf[BR_SIZE]) {
    memset(buf, 0, BR_SIZE);
    put_u32(buf + 0, BR_MAGIC);
    put_u32(buf + 4, rec->seq);
    buf[8] = rec->primary_slot;
    buf[9] = rec->slot_state[0];
    buf[10] = rec->slot_state[1];
    buf[11] = rec->trial_attempts;
    put_u32(buf + 12, rec->security_floor);
    put_u32(buf + 16, rec->last_result);
    put_u32(buf + 20, br_crc32(buf, BR_CRC_COVERAGE));
    put_u32(buf + 24, (uint32_t)(BR_COMMIT_PATTERN & 0xFFFFFFFFu));
    put_u32(buf + 28, (uint32_t)(BR_COMMIT_PATTERN >> 32));
}

bool boot_record_deserialize(const uint8_t buf[BR_SIZE], boot_record_t *out) {
    if (get_u32(buf + 0) != BR_MAGIC) {
        return false;
    }
    // Checked before the CRC because it is a cheaper reject for the common
    // case: the next free entry, still entirely 0xFF.
    if (get_u64(buf + 24) != BR_COMMIT_PATTERN) {
        return false;
    }
    if (get_u32(buf + 20) != br_crc32(buf, BR_CRC_COVERAGE)) {
        return false;
    }

    out->seq = get_u32(buf + 4);
    out->primary_slot = buf[8];
    out->slot_state[0] = buf[9];
    out->slot_state[1] = buf[10];
    out->trial_attempts = buf[11];
    out->security_floor = get_u32(buf + 12);
    out->last_result = get_u32(buf + 16);

    // Reject structurally impossible values rather than trusting flash. A
    // record that passes CRC but names slot 7 means our own writer has a bug,
    // and booting on it would be worse than falling back.
    if (out->primary_slot >= BR_SLOT_COUNT) {
        return false;
    }
    if (out->slot_state[0] > SLOT_REJECTED || out->slot_state[1] > SLOT_REJECTED) {
        return false;
    }
    return true;
}

// ---- page scanning ----

static bool entry_is_erased(const uint8_t buf[BR_SIZE]) {
    for (size_t i = 0; i < BR_SIZE; i++) {
        if (buf[i] != FLASH_ERASED_BYTE) {
            return false;
        }
    }
    return true;
}

// Scans one page for its newest valid record, and reports the first free entry
// via *free_idx (BR_PER_PAGE when the page is full).
//
// The scan is exhaustive rather than stopping at the first erased entry. A cut
// can leave an ECC-damaged entry mid-page, and since damaged entries cannot be
// rewritten, valid records may exist after it. Stopping early would hide the
// newest state and silently roll the device back.
static bool scan_page(const br_ctx_t *ctx, int page, boot_record_t *out,
                      uint32_t *slot_idx, uint32_t *free_idx) {
    bool found = false;
    boot_record_t best;
    uint32_t best_idx = 0;
    uint32_t first_free = BR_PER_PAGE;

    for (uint32_t i = 0; i < BR_PER_PAGE; i++) {
        uint8_t buf[BR_SIZE];
        uint32_t addr = ctx->page_addr[page] + i * BR_SIZE;
        flash_status_t st = flash_read(ctx->flash, addr, buf, BR_SIZE);

        if (st == FLASH_ERR_ECC) {
            // A torn double-word. The entry is unusable and, because we never
            // reprogram, so is the space. Treat it as occupied and continue.
            continue;
        }
        if (st != FLASH_OK) {
            continue;
        }
        if (entry_is_erased(buf)) {
            if (first_free == BR_PER_PAGE) {
                first_free = i;
            }
            continue;
        }

        boot_record_t rec;
        if (!boot_record_deserialize(buf, &rec)) {
            continue; // partial or corrupt: ignore, the space is spent
        }
        // Sequence numbers only grow, so a strictly greater seq is newer.
        // Wraparound is not handled: at one record per state change, 2^32
        // records would outlast the flash by orders of magnitude.
        if (!found || rec.seq > best.seq) {
            best = rec;
            best_idx = i;
            found = true;
        }
    }

    if (free_idx) {
        *free_idx = first_free;
    }
    if (found) {
        *out = best;
        if (slot_idx) {
            *slot_idx = best_idx;
        }
    }
    return found;
}

// ---- public API ----

void boot_record_init(br_ctx_t *ctx, const flash_if_t *flash, uint32_t page_a,
                      uint32_t page_b) {
    ctx->flash = flash;
    ctx->page_addr[0] = page_a;
    ctx->page_addr[1] = page_b;
    ctx->cur_page = -1;
    ctx->cur_slot = 0;
}

bool boot_record_load(br_ctx_t *ctx, boot_record_t *out) {
    boot_record_t rec[2];
    uint32_t idx[2];
    bool ok[2];

    for (int p = 0; p < 2; p++) {
        ok[p] = scan_page(ctx, p, &rec[p], &idx[p], NULL);
    }

    if (!ok[0] && !ok[1]) {
        ctx->cur_page = -1;
        return false;
    }

    int winner;
    if (ok[0] && ok[1]) {
        winner = (rec[1].seq > rec[0].seq) ? 1 : 0;
    } else {
        winner = ok[0] ? 0 : 1;
    }

    ctx->cur_page = winner;
    ctx->cur_slot = idx[winner];
    *out = rec[winner];
    return true;
}

// Programs a 32-byte record into one page entry, commit word last.
static flash_status_t write_entry(const br_ctx_t *ctx, int page, uint32_t slot,
                                  const boot_record_t *rec) {
    uint8_t buf[BR_SIZE];
    boot_record_serialize(rec, buf);
    uint32_t base = ctx->page_addr[page] + slot * BR_SIZE;

    for (uint32_t dw = 0; dw < 4u; dw++) {
        uint64_t v = get_u64(buf + dw * FLASH_DW_SIZE);
        flash_status_t st = flash_program_dw(ctx->flash, base + dw * FLASH_DW_SIZE, v);
        if (st != FLASH_OK) {
            return st;
        }
    }
    return FLASH_OK;
}

flash_status_t boot_record_append(br_ctx_t *ctx, const boot_record_t *rec) {
    boot_record_t current;
    boot_record_t to_write = *rec;

    if (!boot_record_load(ctx, &current)) {
        // Nothing valid on the device. The caller should have used
        // boot_record_factory_init, but handle it rather than corrupting.
        return boot_record_factory_init(ctx, rec);
    }
    to_write.seq = current.seq + 1u;

    boot_record_t tmp;
    uint32_t free_idx = BR_PER_PAGE;
    (void)scan_page(ctx, ctx->cur_page, &tmp, NULL, &free_idx);

    if (free_idx < BR_PER_PAGE) {
        return write_entry(ctx, ctx->cur_page, free_idx, &to_write);
    }

    // Page full: rotate. Erasing the other page is safe at any instant because
    // the newest record lives in the current page and stays there. If power is
    // lost during the erase, the next boot finds the current page and simply
    // retries the rotation.
    int other = ctx->cur_page ^ 1;
    flash_status_t st = flash_erase_page(ctx->flash, ctx->page_addr[other]);
    if (st != FLASH_OK) {
        return st;
    }
    st = write_entry(ctx, other, 0, &to_write);
    if (st != FLASH_OK) {
        return st;
    }
    ctx->cur_page = other;
    ctx->cur_slot = 0;
    return FLASH_OK;
}

flash_status_t boot_record_factory_init(br_ctx_t *ctx, const boot_record_t *rec) {
    boot_record_t first = *rec;
    first.seq = 1u;

    // Erase both pages so the device starts from a known state. Order is
    // irrelevant here: by definition there is no record worth preserving.
    for (int p = 0; p < 2; p++) {
        flash_status_t st = flash_erase_page(ctx->flash, ctx->page_addr[p]);
        if (st != FLASH_OK) {
            return st;
        }
    }
    flash_status_t st = write_entry(ctx, 0, 0, &first);
    if (st != FLASH_OK) {
        return st;
    }
    ctx->cur_page = 0;
    ctx->cur_slot = 0;
    return FLASH_OK;
}