#include <stdint.h>

// LD2, the user LED on the NUCLEO-G474RE, is on PA5
#define RCC_AHB2ENR (*(volatile uint32_t *)0x4002104Cu)
#define GPIOA_MODER (*(volatile uint32_t *)0x48000000u)
#define GPIOA_ODR (*(volatile uint32_t *)0x48000014u)

#define LED_PIN 5u

int main(void) {
    // Every peripheral is clock-gated off at reset. Enable GPIOA first.
    // Writing to its registers before this has no effect at all.
    RCC_AHB2ENR |= (1u << 0);

    // MODER holds 2 bits per pin. 01 is general-purpose output
    GPIOA_MODER &= ~(3u << (LED_PIN * 2));
    GPIOA_MODER |= (1u << (LED_PIN * 2));

    for (;;) {
        GPIOA_ODR ^= (1u << LED_PIN);
        // Crude delay. The core runs at 16 MHz from HSI after reset, so this
        // gives a visible blink. A timer comes later.
        for (volatile uint32_t i = 0; i < 400000u; i++) {
        }
    }
}