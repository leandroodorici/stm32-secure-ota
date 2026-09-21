#include <stdio.h>
#include <string.h>

#include "boot_record.h"
#include "sim_flash.h"
#include "test_util.h"

// The exhaustive sweep
//
// docs/DESIGN.md claims a power loss at any point leaves the device with at
// least one valid, bootable record. This test enumerates every flash operation
// in an update lifecycle, cuts at each in both modes, reboots, and asserts recovery.
//
// Enumeration rather than sampling is the point. Random testing leaves
// untested windows, and the premise of this design is that untested windows
// are unacceptable on a device nobody can reach. The full sweep is a few
// hundred iterations and runs in milliseconds, so there is no reason to
// sample.

#define PAGE_A (SIM_BASE + 0u * FLASH_PAGE_SIZE)
#define PAGE_B (SIM_BASE + 1u * FLASH_PAGE_SIZE)

#define MAX_STEPS 8

typedef struct {
    const char *name;
    boot_record_t rec;
} step_t;

// A representative update lifecycle, mirroring docs/DESIGN.md sections 6 and 7
static void build_sequence(step_t *steps, int *count) {
    int n = 0;
    boot_record_t r;
    memset(&r, 0, sizeof(r));

    // Steady state: running a confirmed image in slot 0
    r.primary_slot = 0;
    r.slot_state[0] = SLOT_CONFIRMED;
    r.slot_state[1] = SLOT_EMPTY;
    r.trial_attempts = 0;
    r.security_floor = 1;
    r.last_result = BR_RESULT_OK;
    steps[n].rec = r;
    steps[n++].name = "steady";

    // Updater is about to erase slot 1. Marking it EMPTY first means a cut
    // during the erase can never leave a half-image the bootloader might try.
    r.slot_state[1] = SLOT_EMPTY;
    steps[n].rec = r;
    steps[n++].name = "slot1-empty";

    // Image fully written and hash-verified
    r.slot_state[1] = SLOT_PENDING;
    steps[n].rec = r;
    steps[n++].name = "slot1-pending";

    // Bootloader takes the trial, incrementing attempts BEFORE jumping so a
    // crash at any point still consumes an attempt.
    r.slot_state[1] = SLOT_TRIAL;
    r.trial_attempts = 1;
    steps[n].rec = r;
    steps[n++].name = "slot1-trial-1";

    // Health check passed: confirm, switch primary, raise the floor. All three
    // in one record so the transition is atomic.
    r.slot_state[1] = SLOT_CONFIRMED;
    r.slot_state[0] = SLOT_CONFIRMED;
    r.primary_slot = 1;
    r.trial_attempts = 0;
    r.security_floor = 2;
    steps[n].rec = r;
    steps[n++].name = "slot1-confirmed";

    *count = n;
}

// Runs the lifecycle with no fault, returning the number of flash operations.
// That count is the sweep range.
static long count_operations(void) {
    sim_flash_t sf;
    flash_if_t fif;
    br_ctx_t ctx;
    step_t steps[MAX_STEPS];
    int n;

    build_sequence(steps, &n);
    sim_flash_init(&sf);
    sim_flash_bind(&sf, &fif);
    boot_record_init(&ctx, &fif, PAGE_A, PAGE_B);

    boot_record_factory_init(&ctx, &steps[0].rec);
    sf.ops = 0; // count only the lifecycle, not the factory write
    for (int i = 1; i < n; i++) {
        boot_record_append(&ctx, &steps[i].rec);
    }
    return sf.ops;
}

// Replays the lifecycle, cutting at operation "cut", then reboots and checks
// the invariants. Returns 0 on success.
static int run_one_cut(long cut, bool tear, char *why, size_t why_len) {
    sim_flash_t sf;
    flash_if_t fif;
    br_ctx_t ctx;
    step_t steps[MAX_STEPS];
    int n;

    build_sequence(steps, &n);
    sim_flash_init(&sf);
    sim_flash_bind(&sf, &fif);
    boot_record_init(&ctx, &fif, PAGE_A, PAGE_B);

    // Start from a device that already has valid state, modelling one in the
    // field rather than a blank board. No fault during this part.
    boot_record_factory_init(&ctx, &steps[0].rec);

    sim_flash_arm_cut(&sf, cut, tear);
    for (int i = 1; i < n; i++) {
        boot_record_append(&ctx, &steps[i].rec);
        if (!sf.powered) {
            break;
        }
    }

    // ---- power returns ----
    sim_flash_reboot(&sf);

    // The bootloader starts from nothing: no cached position, fresh context.
    // This is the real recovery path.
    br_ctx_t fresh;
    boot_record_init(&fresh, &fif, PAGE_A, PAGE_B);
    boot_record_t got;

    // 1. A valid record is still readable
    if (!boot_record_load(&fresh, &got)) {
        snprintf(why, why_len, "no valid record after cut");
        return 1;
    }

    // 2. The surviving state is one we intended to write, not a mixture of two
    bool matches = false;
    for (int i = 0; i < n; i++) {
        const boot_record_t *s = &steps[i].rec;
        if (got.primary_slot == s->primary_slot &&
            got.slot_state[0] == s->slot_state[0] &&
            got.slot_state[1] == s->slot_state[1] &&
            got.trial_attempts == s->trial_attempts &&
            got.security_floor == s->security_floor) {
            matches = true;
            break;
        }
    }
    if (!matches) {
        snprintf(why, why_len, "surviving record matches no intended state");
        return 1;
    }

    // 3. A bootable slot exists: either the primary is usable, or the other
    //    slot is CONFIRMED and available as fallback.
    uint8_t ps = got.slot_state[got.primary_slot];
    uint8_t os = got.slot_state[got.primary_slot ^ 1u];
    if (ps != SLOT_CONFIRMED && ps != SLOT_TRIAL && os != SLOT_CONFIRMED) {
        snprintf(why, why_len, "no bootable slot: primary=%u other=%u", ps, os);
        return 1;
    }

    // 4. The device can still make progress. Booting is not enough: a device
    //    that can never write again is bricked in slow motion.
    boot_record_t next = got;
    next.last_result = BR_RESULT_OK;
    if (boot_record_append(&fresh, &next) != FLASH_OK) {
        snprintf(why, why_len, "cannot append after recovery");
        return 1;
    }
    boot_record_t after;
    if (!boot_record_load(&fresh, &after) || after.seq != got.seq + 1u) {
        snprintf(why, why_len, "post-recovery append not visible");
        return 1;
    }
    return 0;
}

static void test_exhaustive_sweep(void) {
    long total = count_operations();
    printf("  sweeping %ld flash operations x 2 cut modes\n", total);

    long failures = 0;
    for (long cut = 1; cut <= total; cut++) {
        for (int tear = 0; tear < 2; tear++) {
            char why[128] = {0};
            if (run_one_cut(cut, tear != 0, why, sizeof(why)) != 0) {
                printf("  FAIL cut=%ld tear=%d: %s\n", cut, tear, why);
                failures++;
            }
        }
    }
    CHECK_EQ(failures, 0);
}

// Page rotation is the most dangerous moment in the design: an erase is in
// flight while the only valid record lives in the page being left. Swept
// separately so a regression here is obvious in the log rather than buried.
static void test_rotation_boundary_sweep(void) {
    long failures = 0;
    int cuts_tested = 0;

    for (int tear = 0; tear < 2; tear++) {
        sim_flash_t sf;
        flash_if_t fif;
        br_ctx_t ctx;
        boot_record_t r;
        memset(&r, 0, sizeof(r));
        r.slot_state[0] = SLOT_CONFIRMED;
        r.security_floor = 1;

        // Fill page A to exactly full, cleanly
        sim_flash_init(&sf);
        sim_flash_bind(&sf, &fif);
        boot_record_init(&ctx, &fif, PAGE_A, PAGE_B);
        boot_record_factory_init(&ctx, &r);
        for (uint32_t i = 1; i < BR_PER_PAGE; i++) {
            r.security_floor = i;
            boot_record_append(&ctx, &r);
        }

        // Snapshot so every cut point starts from the same state. sim_flash_t
        // holds no internal pointers, so a struct copy is a valid snapshot.
        sim_flash_t saved = sf;

        // The next append must rotate: erase page B, then write there
        for (long cut = 1; cut <= 6; cut++) {
            sf = saved;
            sim_flash_bind(&sf, &fif); // ctx points at sf: rebind after copying

            br_ctx_t c2;
            boot_record_init(&c2, &fif, PAGE_A, PAGE_B);
            boot_record_t tmp;
            boot_record_load(&c2, &tmp);

            sim_flash_arm_cut(&sf, cut, tear != 0);
            boot_record_t rot = r;
            rot.security_floor = 0xAAAA;
            boot_record_append(&c2, &rot);

            sim_flash_reboot(&sf);
            br_ctx_t fresh;
            boot_record_init(&fresh, &fif, PAGE_A, PAGE_B);
            boot_record_t got;
            cuts_tested++;

            if (!boot_record_load(&fresh, &got)) {
                printf("  FAIL rotation cut=%ld tear=%d: no valid record\n", cut, tear);
                failures++;
                continue;
            }
            // Either the rotation completed or it did not. A mixture would mean
            // the design is broken.
            if (got.security_floor != 0xAAAA && got.security_floor != BR_PER_PAGE - 1u) {
                printf("  FAIL rotation cut=%ld tear=%d: floor=%u\n", cut, tear,
                       got.security_floor);
                failures++;
                continue;
            }
            boot_record_t next = got;
            if (boot_record_append(&fresh, &next) != FLASH_OK) {
                printf("  FAIL rotation cut=%ld tear=%d: stuck after recovery\n", cut,
                       tear);
                failures++;
            }
        }
    }
    printf("  rotation boundary: %d cut points tested\n", cuts_tested);
    CHECK_EQ(failures, 0);
}

TEST_MAIN_BEGIN("power-fail sweep")
RUN(test_exhaustive_sweep);
RUN(test_rotation_boundary_sweep);
TEST_MAIN_END()