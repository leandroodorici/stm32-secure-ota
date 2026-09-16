#include <string.h>

#include "sim_flash.h"
#include "test_util.h"

// Tests for the simulator itself.
//
// Every correctness claim in this project rests on this model being faithful
// to the STM32G4 flash controller. If it is more permissive than the
// real part, the power-fail sweep proves nothing: code could pass here and
// still brick a device.
//
// Each test below corresponds to a documented controller behaviour taken from
// RM0440. docs/DESIGN.md section 12 tracks which of these still need
// confirming on actual silicon.

#define PAGE0 (SIM_BASE + 0u * FLASH_PAGE_SIZE)
#define PAGE1 (SIM_BASE + 1u * FLASH_PAGE_SIZE)

static sim_flash_t sf;
static flash_if_t f;

static void setup(void)
{
    sim_flash_init(&sf);
    sim_flash_bind(&sf, &f);
}

static uint64_t read_dw(uint32_t addr)
{
    uint64_t v = 0;
    flash_read(&f, addr, &v, sizeof(v));
    return v;
}

// ---- basic semantics ----

static void test_starts_erased(void)
{
    setup();
    CHECK_EQ(read_dw(PAGE0), (long long)FLASH_ERASED_DW);
    CHECK_EQ(read_dw(PAGE1 + 512u), (long long)FLASH_ERASED_DW);
}

static void test_program_then_read(void)
{
    setup();
    CHECK_EQ(flash_program_dw(&f, PAGE0, 0x0123456789ABCDEFull), FLASH_OK);
    CHECK_EQ(read_dw(PAGE0), (long long)0x0123456789ABCDEFull);
    CHECK_EQ(sf.programs, 1);
}

static void test_erase_restores_ff(void)
{
    setup();
    flash_program_dw(&f, PAGE0, 0x1122334455667788ull);
    flash_program_dw(&f, PAGE0 + 64u, 0x99AABBCCDDEEFF00ull);
    CHECK_EQ(flash_erase_page(&f, PAGE0), FLASH_OK);
    CHECK_EQ(read_dw(PAGE0), (long long)FLASH_ERASED_DW);
    CHECK_EQ(read_dw(PAGE0 + 64u), (long long)FLASH_ERASED_DW);
    CHECK_EQ(sf.erases, 1);
}

static void test_erase_is_page_scoped(void)
{
    // Page rotation erases the spare page while the only valid record lives in
    // the other one. If erase bled across pages, that design would be unsafe.
    setup();
    flash_program_dw(&f, PAGE1, 0xAAAAAAAAAAAAAAAAull);
    flash_erase_page(&f, PAGE0);
    CHECK_EQ(read_dw(PAGE1), (long long)0xAAAAAAAAAAAAAAAAull);
}

// ---- the constraint that shapes the design ----

static void test_cannot_reprogram_written_doubleword(void)
{
    // The most important behaviour to model correctly. The G4 stores an ECC
    // syndrome per double-word, so a second write would leave data and
    // syndrome inconsistent and the controller refuses it.
    //
    // This is why boot state is append-only rather than a mutable struct with
    // a confirmed flag cleared in place.
    setup();
    CHECK_EQ(flash_program_dw(&f, PAGE0, 0x1111111111111111ull), FLASH_OK);
    CHECK_EQ(flash_program_dw(&f, PAGE0, 0x2222222222222222ull), FLASH_ERR_NOT_ERASED);
    // The refused write leaves the original value untouched.
    CHECK_EQ(read_dw(PAGE0), (long long)0x1111111111111111ull);
}

static void test_cannot_even_clear_more_bits(void)
{
    // Separate from the test above because this is where intuition from plain
    // NOR flash misleads. Without ECC you may clear further bits in place,
    // which is how a confirmed flag is often implemented. With a syndrome per
    // double-word, that option does not exist.
    setup();
    flash_program_dw(&f, PAGE0, 0xFFFFFFFFFFFFFFF0ull);
    CHECK_EQ(flash_program_dw(&f, PAGE0, 0xFFFFFFFFFFFFFF00ull), FLASH_ERR_NOT_ERASED);
}

static void test_program_after_erase_is_allowed(void)
{
    // The counterpart: erasing genuinely frees the location. Without this the
    // two tests above could pass because the model refuses everything.
    setup();
    flash_program_dw(&f, PAGE0, 0x1111111111111111ull);
    flash_erase_page(&f, PAGE0);
    CHECK_EQ(flash_program_dw(&f, PAGE0, 0x2222222222222222ull), FLASH_OK);
    CHECK_EQ(read_dw(PAGE0), (long long)0x2222222222222222ull);
}

// ---- alignment and bounds ----

static void test_rejects_misaligned_program(void)
{
    setup();
    CHECK_EQ(flash_program_dw(&f, PAGE0 + 4u, 0x1234ull), FLASH_ERR_ALIGN);
    CHECK_EQ(flash_program_dw(&f, PAGE0 + 1u, 0x1234ull), FLASH_ERR_ALIGN);
}

static void test_rejects_misaligned_erase(void)
{
    setup();
    CHECK_EQ(flash_erase_page(&f, PAGE0 + 8u), FLASH_ERR_ALIGN);
}

static void test_rejects_out_of_range(void)
{
    setup();
    CHECK_EQ(flash_program_dw(&f, SIM_BASE - 8u, 0ull), FLASH_ERR_RANGE);
    CHECK_EQ(flash_program_dw(&f, SIM_BASE + SIM_SIZE, 0ull), FLASH_ERR_RANGE);
    CHECK_EQ(flash_erase_page(&f, SIM_BASE + SIM_SIZE), FLASH_ERR_RANGE);

    // A read starting inside but running past the end must be refused rather
    // than truncated. Silently short reads are how parsers get confused.
    uint8_t buf[16];
    CHECK_EQ(flash_read(&f, SIM_BASE + SIM_SIZE - 8u, buf, sizeof(buf)),
             FLASH_ERR_RANGE);
}

// ---- fault injection ----

static void test_clean_cut_leaves_flash_untouched(void)
{
    setup();
    flash_program_dw(&f, PAGE0, 0x1111111111111111ull);

    sim_flash_arm_cut(&sf, 1, false);
    flash_program_dw(&f, PAGE0 + 8u, 0x2222222222222222ull);

    sim_flash_reboot(&sf);
    CHECK_EQ(read_dw(PAGE0), (long long)0x1111111111111111ull);
    CHECK_EQ(read_dw(PAGE0 + 8u), (long long)FLASH_ERASED_DW);
}

static void test_torn_write_raises_ecc_on_read(void)
{
    // The important one. A torn double-word must not read back as half-written
    // data, because code would then act on bytes that were never committed.
    setup();
    sim_flash_arm_cut(&sf, 1, true);
    flash_program_dw(&f, PAGE0, 0xDEADBEEFCAFEBABEull);
    sim_flash_reboot(&sf);

    uint64_t v;
    CHECK_EQ(flash_read(&f, PAGE0, &v, sizeof(v)), FLASH_ERR_ECC);
}

static void test_ecc_fault_covers_any_overlapping_read(void)
{
    // A read that merely touches a damaged double-word must fail rather than
    // returning the good bytes around it.
    setup();
    sim_flash_arm_cut(&sf, 1, true);
    flash_program_dw(&f, PAGE0 + 16u, 0x1122334455667788ull);
    sim_flash_reboot(&sf);

    uint8_t buf[32];
    CHECK_EQ(flash_read(&f, PAGE0, buf, sizeof(buf)), FLASH_ERR_ECC);
    // A read stopping before the damaged double-word is fine.
    CHECK_EQ(flash_read(&f, PAGE0, buf, 16u), FLASH_OK);
}

static void test_torn_doubleword_cannot_be_reprogrammed(void)
{
    // The space is spent until the page is erased. Any recovery design that
    // assumes it can rewrite the damaged entry in place is wrong.
    setup();
    sim_flash_arm_cut(&sf, 1, true);
    flash_program_dw(&f, PAGE0, 0xDEADBEEFCAFEBABEull);
    sim_flash_reboot(&sf);

    CHECK_EQ(flash_program_dw(&f, PAGE0, 0x1111111111111111ull), FLASH_ERR_NOT_ERASED);
    CHECK_EQ(flash_erase_page(&f, PAGE0), FLASH_OK);
    CHECK_EQ(flash_program_dw(&f, PAGE0, 0x1111111111111111ull), FLASH_OK);
}

static void test_nothing_happens_after_the_cut(void)
{
    // Once power is gone it stays gone until reboot. Without this a test could
    // accidentally recover by performing writes a dead device could not have.
    setup();
    sim_flash_arm_cut(&sf, 1, false);
    flash_program_dw(&f, PAGE0, 0x1111111111111111ull);
    flash_program_dw(&f, PAGE0 + 8u, 0x2222222222222222ull);
    flash_erase_page(&f, PAGE1);

    sim_flash_reboot(&sf);
    CHECK_EQ(read_dw(PAGE0), (long long)FLASH_ERASED_DW);
    CHECK_EQ(read_dw(PAGE0 + 8u), (long long)FLASH_ERASED_DW);
    CHECK_EQ(sf.programs, 0);
    CHECK_EQ(sf.erases, 0);
}

static void test_cut_lands_on_the_nth_operation(void)
{
    // The sweep in the next step enumerates cut points by number, so the
    // injector must interrupt exactly the operation asked for. An off-by-one
    // here would silently skip a cut point and nobody would notice.
    setup();
    sim_flash_arm_cut(&sf, 3, false);
    flash_program_dw(&f, PAGE0 + 0u, 1ull);  // op 1: runs
    flash_program_dw(&f, PAGE0 + 8u, 2ull);  // op 2: runs
    flash_program_dw(&f, PAGE0 + 16u, 3ull); // op 3: cut
    flash_program_dw(&f, PAGE0 + 24u, 4ull); // never happens

    sim_flash_reboot(&sf);
    CHECK_EQ(read_dw(PAGE0 + 0u), 1);
    CHECK_EQ(read_dw(PAGE0 + 8u), 2);
    CHECK_EQ(read_dw(PAGE0 + 16u), (long long)FLASH_ERASED_DW);
    CHECK_EQ(read_dw(PAGE0 + 24u), (long long)FLASH_ERASED_DW);
    CHECK_EQ(sf.programs, 2);
}

static void test_partial_erase_leaves_page_mixed(void)
{
    // A cut during erase leaves some of the page erased and some not, which is
    // why erase ordering matters in the boot state design.
    setup();
    for (uint32_t off = 0; off < FLASH_PAGE_SIZE; off += FLASH_DW_SIZE) {
        flash_program_dw(&f, PAGE0 + off, 0x5555555555555555ull);
    }
    sim_flash_arm_cut(&sf, 1, true);
    flash_erase_page(&f, PAGE0);
    sim_flash_reboot(&sf);

    CHECK_EQ(read_dw(PAGE0), (long long)FLASH_ERASED_DW);
    CHECK_EQ(read_dw(PAGE0 + FLASH_PAGE_SIZE - 8u), (long long)0x5555555555555555ull);
}

static void test_reboot_preserves_contents(void)
{
    // If the model got this wrong every recovery test would pass vacuously.
    setup();
    flash_program_dw(&f, PAGE0, 0xABCDEF0123456789ull);
    sim_flash_reboot(&sf);
    CHECK_EQ(read_dw(PAGE0), (long long)0xABCDEF0123456789ull);
}

TEST_MAIN_BEGIN("sim_flash")
RUN(test_starts_erased);
RUN(test_program_then_read);
RUN(test_erase_restores_ff);
RUN(test_erase_is_page_scoped);
RUN(test_cannot_reprogram_written_doubleword);
RUN(test_cannot_even_clear_more_bits);
RUN(test_program_after_erase_is_allowed);
RUN(test_rejects_misaligned_program);
RUN(test_rejects_misaligned_erase);
RUN(test_rejects_out_of_range);
RUN(test_clean_cut_leaves_flash_untouched);
RUN(test_torn_write_raises_ecc_on_read);
RUN(test_ecc_fault_covers_any_overlapping_read);
RUN(test_torn_doubleword_cannot_be_reprogrammed);
RUN(test_nothing_happens_after_the_cut);
RUN(test_cut_lands_on_the_nth_operation);
RUN(test_partial_erase_leaves_page_mixed);
RUN(test_reboot_preserves_contents);
TEST_MAIN_END()