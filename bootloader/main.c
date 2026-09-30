#include <stdint.h>

#include "boot_handoff.h"
#include "boot_record.h"
#include "flash_stm32.h"

// Slot and boot state addresses, from the flash map in docs/DESIGN.md
// section 4. Slot A is in bank 1 and slot B in bank 2, so the update agent
// always writes to a bank it is not executing from.
#define SLOT_A_BASE 0x08010000u
#define SLOT_B_BASE 0x08040000u
#define SLOT_SIZE (192u * 1024u)

// Two 2 KB pages used as a ping-pong pair: when one fills, the other is
// erased and writing continues there.
#define BOOT_STATE_PAGE_A 0x0800F000u
#define BOOT_STATE_PAGE_B 0x0800F800u

// The application's vector table is not at the slot base. The first 512 bytes
// hold the image header, so the table starts at +0x200. The offset is not
// arbitrary: VTOR requires the vector table to be aligned to a power of 2 at
// least as large as the table itself, and the G474's table needs 512 bytes.
#define APP_VECTOR_OFFSET 0x200u

// SRAM1 + SRAM2, contiguous from 0x20000000. Used to sanity-check the stack
// pointer we are about to install.
#define SRAM_BASE 0x20000000u
#define SRAM_SIZE (96u * 1024u)

// Cortex-M4 registers we touch directly. Using literal addresses rather than
// CMSIS keeps the bootloader free of external dependencies, which matters for
// code that can never be updated in the field.
#define SCB_VTOR (*(volatile uint32_t *)0xE000ED08u)
#define SYSTICK_CTRL (*(volatile uint32_t *)0xE000E010u)
#define NVIC_ICER ((volatile uint32_t *)0xE000E180u) // interrupt clear-enable
#define NVIC_ICPR ((volatile uint32_t *)0xE000E280u) // interrupt clear-pending

// Independent watchdog. Clocked from the ~32 kHz LSI, which starting
// the watchdog turns on by itself. Once started, only a reset stops it.
#define IWDG_KR (*(volatile uint32_t *)0x40003000u)
#define IWDG_PR (*(volatile uint32_t *)0x40003004u)
#define IWDG_RLR (*(volatile uint32_t *)0x40003008u)
#define IWDG_SR (*(volatile uint32_t *)0x4000300Cu)
#define IWDG_KEY_START 0xCCCCu
#define IWDG_KEY_UNLOCK 0x5555u
#define IWDG_KEY_REFRESH 0xAAAAu

// 32 kHz / 128 = 250 Hz, and 1000 ticks at 250 Hz is 4 s. It has to
// cover worst-case verification plus application startup; revisit
// once M2 measures signature verification.
#define IWDG_PR_DIV128 5u
#define IWDG_RELOAD 999u

#define RCC_CSR (*(volatile uint32_t *)0x40021094u)
#define RCC_CSR_RMVF (1u << 23) // write 1 to clear all reset flags

static void watchdog_start(void) {
    IWDG_KR = IWDG_KEY_START;  // counting starts now, at the default timeout
    IWDG_KR = IWDG_KEY_UNLOCK; // allow writes to PR and RLR
    IWDG_PR = IWDG_PR_DIV128;
    IWDG_RLR = IWDG_RELOAD;
    // The new values cross into the slow LSI clock domain. SR reads zero once
    // they have landed. If the LSI never runs this loops forever, but the
    // watchdog is already counting and resets the part.
    while (IWDG_SR != 0u) {
    }
    IWDG_KR = IWDG_KEY_REFRESH;
}

// The reset flags are sticky: they accumulate until cleared. Reading and then
// clearing them means the next boot sees only the cause of its own reset.
static void capture_reset_cause(void) {
    uint32_t flags = RCC_CSR & RESET_FLAG_MASK;
    RCC_CSR |= RCC_CSR_RMVF;

    // TEMPORARY DIAGNOSTIC: count boots and accumulate reset flags since
    // power-on, in the spare handoff words.
    // volatile uint32_t *diag = (volatile uint32_t *)(BOOT_HANDOFF_ADDR + 8u);
    // if (BOOT_HANDOFF->magic == BOOT_HANDOFF_MAGIC && diag[2] == 0xD1A60000u) {
    //    diag[0]++;
    //    diag[1] |= flags;
    //} else {
    //    diag[0] = 1u;
    //    diag[1] = flags;
    //    diag[2] = 0xD1A60000u;
    //}
    BOOT_HANDOFF->reset_flags = flags;
    BOOT_HANDOFF->magic = BOOT_HANDOFF_MAGIC;
}

static uint32_t slot_base(uint8_t slot) {
    return (slot == 0u) ? SLOT_A_BASE : SLOT_B_BASE;
}

// Everything a real verification will eventually do lives here. For now it is
// only the two structural checks from section 7: the stack pointer must land in
// SRAM and the reset vector must point inside the slot with its Thumb bit set.
static int slot_is_bootable(uint8_t slot) {
    uint32_t base = slot_base(slot);
    const uint32_t *vt = (const uint32_t *)(base + APP_VECTOR_OFFSET);
    uint32_t sp = vt[0];
    uint32_t pc = vt[1];

    // The stack pointer must land inside SRAM. A slot that was never
    // programmed reads as 0xFFFFFFFF and fails here.
    if (sp < SRAM_BASE || sp > (SRAM_BASE + SRAM_SIZE)) {
        return 0;
    }

    // The reset vector must point inside the slot, with bit 0 set. The
    // Cortex-M only executes Thumb code, and it signals that by setting the
    // low bit of every function pointer. A cleared bit would fault instantly.
    if ((pc & 1u) == 0u) {
        return 0;
    }
    if (pc < base || pc > (base + SLOT_SIZE)) {
        return 0;
    }
    return 1;
}

static void jump_to_app(uint32_t vector_addr) {
    // Give the application a full timeout, however long slot selection took
    IWDG_KR = IWDG_KEY_REFRESH;

    const uint32_t *vt = (const uint32_t *)vector_addr;
    uint32_t sp = vt[0];
    uint32_t pc = vt[1];

    // Hand over a machine close to its reset state. Anything left enabled
    // keeps running after the jump, and an interrupt firing before the
    // application has installed its own handlers is a hard fault with a
    // confusing stack trace.
    __asm volatile("cpsid i" ::: "memory"); // interrupts off while we switch

    SYSTICK_CTRL = 0;

    // The G474 has around 100 peripheral interrupts, spread across 4 NVIC
    // registers of 32 bits each. Disable and un-pend all of them: a pending
    // interrupt survives being disabled and would fire the moment the
    // application enables that line.
    for (int i = 0; i < 4; i++) {
        NVIC_ICER[i] = 0xFFFFFFFFu;
        NVIC_ICPR[i] = 0xFFFFFFFFu;
    }

    // Point the core at the application's vector table before its code runs
    SCB_VTOR = vector_addr;

    // Barriers: make the VTOR write visible and flush the pipeline before
    // anything depends on the new table.
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");

    // Install the application's stack pointer, re-enable interrupts, and
    // branch.
    __asm volatile("msr msp, %0\n"
                   "cpsie i\n"
                   "bx %1\n"
                   :
                   : "r"(sp), "r"(pc)
                   : "memory");

    __builtin_unreachable();
}

int main(void) {
    // Started before anything else, so a hang anywhere after this point,
    // including inside verification, still ends in a reset the next boot
    // can count.
    watchdog_start();
    capture_reset_cause();

    flash_if_t flash;
    flash_stm32_bind(&flash);

    br_ctx_t ctx;
    boot_record_init(&ctx, &flash, BOOT_STATE_PAGE_A, BOOT_STATE_PAGE_B);

    boot_record_t rec;
    if (!boot_record_load(&ctx, &rec)) {
        // Factory path: no valid record exists. If slot A holds something
        // bootable, adopt it and write the first record.
        if (!slot_is_bootable(0)) {
            for (;;) {
            }
        }
        boot_record_t first = {0};
        first.primary_slot = 0;
        first.slot_state[0] = SLOT_CONFIRMED;
        first.slot_state[1] = SLOT_EMPTY;
        first.security_floor = 0;
        first.last_result = BR_RESULT_OK;
        if (boot_record_factory_init(&ctx, &first) != FLASH_OK) {
            for (;;) {
            }
        }
        rec = first;
    }

    uint8_t primary = rec.primary_slot;
    uint8_t other = primary ^ 1u;

    // A slot awaiting or undergoing trial takes priority over the primary
    for (uint8_t s = 0; s < BR_SLOT_COUNT; s++) {
        uint8_t state = rec.slot_state[s];
        if (state != SLOT_PENDING && state != SLOT_TRIAL) {
            continue;
        }

        int reject = 0;
        uint32_t reason = BR_RESULT_NONE;

        // The attempt counter is incremented before jumping, so a crash at
        // any point still consumes an attempt.
        if (state == SLOT_TRIAL && rec.trial_attempts >= BR_MAX_TRIALS) {
            reject = 1;
            reason = BR_RESULT_NOT_CONFIRMED;
        } else if (!slot_is_bootable(s)) {
            reject = 1;
            reason = BR_RESULT_BAD_HEADER;
        }

        boot_record_t next = rec;
        if (reject) {
            next.slot_state[s] = SLOT_REJECTED;
            next.last_result = reason;
            (void)boot_record_append(&ctx, &next);
            rec = next;
            break;
        }

        // A PENDING image is starting a fresh trial, so the count restarts.
        // Carrying over attempts from an earlier, rejected image would give
        // this one fewer than BR_MAX_TRIALS chances. The bootloader enforces
        // this itself rather than trusting the application to have reset it.
        next.slot_state[s] = SLOT_TRIAL;
        next.trial_attempts =
            (state == SLOT_PENDING) ? 1u : (uint8_t)(rec.trial_attempts + 1u);
        if (boot_record_append(&ctx, &next) == FLASH_OK) {
            jump_to_app(slot_base(s) + APP_VECTOR_OFFSET);
        }
        break;
    }

    // Normal path. A confirmed primary that checks out is booted with no
    // flash write at all, which matters for wear and energy on a device that
    // reboots often.
    if (rec.slot_state[primary] == SLOT_CONFIRMED && slot_is_bootable(primary)) {
        jump_to_app(slot_base(primary) + APP_VECTOR_OFFSET);
    }

    // Fall back to the other slot, recording the switch
    if (rec.slot_state[other] == SLOT_CONFIRMED && slot_is_bootable(other)) {
        boot_record_t next = rec;
        next.primary_slot = other;
        next.last_result = BR_RESULT_OK;
        (void)boot_record_append(&ctx, &next);
        jump_to_app(slot_base(other) + APP_VECTOR_OFFSET);
    }

    // Nothing bootable. Section 7 calls for a halt with an error indication;
    // recovery is over SWD by design, since a recovery mode would mean
    // transport code in the bootloader.
    for (;;) {
    }
}