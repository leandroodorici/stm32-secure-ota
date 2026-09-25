#include <stdint.h>

// Where slot A begins. From the flash map in docs/DESIGN.md section 4.
// Hard-coded for now: slot selection from boot records comes later.
#define SLOT_A_BASE 0x08010000u

// The application's vector table is not at the slot base. The first 512 bytes
// are reserved for the image header, so the table starts at +0x200. The offset
// is not arbitrary: VTOR requires the vector table to be aligned to a power of
// 2 at least as large as the table itself. G474's table needs 512-byte alignment.
//
// TODO(M2): restore to 0x200 once imgtool prepends a real header. Until then
// the application's vector table sits at the slot base, so the offset is 0 to
// keep the bootloader and the linker script in agreement.
#define APP_VECTOR_OFFSET 0u

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

static int app_looks_valid(uint32_t vector_addr) {
    const uint32_t *vt = (const uint32_t *)vector_addr;
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
    if (pc < SLOT_A_BASE || pc > (SLOT_A_BASE + 192u * 1024u)) {
        return 0;
    }
    return 1;
}

static void jump_to_app(uint32_t vector_addr) {
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
    uint32_t vectors = SLOT_A_BASE + APP_VECTOR_OFFSET;

    if (app_looks_valid(vectors)) {
        jump_to_app(vectors);
    }

    // No bootable application. Section 7 of DESIGN.md calls for a safe halt
    // with an error indication. For now, stop where a debugger can see it.
    for (;;) {
    }
}