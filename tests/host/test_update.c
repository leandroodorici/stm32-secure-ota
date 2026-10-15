#include <stdio.h>
#include <string.h>

#include "flash_map.h"
#include "mbedtls/sha256.h"
#include "sim_flash.h"
#include "test_util.h"
#include "update_agent.h"

// The update agent against the real flash map. The node runs from slot A,
// which is CONFIRMED and primary; slot B holds an older CONFIRMED image. Each
// test plays the gateway, sending signed images built with the throwaway key.
//
// Beyond the protocol itself, these check what must hold however a transfer
// ends: nothing outside the boot state, slot B and the progress log is ever
// written; slot B is never left marked bootable unless it verifies; and an
// interrupted transfer always completes when the gateway tries again.

#ifndef UPDATE_IMAGE_DIR
#error "UPDATE_IMAGE_DIR must be defined by the build"
#endif
#ifndef TEST_IMAGE_DIR
#error "TEST_IMAGE_DIR must be defined by the build"
#endif

#define FLOOR 3u
#define CHUNK 1024u

extern const uint8_t boot_pubkey[IMG_PUBKEY_LEN];

typedef struct {
    uint8_t data[SLOT_SIZE];
    uint32_t len;
    uint32_t version;
    uint32_t counter;
    uint8_t digest[32];
} image_t;

static image_t g_old;   // in slot B to begin with, counter 3
static image_t g_new;   // the update, counter 5
static image_t g_other; // a different update, counter 5
static image_t g_for_a; // a valid image, but signed for slot A

static sim_flash_t g_sf;
static flash_if_t g_fif;
static upd_agent_t g_agent;
static uint8_t g_before[SIM_SIZE];

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int load(const char *dir, const char *name, image_t *img) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        printf("  cannot open %s\n", path);
        return 0;
    }
    memset(img->data, 0xFF, sizeof(img->data));
    img->len = (uint32_t)fread(img->data, 1, sizeof(img->data), f);
    fclose(f);
    img->version = get_u32(img->data + 16);
    img->counter = get_u32(img->data + 24);
    return mbedtls_sha256(img->data, img->len, img->digest, 0) == 0;
}

static boot_record_t record(void) {
    br_ctx_t ctx;
    boot_record_t rec;
    memset(&rec, 0, sizeof(rec));
    boot_record_init(&ctx, &g_fif, BOOT_STATE_PAGE_A, BOOT_STATE_PAGE_B);
    (void)boot_record_load(&ctx, &rec);
    return rec;
}

static void append(const boot_record_t *rec) {
    br_ctx_t ctx;
    boot_record_init(&ctx, &g_fif, BOOT_STATE_PAGE_A, BOOT_STATE_PAGE_B);
    (void)boot_record_append(&ctx, rec);
}

static void setup(void) {
    sim_flash_init(&g_sf);
    sim_flash_bind(&g_sf, &g_fif);

    br_ctx_t ctx;
    boot_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.primary_slot = 0;
    rec.slot_state[0] = SLOT_CONFIRMED;
    rec.slot_state[1] = SLOT_CONFIRMED;
    rec.security_floor = FLOOR;
    rec.last_result = BR_RESULT_OK;
    boot_record_init(&ctx, &g_fif, BOOT_STATE_PAGE_A, BOOT_STATE_PAGE_B);
    (void)boot_record_factory_init(&ctx, &rec);

    // Something in the running slot, to show it is never touched
    memset(g_sf.mem + (SLOT_A_BASE - SIM_BASE), 0xA5, 8192);
    memcpy(g_sf.mem + (SLOT_B_BASE - SIM_BASE), g_old.data, g_old.len);

    memcpy(g_before, g_sf.mem, sizeof(g_before));
    g_sf.erases = 0;
    g_sf.programs = 0;
    upd_init(&g_agent, &g_fif, boot_pubkey, 0);
}

// Power returns and the application starts again: flash is kept, the
// agent's RAM is not
static void reboot(void) {
    sim_flash_reboot(&g_sf);
    upd_init(&g_agent, &g_fif, boot_pubkey, 0);
}

static int same_range(uint32_t from, uint32_t to) {
    return memcmp(g_sf.mem + (from - SIM_BASE), g_before + (from - SIM_BASE),
                  to - from) == 0;
}

// Everything except the boot state, slot B and the progress log is as it was
static int untouched_elsewhere(void) {
    return same_range(SIM_BASE, BOOT_STATE_PAGE_A) &&
           same_range(BOOT_STATE_PAGE_B + FLASH_PAGE_SIZE, SLOT_B_BASE) &&
           same_range(PROGRESS_PAGE_B + FLASH_PAGE_SIZE, SIM_BASE + SIM_SIZE);
}

static br_result_t verify_slot_b(void) {
    return image_verify(&g_fif, SLOT_B_BASE, SLOT_SIZE, boot_pubkey, FLOOR, NULL);
}

static int slot_b_holds(const image_t *img) {
    return memcmp(g_sf.mem + (SLOT_B_BASE - SIM_BASE), img->data, img->len) == 0;
}

// ---- the gateway's side ----

static proto_offer_t offer_for(const image_t *img) {
    proto_offer_t o;
    memset(&o, 0, sizeof(o));
    o.image_size = img->len;
    o.version = img->version;
    o.security_counter = img->counter;
    o.target_slot = 1;
    o.chunk_size = CHUNK;
    memcpy(o.digest, img->digest, sizeof(o.digest));
    return o;
}

static upd_reply_t send_chunk(const image_t *img, uint32_t offset, uint32_t len) {
    proto_chunk_t c;
    c.offset = offset;
    c.length = (uint16_t)len;
    c.data = img->data + offset;
    c.crc = br_crc32(c.data, len);
    return upd_chunk(&g_agent, &c);
}

// Sends chunks from offset until the image is complete or a reply is not the
// expected CHUNK_ACK. Returns the offset reached.
static uint32_t send_from(const image_t *img, uint32_t offset, uint32_t chunk,
                          uint32_t max_chunks) {
    for (uint32_t i = 0; i < max_chunks && offset < img->len; i++) {
        uint32_t n = img->len - offset < chunk ? img->len - offset : chunk;
        upd_reply_t r = send_chunk(img, offset, n);
        if (r.type != MSG_CHUNK_ACK || r.value != offset + n) {
            break;
        }
        offset += n;
    }
    return offset;
}

// A whole update, OFFER to COMMIT. Returns the VERIFY_RESULT, or -1 if any
// earlier reply was not the expected one. *accepted receives the offset the
// node asked to resume from.
static long transfer(const image_t *img, uint16_t chunk, uint32_t *accepted) {
    proto_offer_t o = offer_for(img);
    o.chunk_size = chunk;
    upd_reply_t r = upd_offer(&g_agent, &o);
    if (r.type != MSG_ACCEPT) {
        return -1;
    }
    if (accepted != NULL) {
        *accepted = r.value;
    }
    if (send_from(img, r.value, chunk, 0xFFFFFFFFu) != img->len) {
        return -1;
    }
    r = upd_commit(&g_agent);
    return r.type == MSG_VERIFY_RESULT ? (long)r.value : -1;
}

// ---- complete updates ----

static void test_update_marks_slot_pending(void) {
    setup();
    uint32_t accepted = 99;
    CHECK_EQ(transfer(&g_new, CHUNK, &accepted), BR_RESULT_OK);
    CHECK_EQ(accepted, 0);

    boot_record_t rec = record();
    CHECK_EQ(rec.slot_state[1], SLOT_PENDING);
    CHECK_EQ(rec.slot_state[0], SLOT_CONFIRMED);
    CHECK_EQ(rec.primary_slot, 0);
    CHECK_EQ(rec.security_floor, FLOOR);
    CHECK_EQ(rec.last_result, BR_RESULT_OK);
    CHECK(slot_b_holds(&g_new));
    CHECK_EQ(verify_slot_b(), BR_RESULT_OK);
    CHECK(untouched_elsewhere());
}

static void test_chunks_may_straddle_pages(void) {
    // 1000-byte chunks do not divide a page, so some chunks start in one page
    // and end in the next, which must be erased before the chunk is written
    setup();
    CHECK_EQ(transfer(&g_new, 1000, NULL), BR_RESULT_OK);
    CHECK(slot_b_holds(&g_new));
    CHECK_EQ(record().slot_state[1], SLOT_PENDING);
}

static void test_smallest_chunks(void) {
    setup();
    CHECK_EQ(transfer(&g_new, 8, NULL), BR_RESULT_OK);
    CHECK(slot_b_holds(&g_new));
}

// ---- refusals ----

static void test_offers_refused_before_any_write(void) {
    setup();
    struct {
        const char *what;
        proto_offer_t offer;
        uint32_t reason;
    } cases[8];
    for (int i = 0; i < 8; i++) {
        cases[i].offer = offer_for(&g_new);
    }
    cases[0].what = "running slot";
    cases[0].offer.target_slot = 0;
    cases[0].reason = BR_RESULT_WRONG_SLOT;
    cases[1].what = "no such slot";
    cases[1].offer.target_slot = 2;
    cases[1].reason = BR_RESULT_WRONG_SLOT;
    cases[2].what = "larger than a slot";
    cases[2].offer.image_size = SLOT_SIZE + 1u;
    cases[2].reason = UPD_ERR_TOO_LARGE;
    cases[3].what = "smaller than header and trailer";
    cases[3].offer.image_size = IMG_HDR_SIZE + IMG_TRAILER_LEN - 1u;
    cases[3].reason = BR_RESULT_BAD_HEADER;
    cases[4].what = "chunk size zero";
    cases[4].offer.chunk_size = 0;
    cases[4].reason = UPD_ERR_CHUNK_SIZE;
    cases[5].what = "chunk size above the maximum";
    cases[5].offer.chunk_size = PROTO_CHUNK_MAX + 8u;
    cases[5].reason = UPD_ERR_CHUNK_SIZE;
    cases[6].what = "chunk size not a multiple of 8";
    cases[6].offer.chunk_size = 1020;
    cases[6].reason = UPD_ERR_CHUNK_SIZE;
    cases[7].what = "counter below the floor";
    cases[7].offer.security_counter = FLOOR - 1u;
    cases[7].reason = BR_RESULT_ROLLBACK_BLOCKED;

    for (int i = 0; i < 8; i++) {
        upd_reply_t r = upd_offer(&g_agent, &cases[i].offer);
        if (r.type != MSG_REJECT || r.value != cases[i].reason) {
            printf("  %s: got type 0x%02x value %u\n", cases[i].what, r.type, r.value);
        }
        CHECK_EQ(r.type, MSG_REJECT);
        CHECK_EQ(r.value, cases[i].reason);
    }
    CHECK_EQ(g_sf.erases, 0);
    CHECK_EQ(g_sf.programs, 0);
    CHECK_EQ(record().slot_state[1], SLOT_CONFIRMED);

    // A refused offer also ends any transfer that was under way
    proto_offer_t good = offer_for(&g_new);
    CHECK_EQ(upd_offer(&g_agent, &good).type, MSG_ACCEPT);
    (void)upd_offer(&g_agent, &cases[0].offer);
    CHECK_EQ(send_chunk(&g_new, 0, CHUNK).value, UPD_ERR_NO_SESSION);
}

static void test_refused_while_on_trial(void) {
    // The running image is on trial: slot B is its only fallback
    setup();
    boot_record_t rec = record();
    rec.slot_state[0] = SLOT_TRIAL;
    rec.trial_attempts = 1;
    append(&rec);
    long writes = g_sf.erases + g_sf.programs;

    proto_offer_t o = offer_for(&g_new);
    upd_reply_t r = upd_offer(&g_agent, &o);
    CHECK_EQ(r.type, MSG_REJECT);
    CHECK_EQ(r.value, UPD_ERR_BUSY_TRIAL);
    CHECK_EQ(g_sf.erases + g_sf.programs, writes);
}

static void test_refused_into_primary_slot(void) {
    // Running from A while B is primary is not a state the bootloader leaves
    // outside a trial, but the primary slot is protected regardless
    setup();
    boot_record_t rec = record();
    rec.primary_slot = 1;
    append(&rec);

    proto_offer_t o = offer_for(&g_new);
    upd_reply_t r = upd_offer(&g_agent, &o);
    CHECK_EQ(r.type, MSG_REJECT);
    CHECK_EQ(r.value, BR_RESULT_WRONG_SLOT);
    CHECK(slot_b_holds(&g_old));
}

// ---- chunks ----

static void test_chunk_and_commit_need_a_session(void) {
    setup();
    upd_reply_t r = send_chunk(&g_new, 0, CHUNK);
    CHECK_EQ(r.type, MSG_REJECT);
    CHECK_EQ(r.value, UPD_ERR_NO_SESSION);
    r = upd_commit(&g_agent);
    CHECK_EQ(r.type, MSG_REJECT);
    CHECK_EQ(r.value, UPD_ERR_NO_SESSION);
    CHECK_EQ(g_sf.erases + g_sf.programs, 0);
}

static void test_bad_chunks_are_not_written(void) {
    setup();
    proto_offer_t o = offer_for(&g_new);
    CHECK_EQ(upd_offer(&g_agent, &o).type, MSG_ACCEPT);
    long programs = g_sf.programs;
    long erases = g_sf.erases;

    // Data that does not match its CRC
    proto_chunk_t c;
    c.offset = 0;
    c.length = CHUNK;
    c.data = g_new.data;
    c.crc = br_crc32(g_new.data, CHUNK) ^ 1u;
    upd_reply_t r = upd_chunk(&g_agent, &c);
    CHECK_EQ(r.type, MSG_REJECT);
    CHECK_EQ(r.value, UPD_ERR_CHUNK_CRC);

    // Shorter than the agreed chunk size, though not the last chunk
    r = send_chunk(&g_new, 0, 512);
    CHECK_EQ(r.type, MSG_REJECT);
    CHECK_EQ(r.value, UPD_ERR_CHUNK_SIZE);

    // Ahead of the offset expected: answered with the offset expected
    r = send_chunk(&g_new, CHUNK, CHUNK);
    CHECK_EQ(r.type, MSG_CHUNK_ACK);
    CHECK_EQ(r.value, 0);
    CHECK_EQ(g_sf.programs, programs);
    CHECK_EQ(g_sf.erases, erases);

    // The session survives all of these
    r = send_chunk(&g_new, 0, CHUNK);
    CHECK_EQ(r.type, MSG_CHUNK_ACK);
    CHECK_EQ(r.value, CHUNK);

    // A resend after a lost CHUNK_ACK is acknowledged again, not rewritten
    programs = g_sf.programs;
    r = send_chunk(&g_new, 0, CHUNK);
    CHECK_EQ(r.type, MSG_CHUNK_ACK);
    CHECK_EQ(r.value, CHUNK);
    CHECK_EQ(g_sf.programs, programs);

    r = upd_commit(&g_agent);
    CHECK_EQ(r.type, MSG_REJECT);
    CHECK_EQ(r.value, UPD_ERR_INCOMPLETE);

    CHECK_EQ(send_from(&g_new, CHUNK, CHUNK, 0xFFFFFFFFu), g_new.len);
    r = upd_commit(&g_agent);
    CHECK_EQ(r.type, MSG_VERIFY_RESULT);
    CHECK_EQ(r.value, BR_RESULT_OK);
}

// ---- verification at COMMIT ----

static void test_tampered_image_is_not_marked_pending(void) {
    // Corrupted before the gateway computed its CRCs, so every chunk arrives
    // intact and only the whole-image check can catch it
    static image_t bad;
    bad = g_new;
    bad.data[IMG_HDR_SIZE + 100u] ^= 0x01u;

    setup();
    CHECK_EQ(transfer(&bad, CHUNK, NULL), BR_RESULT_BAD_HASH);
    CHECK_EQ(record().slot_state[1], SLOT_EMPTY);

    // The progress is forgotten: trying again starts from zero rather than
    // resuming into the same bad data
    reboot();
    proto_offer_t o = offer_for(&bad);
    upd_reply_t r = upd_offer(&g_agent, &o);
    CHECK_EQ(r.type, MSG_ACCEPT);
    CHECK_EQ(r.value, 0);
}

static void test_image_for_other_slot_is_not_marked_pending(void) {
    setup();
    CHECK_EQ(transfer(&g_for_a, CHUNK, NULL), BR_RESULT_WRONG_SLOT);
    CHECK_EQ(record().slot_state[1], SLOT_EMPTY);
}

static void test_image_must_be_exactly_what_was_offered(void) {
    // A valid image followed by bytes it does not cover: the offered size
    // does not match the signed image
    static image_t longer;
    longer = g_new;
    longer.len += 8u;

    setup();
    CHECK_EQ(transfer(&longer, CHUNK, NULL), BR_RESULT_BAD_HEADER);
    CHECK_EQ(record().slot_state[1], SLOT_EMPTY);
}

// ---- resuming ----

static void test_resume_after_reset(void) {
    // Three chunks complete the first page and part of the second. After a
    // reset the node resumes from the page it last completed.
    setup();
    proto_offer_t o = offer_for(&g_new);
    CHECK_EQ(upd_offer(&g_agent, &o).type, MSG_ACCEPT);
    CHECK_EQ(send_from(&g_new, 0, CHUNK, 3), 3u * CHUNK);

    reboot();
    uint32_t accepted = 0;
    CHECK_EQ(transfer(&g_new, CHUNK, &accepted), BR_RESULT_OK);
    CHECK_EQ(accepted, FLASH_PAGE_SIZE);
    CHECK(slot_b_holds(&g_new));
    CHECK_EQ(record().slot_state[1], SLOT_PENDING);
}

static void test_resume_after_abort(void) {
    setup();
    proto_offer_t o = offer_for(&g_new);
    CHECK_EQ(upd_offer(&g_agent, &o).type, MSG_ACCEPT);
    CHECK_EQ(send_from(&g_new, 0, CHUNK, 3), 3u * CHUNK);

    upd_abort(&g_agent);
    CHECK_EQ(send_chunk(&g_new, 3u * CHUNK, CHUNK).value, UPD_ERR_NO_SESSION);

    uint32_t accepted = 0;
    CHECK_EQ(transfer(&g_new, CHUNK, &accepted), BR_RESULT_OK);
    CHECK_EQ(accepted, FLASH_PAGE_SIZE);
}

static void test_resume_only_for_the_same_image(void) {
    // An interrupted transfer of one image, then part of another written over
    // its first page, then the first offered again. Resuming the first would
    // keep the other's bytes in that page, so it must start from zero.
    setup();
    proto_offer_t o = offer_for(&g_new);
    CHECK_EQ(upd_offer(&g_agent, &o).type, MSG_ACCEPT);
    CHECK_EQ(send_from(&g_new, 0, CHUNK, 3), 3u * CHUNK);
    reboot();

    o = offer_for(&g_other);
    upd_reply_t r = upd_offer(&g_agent, &o);
    CHECK_EQ(r.type, MSG_ACCEPT);
    CHECK_EQ(r.value, 0);
    CHECK_EQ(send_from(&g_other, 0, CHUNK, 1), CHUNK);
    reboot();

    uint32_t accepted = 99;
    CHECK_EQ(transfer(&g_new, CHUNK, &accepted), BR_RESULT_OK);
    CHECK_EQ(accepted, 0);
    CHECK(slot_b_holds(&g_new));
}

static void test_no_resume_into_a_verified_slot(void) {
    // Once an image is PENDING, offering it again starts over: the slot is
    // marked EMPTY before it is erased, never resumed into
    setup();
    CHECK_EQ(transfer(&g_new, CHUNK, NULL), BR_RESULT_OK);
    proto_offer_t o = offer_for(&g_new);
    upd_reply_t r = upd_offer(&g_agent, &o);
    CHECK_EQ(r.type, MSG_ACCEPT);
    CHECK_EQ(r.value, 0);
    CHECK_EQ(record().slot_state[1], SLOT_EMPTY);
}

// ---- power cuts ----
//
// A whole update, cut at every flash operation from OFFER to the PENDING
// record, cleanly and mid-operation. After each cut and a reboot:
//
//  - the boot record still names slot A, CONFIRMED, as primary
//  - slot B is EMPTY, or holds an image that verifies: never marked
//    bootable over a partial image
//  - nothing outside the regions the agent owns has changed
//  - the gateway trying again completes the update
//
// Resuming is counted, so a change that quietly always restarts from zero
// fails the test instead of passing it more slowly.

// ---- confirmation ----

// Slot A, the running slot, on trial after an update from slot B, which is
// still CONFIRMED and primary. filler more copies of that record are appended,
// so the confirmation can be made to land at the end of a page.
static boot_record_t on_trial(int filler) {
    boot_record_t rec = record();
    rec.primary_slot = 1;
    rec.slot_state[0] = SLOT_TRIAL;
    rec.trial_attempts = 2;
    rec.last_result = BR_RESULT_OK;
    for (int i = 0; i <= filler; i++) {
        append(&rec);
    }
    return record();
}

static void test_confirm_trial_image(void) {
    setup();
    boot_record_t before = on_trial(0);

    CHECK_EQ(upd_confirm(&g_agent, 5), BR_RESULT_OK);
    boot_record_t rec = record();
    CHECK_EQ(rec.seq, before.seq + 1u);
    CHECK_EQ(rec.slot_state[0], SLOT_CONFIRMED);
    CHECK_EQ(rec.primary_slot, 0);
    CHECK_EQ(rec.security_floor, 5u);
    CHECK_EQ(rec.last_result, BR_RESULT_OK);
    // The previous image stays a fallback, and the attempt count stays as a
    // record of how the trial went
    CHECK_EQ(rec.slot_state[1], SLOT_CONFIRMED);
    CHECK_EQ(rec.trial_attempts, 2);
    CHECK(slot_b_holds(&g_old));
    CHECK(untouched_elsewhere());
}

static void test_confirm_never_lowers_the_floor(void) {
    setup();
    boot_record_t rec = on_trial(0);
    rec.security_floor = 7;
    append(&rec);

    CHECK_EQ(upd_confirm(&g_agent, 5), BR_RESULT_OK);
    CHECK_EQ(record().security_floor, 7u);
    CHECK_EQ(record().slot_state[0], SLOT_CONFIRMED);
}

static void test_confirm_twice_writes_once(void) {
    // A confirmed image is confirmed again without a write, so the
    // application can retry without knowing whether the first one landed
    setup();
    (void)on_trial(0);
    CHECK_EQ(upd_confirm(&g_agent, 5), BR_RESULT_OK);
    long writes = g_sf.erases + g_sf.programs;
    uint32_t seq = record().seq;

    CHECK_EQ(upd_confirm(&g_agent, 5), BR_RESULT_OK);
    CHECK_EQ(g_sf.erases + g_sf.programs, writes);
    CHECK_EQ(record().seq, seq);
}

static void test_confirm_only_from_trial(void) {
    // A running slot is never EMPTY, PENDING or REJECTED in practice, but if
    // the record says so it is not the application's to overrule
    static const uint8_t states[] = {SLOT_EMPTY, SLOT_PENDING, SLOT_REJECTED};
    for (size_t i = 0; i < sizeof(states); i++) {
        setup();
        boot_record_t rec = on_trial(0);
        rec.slot_state[0] = states[i];
        append(&rec);
        long writes = g_sf.erases + g_sf.programs;

        CHECK_EQ(upd_confirm(&g_agent, 5), BR_RESULT_NONE);
        CHECK_EQ(g_sf.erases + g_sf.programs, writes);
        CHECK_EQ(record().slot_state[0], states[i]);
    }
}

static void test_update_after_confirm(void) {
    // On trial nothing is accepted. Once confirmed, the old primary can be
    // updated, but not with an image below the floor the confirmation raised.
    setup();
    (void)on_trial(0);
    proto_offer_t o = offer_for(&g_new);
    CHECK_EQ(upd_offer(&g_agent, &o).value, UPD_ERR_BUSY_TRIAL);

    CHECK_EQ(upd_confirm(&g_agent, 5), BR_RESULT_OK);
    proto_offer_t old = offer_for(&g_old);
    upd_reply_t r = upd_offer(&g_agent, &old);
    CHECK_EQ(r.type, MSG_REJECT);
    CHECK_EQ(r.value, BR_RESULT_ROLLBACK_BLOCKED);
    CHECK_EQ(transfer(&g_new, CHUNK, NULL), BR_RESULT_OK);
    CHECK_EQ(record().slot_state[1], SLOT_PENDING);
}

// Every power cut during a confirmation leaves the trial record or the
// confirmed one, never anything else, and confirming again then completes it.
// Run once with room in the page, and once with the confirmation landing on
// a page change, where it erases the other page first.
static boot_record_t confirm_cut(int filler, long cut, int tear) {
    setup();
    boot_record_t trial = on_trial(filler);
    sim_flash_arm_cut(&g_sf, cut, tear != 0);
    (void)upd_confirm(&g_agent, 5);
    return trial;
}

static void test_power_cut_during_confirm(void) {
    // The factory record and the trial record, then filler: BR_PER_PAGE - 2
    // fills the first page exactly
    static const int fillers[] = {0, (int)BR_PER_PAGE - 2};
    long failures = 0;
    for (size_t f = 0; f < 2; f++) {
        (void)confirm_cut(fillers[f], -1, 0);
        long total = g_sf.ops;
        // Four double-words, and with a page change an erase before them
        CHECK_EQ(total, f == 0 ? 4 : 5);
        for (long cut = 1; cut <= total; cut++) {
            for (int tear = 0; tear < 2; tear++) {
                boot_record_t trial = confirm_cut(fillers[f], cut, tear);
                reboot();

                boot_record_t rec = record();
                int still_trial = rec.seq == trial.seq &&
                                  rec.slot_state[0] == SLOT_TRIAL &&
                                  rec.primary_slot == 1 && rec.security_floor == FLOOR;
                int confirmed = rec.seq == trial.seq + 1u &&
                                rec.slot_state[0] == SLOT_CONFIRMED &&
                                rec.primary_slot == 0 && rec.security_floor == 5u;
                const char *why = NULL;
                if (!still_trial && !confirmed) {
                    why = "neither the trial record nor the confirmed one";
                } else if (upd_confirm(&g_agent, 5) != BR_RESULT_OK ||
                           record().slot_state[0] != SLOT_CONFIRMED) {
                    why = "confirming again did not complete";
                } else if (!untouched_elsewhere()) {
                    why = "flash written outside the boot state";
                }
                if (why != NULL) {
                    printf("  FAIL filler=%d cut=%ld tear=%d: %s\n", fillers[f], cut,
                           tear, why);
                    failures++;
                }
            }
        }
        printf("  filler %d: swept %ld flash operations x 2 cut modes\n", fillers[f],
               total);
    }
    CHECK_EQ(failures, 0);
}

static long run_cut(long cut, int tear) {
    setup();
    sim_flash_arm_cut(&g_sf, cut, tear != 0);
    (void)transfer(&g_new, CHUNK, NULL);
    return g_sf.ops;
}

static void test_power_cut_at_every_operation(void) {
    long total = run_cut(-1, 0);
    CHECK_EQ(record().slot_state[1], SLOT_PENDING);

    long failures = 0;
    long resumed = 0;
    for (long cut = 1; cut <= total; cut++) {
        for (int tear = 0; tear < 2; tear++) {
            (void)run_cut(cut, tear);
            reboot();

            boot_record_t rec = record();
            uint8_t b = rec.slot_state[1];
            const char *why = NULL;
            if (rec.primary_slot != 0 || rec.slot_state[0] != SLOT_CONFIRMED) {
                why = "slot A no longer confirmed primary";
            } else if (b != SLOT_EMPTY && b != SLOT_CONFIRMED && b != SLOT_PENDING) {
                why = "slot B in an unexpected state";
            } else if (b != SLOT_EMPTY && verify_slot_b() != BR_RESULT_OK) {
                why = "slot B marked bootable but does not verify";
            } else if (!untouched_elsewhere()) {
                why = "flash written outside the agent's regions";
            }

            uint32_t accepted = 0;
            if (why == NULL) {
                if (transfer(&g_new, CHUNK, &accepted) != BR_RESULT_OK ||
                    !slot_b_holds(&g_new) || record().slot_state[1] != SLOT_PENDING) {
                    why = "retry did not complete the update";
                } else if (accepted > 0u) {
                    resumed++;
                }
            }
            if (why != NULL) {
                printf("  FAIL cut=%ld tear=%d: %s\n", cut, tear, why);
                failures++;
            }
        }
    }
    printf("  swept %ld flash operations x 2 cut modes, %ld retries resumed\n", total,
           resumed);
    CHECK_EQ(failures, 0);
    CHECK(resumed > 0);
}

TEST_MAIN_BEGIN("update agent")
if (!load(UPDATE_IMAGE_DIR, "old.img", &g_old) ||
    !load(UPDATE_IMAGE_DIR, "new.img", &g_new) ||
    !load(UPDATE_IMAGE_DIR, "other.img", &g_other) ||
    !load(TEST_IMAGE_DIR, "good.img", &g_for_a)) {
    return 1;
}
RUN(test_update_marks_slot_pending);
RUN(test_chunks_may_straddle_pages);
RUN(test_smallest_chunks);
RUN(test_offers_refused_before_any_write);
RUN(test_refused_while_on_trial);
RUN(test_refused_into_primary_slot);
RUN(test_chunk_and_commit_need_a_session);
RUN(test_bad_chunks_are_not_written);
RUN(test_tampered_image_is_not_marked_pending);
RUN(test_image_for_other_slot_is_not_marked_pending);
RUN(test_image_must_be_exactly_what_was_offered);
RUN(test_resume_after_reset);
RUN(test_resume_after_abort);
RUN(test_resume_only_for_the_same_image);
RUN(test_no_resume_into_a_verified_slot);
RUN(test_power_cut_at_every_operation);
RUN(test_confirm_trial_image);
RUN(test_confirm_never_lowers_the_floor);
RUN(test_confirm_twice_writes_once);
RUN(test_confirm_only_from_trial);
RUN(test_update_after_confirm);
RUN(test_power_cut_during_confirm);
TEST_MAIN_END()
