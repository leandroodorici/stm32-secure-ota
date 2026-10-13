#include <stdio.h>
#include <string.h>

#include "progress.h"
#include "sim_flash.h"
#include "test_util.h"

// The progress log decides where an interrupted transfer resumes. Resuming
// too early only costs time; resuming too late, past bytes that were never
// written, would assemble an image with a hole in it. So beyond the basics,
// these tests cut power at every flash operation of a run of saves and check
// that the log never claims more than was actually saved.

#define PAGE_A (SIM_BASE + 2u * FLASH_PAGE_SIZE)
#define PAGE_B (SIM_BASE + 3u * FLASH_PAGE_SIZE)

static sim_flash_t g_sf;
static flash_if_t g_fif;
static progress_log_t g_log;

static void setup(void) {
    sim_flash_init(&g_sf);
    sim_flash_bind(&g_sf, &g_fif);
    progress_init(&g_log, &g_fif, PAGE_A, PAGE_B);
}

static progress_t make(uint32_t offset, uint8_t tag) {
    progress_t p;
    memset(&p, 0, sizeof(p));
    p.image_size = 100000;
    for (uint32_t i = 0; i < PG_DIGEST_LEN; i++) {
        p.digest[i] = (uint8_t)(tag + i);
    }
    p.offset = offset;
    return p;
}

static int same(const progress_t *a, const progress_t *b) {
    return a->image_size == b->image_size && a->offset == b->offset &&
           memcmp(a->digest, b->digest, PG_DIGEST_LEN) == 0;
}

// ---- basics ----

static void test_empty_log_has_no_record(void) {
    setup();
    progress_t p;
    CHECK(!progress_load(&g_log, &p));
}

static void test_newest_record_wins(void) {
    setup();
    progress_t a = make(2048, 1);
    progress_t b = make(4096, 1);
    CHECK_EQ(progress_save(&g_log, &a), FLASH_OK);
    CHECK_EQ(progress_save(&g_log, &b), FLASH_OK);

    progress_t got;
    CHECK(progress_load(&g_log, &got));
    CHECK(same(&got, &b));
}

static void test_survives_many_page_rotations(void) {
    // 200 saves fill a page three times over
    setup();
    progress_t last;
    for (uint32_t i = 1; i <= 200u; i++) {
        last = make(i * 2048u, 7);
        CHECK_EQ(progress_save(&g_log, &last), FLASH_OK);
    }
    progress_t got;
    CHECK(progress_load(&g_log, &got));
    CHECK(same(&got, &last));
}

static void test_damaged_entries_are_skipped(void) {
    // A record whose CRC no longer matches, as a torn write would leave, is
    // ignored, and saving continues past it
    setup();
    progress_t a = make(2048, 3);
    CHECK_EQ(progress_save(&g_log, &a), FLASH_OK);
    g_sf.mem[(PAGE_A - SIM_BASE) + 9u] ^= 0x01u;

    progress_t got;
    CHECK(!progress_load(&g_log, &got));

    progress_t b = make(4096, 3);
    CHECK_EQ(progress_save(&g_log, &b), FLASH_OK);
    CHECK(progress_load(&g_log, &got));
    CHECK(same(&got, &b));
}

static void test_full_page_of_damage_is_recovered(void) {
    // A page entirely of non-erased garbage, with no valid record anywhere:
    // saving must still succeed rather than finding nowhere to write
    setup();
    memset(g_sf.mem + (PAGE_A - SIM_BASE), 0x5A, FLASH_PAGE_SIZE);
    progress_t a = make(2048, 9);
    CHECK_EQ(progress_save(&g_log, &a), FLASH_OK);
    progress_t got;
    CHECK(progress_load(&g_log, &got));
    CHECK(same(&got, &a));
}

// ---- power cuts ----
//
// A run of saves, with the log already holding enough records that the run
// crosses a page rotation. Every flash operation of the run is cut, cleanly
// and mid-operation. After each cut, the newest record must be either the
// last save that completed or the one that was in flight: never anything
// older, never anything that was not saved.

#define PREFILL (PG_PER_PAGE - 3u)
#define SAVES 8u

static long run_saves(long cut, int tear, uint32_t *completed) {
    setup();
    for (uint32_t i = 1; i <= PREFILL; i++) {
        progress_t p = make(i * 2048u, 1);
        (void)progress_save(&g_log, &p);
    }
    g_sf.ops = 0;
    sim_flash_arm_cut(&g_sf, cut, tear != 0);

    *completed = PREFILL;
    for (uint32_t i = PREFILL + 1u; i <= PREFILL + SAVES; i++) {
        progress_t p = make(i * 2048u, 1);
        if (progress_save(&g_log, &p) != FLASH_OK || !g_sf.powered) {
            break;
        }
        *completed = i;
    }
    return g_sf.ops;
}

static void test_power_cut_at_every_operation(void) {
    uint32_t completed;
    long total = run_saves(-1, 0, &completed);
    CHECK_EQ(completed, PREFILL + SAVES);
    printf("  sweeping %ld flash operations x 2 cut modes\n", total);

    long failures = 0;
    for (long cut = 1; cut <= total; cut++) {
        for (int tear = 0; tear < 2; tear++) {
            (void)run_saves(cut, tear, &completed);
            sim_flash_reboot(&g_sf);

            progress_t got;
            uint32_t offset = 0;
            if (progress_load(&g_log, &got)) {
                offset = got.offset;
            }
            uint32_t lo = completed * 2048u;
            uint32_t hi = (completed + 1u) * 2048u;
            if (offset != lo && offset != hi) {
                printf("  FAIL cut=%ld tear=%d: offset %u, saved up to %u\n", cut, tear,
                       offset, lo);
                failures++;
                continue;
            }

            // And the log keeps working after recovery
            progress_t next = make(hi + 2048u, 1);
            if (progress_save(&g_log, &next) != FLASH_OK ||
                !progress_load(&g_log, &got) || !same(&got, &next)) {
                printf("  FAIL cut=%ld tear=%d: cannot save after recovery\n", cut, tear);
                failures++;
            }
        }
    }
    CHECK_EQ(failures, 0);
}

TEST_MAIN_BEGIN("progress")
RUN(test_empty_log_has_no_record);
RUN(test_newest_record_wins);
RUN(test_survives_many_page_rotations);
RUN(test_damaged_entries_are_skipped);
RUN(test_full_page_of_damage_is_recovered);
RUN(test_power_cut_at_every_operation);
TEST_MAIN_END()
