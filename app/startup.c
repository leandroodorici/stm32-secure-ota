#include <stdint.h>

// Symbols defined by the linker script. They are addresses:
// what matters is where each one sits, not what is stored there.
// Declaring them as arrays to avoid needing &.
extern uint32_t _estack;
extern uint32_t _sidata, _sdata, _edata;
extern uint32_t _sbss, _ebss;

int main(void);

// Coprocessor access control: CP10 and CP11 are the FPU
#define SCB_CPACR (*(volatile uint32_t *)0xE000ED88u)
#define CPACR_CP10_CP11_FULL (0xFu << 20)

void Reset_Handler(void) {
    // The application is built for the hardware FPU, and the FreeRTOS port
    // saves FPU registers on every context switch. Enable the FPU before any
    // code can use it, and let the change take effect before continuing.
    SCB_CPACR |= CPACR_CP10_CP11_FULL;
    __asm volatile("dsb" ::: "memory");
    __asm volatile("isb" ::: "memory");
    // Copy initialised globals from flash to RAM. Before this runs, any
    // global with a non-zero initialiser holds garbage.
    uint32_t *src = &_sidata;
    for (uint32_t *dst = &_sdata; dst < &_edata;) {
        *dst++ = *src++;
    }

    // Zero the uninitialised globals
    for (uint32_t *p = &_sbss; p < &_ebss;) {
        *p++ = 0;
    }

    main();

    // main() must not return. If it does, stop here rather than executing
    // whatever bytes follow in flash.
    for (;;) {
    }
}

// A default handler for every exception we have not implemented. Spinning
// rather than returning means a fault is visible under the debugger instead
// of silently resuming into undefined behaviour.
void Default_Handler(void) {
    for (;;) {
    }
}

// Every handler aliases Default_Handler unless something else defines it.
// The weak attribute lets a strong definition elsewhere win at link time.
#define WEAK_ALIAS __attribute__((weak, alias("Default_Handler")))

void NMI_Handler(void) WEAK_ALIAS;
void HardFault_Handler(void) WEAK_ALIAS;
void MemManage_Handler(void) WEAK_ALIAS;
void BusFault_Handler(void) WEAK_ALIAS;
void UsageFault_Handler(void) WEAK_ALIAS;
void SVC_Handler(void) WEAK_ALIAS;
void DebugMon_Handler(void) WEAK_ALIAS;
void PendSV_Handler(void) WEAK_ALIAS;
void SysTick_Handler(void) WEAK_ALIAS;

// Peripheral interrupts the application handles. Like the core exceptions,
// each defaults to Default_Handler until a driver defines it.
void LPUART1_IRQHandler(void) WEAK_ALIAS;

// The vector table. The linker script places .isr_vector at the start of
// flash, because the core reads the first two words at reset: the initial
// stack pointer, then the address of the reset handler.
//
// The 16 core exceptions are followed by the G474's peripheral interrupts, up
// to the highest one in use: LPUART1, interrupt 91, at index 16 + 91. Every
// other entry points at Default_Handler, so an interrupt enabled by mistake
// stops visibly instead of jumping to address 0. At 108 entries the table
// stays within the 512 bytes that VTOR alignment allows after the header.
#define IRQ_LPUART1 91

__attribute__((section(".isr_vector"), used)) const void *const vector_table[] = {
    &_estack,      // 0: initial stack pointer
    Reset_Handler, // 1: reset
    NMI_Handler,
    HardFault_Handler,
    MemManage_Handler,
    BusFault_Handler,
    UsageFault_Handler,
    0,
    0,
    0,
    0, // reserved
    SVC_Handler,
    DebugMon_Handler,
    0, // reserved
    PendSV_Handler,
    SysTick_Handler,
    // GNU range designator: every peripheral entry below LPUART1's
    [16 ... 16 + IRQ_LPUART1 - 1] = Default_Handler,
    [16 + IRQ_LPUART1] = LPUART1_IRQHandler,
};