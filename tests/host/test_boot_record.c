#include <string.h>

#include "boot_record.h"
#include "sim_flash.h"
#include "test_util.h"

// Logic correctness without fault injection. Power-fail behaviour is covered
// separately in test_powerfail.c, because the two failure modes are worth
// keeping distinct in the output: a failure here means the state machine is
// wrong, a failure there means recovery is wrong.

#define PAGE_A (SIM_BASE + 0u * FLASH_PAGE_SIZE)
#define PAGE_B (SIM_BASE + 1u * FLASH_PAGE_SIZE)

static sim_flash_t sf;
static flash_if_t fif;
static br_ctx_t ctx;

static void setup(void) {
    sim_flash_init(&sf);
    sim_flash_bind(&sf, &fif);
    boot_record_init(&ctx, &fif, PAGE_A, PAGE_B);
}

static boot_record_t make_rec(uint8_t primary, uint8_t s0, uint8_t s1, uint8_t trials,
                              uint32_t floor) {
    boot_record_t r;
    memset(&r, 0, sizeof(r));
    r.primary_slot = primary;
    r.slot_state[0] = s0;
    r.slot_state[1] = s1;
    r.trial_attempts = trials;
    r.security_floor = floor;
    r.last_result = BR_RESULT_OK;
    return r;
}

// ---- on-flash layout ----

static void test_record_is_four_doublewords(void) {
    // The entire power-fail argument rests on a record being exactly four
    // double-words with the commit word last. If this changes, the write
    // ordering and CRC coverage have to be revisited together.
    CHECK_EQ(BR_SIZE, 32);
    CHECK_EQ(BR_SIZE % FLASH_DW_SIZE, 0);
    CHECK_EQ(BR_PER_PAGE, 64);
}

static void test_serialize_roundtrip(void) {
    boot_record_t in = make_rec(1, SLOT_CONFIRMED, SLOT_TRIAL, 2, 0xDEADBEEF);
    in.seq = 0x12345678;
    in.last_result = BR_RESULT_NOT_CONFIRMED;

    uint8_t buf[BR_SIZE];
    boot_record_serialize(&in, buf);

    boot_record_t out;
    CHECK(boot_record_deserialize(buf, &out));
    CHECK_EQ(out.seq, in.seq);
    CHECK_EQ(out.primary_slot, in.primary_slot);
    CHECK_EQ(out.slot_state[0], in.slot_state[0]);
    CHECK_EQ(out.slot_state[1], in.slot_state[1]);
    CHECK_EQ(out.trial_attempts, in.trial_attempts);
    CHECK_EQ(out.security_floor, in.security_floor);
    CHECK_EQ(out.last_result, in.last_result);
}

static void test_field_offsets_are_frozen(void) {
    // The bootloader and application both parse these bytes, and the
    // bootloader cannot be updated in the field. If an offset moves, a
    // deployed device stops understanding records written by new firmware.
    boot_record_t in = make_rec(1, SLOT_PENDING, SLOT_CONFIRMED, 3, 0x11223344);
    in.seq = 0xAABBCCDD;
    in.last_result = 0x55667788;

    uint8_t b[BR_SIZE];
    boot_record_serialize(&in, b);

    CHECK_EQ(b[0], 0x42); // magic, little-endian "BOR1"
    CHECK_EQ(b[4], 0xDD); // seq
    CHECK_EQ(b[7], 0xAA);
    CHECK_EQ(b[8], 1); // primary_slot
    CHECK_EQ(b[9], SLOT_PENDING);
    CHECK_EQ(b[10], SLOT_CONFIRMED);
    CHECK_EQ(b[11], 3);    // trial_attempts
    CHECK_EQ(b[12], 0x44); // security_floor
    CHECK_EQ(b[16], 0x88); // last_result
    CHECK_EQ(b[24], 0x42); // commit pattern low byte
}

// ---- rejection of damaged records ----

static void test_corrupt_crc_rejected(void) {
    boot_record_t in = make_rec(0, SLOT_CONFIRMED, SLOT_EMPTY, 0, 1);
    in.seq = 5;
    uint8_t buf[BR_SIZE];
    boot_record_serialize(&in, buf);
    buf[12] ^= 0xFF; // flip a byte inside CRC coverage

    boot_record_t out;
    CHECK(!boot_record_deserialize(buf, &out));
}

static void test_torn_at_every_doubleword_rejected(void) {
    // A power cut can leave any prefix of a record written. Every such prefix
    // must be rejected. This is the unit-level form of what the power-fail
    // sweep proves at system level: no partial record is ever accepted.
    boot_record_t in = make_rec(1, SLOT_CONFIRMED, SLOT_PENDING, 1, 42);
    in.seq = 9;
    uint8_t full[BR_SIZE];
    boot_record_serialize(&in, full);

    for (uint32_t dws = 0; dws < 4u; dws++) {
        uint8_t buf[BR_SIZE];
        memset(buf, FLASH_ERASED_BYTE, BR_SIZE);
        memcpy(buf, full, dws * FLASH_DW_SIZE);
        boot_record_t out;
        CHECK(!boot_record_deserialize(buf, &out));
    }

    // And the complete record IS accepted, so the loop above is not passing
    // simply because deserialize rejects everything.
    boot_record_t out;
    CHECK(boot_record_deserialize(full, &out));
    CHECK_EQ(out.seq, 9);
}

static void test_impossible_values_rejected(void) {
    // Passes CRC but names a slot that cannot exist: writer is buggy.
    boot_record_t in = make_rec(7, SLOT_CONFIRMED, SLOT_EMPTY, 0, 1);
    in.seq = 5;
    uint8_t buf[BR_SIZE];
    boot_record_serialize(&in, buf);
    boot_record_t out;
    CHECK(!boot_record_deserialize(buf, &out));

    boot_record_t bad_state = make_rec(0, 99, SLOT_EMPTY, 0, 1);
    bad_state.seq = 5;
    boot_record_serialize(&bad_state, buf);
    CHECK(!boot_record_deserialize(buf, &out));
}

// ---- load and append ----

static void test_load_on_blank_device(void) {
    setup();
    boot_record_t got;
    CHECK(!boot_record_load(&ctx, &got));
    CHECK_EQ(ctx.cur_page, -1);
}

static void test_factory_init_then_load(void) {
    setup();
    boot_record_t first = make_rec(0, SLOT_CONFIRMED, SLOT_EMPTY, 0, 0);
    CHECK_EQ(boot_record_factory_init(&ctx, &first), FLASH_OK);

    boot_record_t got;
    CHECK(boot_record_load(&ctx, &got));
    CHECK_EQ(got.seq, 1);
    CHECK_EQ(got.slot_state[0], SLOT_CONFIRMED);
}

static void test_append_increments_seq(void) {
    setup();
    boot_record_t r = make_rec(0, SLOT_CONFIRMED, SLOT_EMPTY, 0, 0);
    CHECK_EQ(boot_record_factory_init(&ctx, &r), FLASH_OK);

    for (uint8_t i = 0; i < 5; i++) {
        r.trial_attempts = i;
        CHECK_EQ(boot_record_append(&ctx, &r), FLASH_OK);
    }

    boot_record_t got;
    CHECK(boot_record_load(&ctx, &got));
    CHECK_EQ(got.seq, 6); // factory = 1, plus five appends
    CHECK_EQ(got.trial_attempts, 4);
}

static void test_never_reprograms(void) {
    // The append-only design exists because the G4 cannot rewrite a
    // double-word. If any path tried, sim_flash would return
    // FLASH_ERR_NOT_ERASED and one of these appends would fail.
    setup();
    boot_record_t r = make_rec(0, SLOT_CONFIRMED, SLOT_EMPTY, 0, 0);
    CHECK_EQ(boot_record_factory_init(&ctx, &r), FLASH_OK);

    for (uint32_t i = 0; i < 300u; i++) {
        r.trial_attempts = (uint8_t)(i % 4u);
        CHECK_EQ(boot_record_append(&ctx, &r), FLASH_OK);
    }

    boot_record_t got;
    CHECK(boot_record_load(&ctx, &got));
    CHECK_EQ(got.seq, 301);
}

// ---- page rotation ----

static void test_page_rotation(void) {
    setup();
    boot_record_t r = make_rec(0, SLOT_CONFIRMED, SLOT_EMPTY, 0, 0);
    CHECK_EQ(boot_record_factory_init(&ctx, &r), FLASH_OK);

    long erases_before = sf.erases;
    for (uint32_t i = 0; i < BR_PER_PAGE + 5u; i++) {
        r.security_floor = i;
        CHECK_EQ(boot_record_append(&ctx, &r), FLASH_OK);
    }

    boot_record_t got;
    CHECK(boot_record_load(&ctx, &got));
    CHECK_EQ(got.security_floor, BR_PER_PAGE + 4u);
    // Exactly one rotation. More would mean we are burning erase cycles
    // unnecessarily, which matters on a device expected to last years.
    CHECK_EQ(sf.erases - erases_before, 1);
    CHECK_EQ(ctx.cur_page, 1);
}

static void test_newest_wins_across_pages(void) {
    // After rotation, page A still holds many valid older records and page B
    // holds the newest. A fresh context with no cached position must pick B.
    // This is the real boot path: the bootloader starts from nothing.
    setup();
    boot_record_t r = make_rec(0, SLOT_CONFIRMED, SLOT_EMPTY, 0, 0);
    CHECK_EQ(boot_record_factory_init(&ctx, &r), FLASH_OK);
    for (uint32_t i = 0; i < BR_PER_PAGE + 3u; i++) {
        r.security_floor = i;
        CHECK_EQ(boot_record_append(&ctx, &r), FLASH_OK);
    }

    br_ctx_t fresh;
    boot_record_init(&fresh, &fif, PAGE_A, PAGE_B);
    boot_record_t got;
    CHECK(boot_record_load(&fresh, &got));
    CHECK_EQ(got.security_floor, BR_PER_PAGE + 2u);
    CHECK_EQ(fresh.cur_page, 1);
}

static void test_scan_continues_past_damaged_entry(void) {
    // A torn entry cannot be rewritten, so valid records exist after it. A scan
    // that stopped at the first unreadable entry would miss the newer records.
    //
    // Asserting only that the new record is found is not enough: a scan that
    // stops early reports the page as full and rotates, which also preserves
    // the record. So this test pins down that no rotation occurred and the
    // write landed in the same page, immediately after the damage.
    setup();
    boot_record_t r = make_rec(0, SLOT_CONFIRMED, SLOT_EMPTY, 0, 100);
    CHECK_EQ(boot_record_factory_init(&ctx, &r), FLASH_OK);

    // Damage entry 1 by tearing its first double-word.
    sim_flash_arm_cut(&sf, 1, true);
    (void)flash_program_dw(&fif, PAGE_A + 1u * BR_SIZE, 0x1122334455667788ull);
    sim_flash_reboot(&sf);

    long erases_before = sf.erases;

    // Should land at entry 2, skipping the damaged entry 1.
    r.security_floor = 200;
    CHECK_EQ(boot_record_append(&ctx, &r), FLASH_OK);

    // No rotation: the page had 62 free entries left, damage or not.
    CHECK_EQ(sf.erases - erases_before, 0);
    CHECK_EQ(ctx.cur_page, 0);

    br_ctx_t fresh;
    boot_record_init(&fresh, &fif, PAGE_A, PAGE_B);
    boot_record_t got;
    CHECK(boot_record_load(&fresh, &got));
    CHECK_EQ(got.security_floor, 200);
    CHECK_EQ(fresh.cur_page, 0);
}

TEST_MAIN_BEGIN("boot_record")
RUN(test_record_is_four_doublewords);
RUN(test_serialize_roundtrip);
RUN(test_field_offsets_are_frozen);
RUN(test_corrupt_crc_rejected);
RUN(test_torn_at_every_doubleword_rejected);
RUN(test_impossible_values_rejected);
RUN(test_load_on_blank_device);
RUN(test_factory_init_then_load);
RUN(test_append_increments_seq);
RUN(test_never_reprograms);
RUN(test_page_rotation);
RUN(test_newest_wins_across_pages);
RUN(test_scan_continues_past_damaged_entry);
TEST_MAIN_END()