#include <stdint.h>

#include "boot_handoff.h"

// LD2, the user LED on the NUCLEO-G474RE, is on PA5
#define RCC_AHB2ENR (*(volatile uint32_t *)0x4002104Cu)
#define GPIOA_MODER (*(volatile uint32_t *)0x48000000u)
#define GPIOA_ODR (*(volatile uint32_t *)0x48000014u)

// The bootloader has already started the independent watchdog and nothing can
// stop it, so from the first instruction the application must refresh it.
#define IWDG_KR (*(volatile uint32_t *)0x40003000u)
#define IWDG_KEY_REFRESH 0xAAAAu

#define LED_PIN 5u

int main(void) {
    // Blink fast after a watchdog reset, so the cause is visible without a
    // debugger. The magic check guards against an application started some
    // other way, where this RAM holds whatever was there before.
    uint32_t delay = 400000u;
    if (BOOT_HANDOFF->magic == BOOT_HANDOFF_MAGIC &&
        (BOOT_HANDOFF->reset_flags & RESET_FLAG_IWDG) != 0u) {
        delay = 100000u;
    }

    // Every peripheral is clock-gated off at reset. Enable GPIOA first.
    // Writing to its registers before this has no effect at all.
    RCC_AHB2ENR |= (1u << 0);

    // MODER holds 2 bits per pin. 01 is general-purpose output
    GPIOA_MODER &= ~(3u << (LED_PIN * 2));
    GPIOA_MODER |= (1u << (LED_PIN * 2));

    for (;;) {
        IWDG_KR = IWDG_KEY_REFRESH;
        GPIOA_ODR ^= (1u << LED_PIN);
        // Crude delay. The core runs at 16 MHz from HSI after reset, so this
        // gives a visible blink. A timer comes later.
        for (volatile uint32_t i = 0; i < delay; i++) {
        }
    }
}