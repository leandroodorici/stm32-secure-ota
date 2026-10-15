#include "board.h"

// See board.h for the pin assignment.

#define RCC_AHB2ENR (*(volatile uint32_t *)0x4002104Cu)
#define RCC_AHB2ENR_GPIOABC 0x7u

#define GPIOA 0x48000000u
#define GPIOB 0x48000400u
#define GPIOC 0x48000800u

#define GPIO_MODER(port) (*(volatile uint32_t *)((port) + 0x00u))
#define GPIO_IDR(port) (*(volatile uint32_t *)((port) + 0x10u))
#define GPIO_ODR(port) (*(volatile uint32_t *)((port) + 0x14u))
// Writing 1 to bit n sets pin n, and to bit n + 16 clears it. One store, so
// tasks driving different pins of the same port never undo each other's
// changes, as a read-modify-write of ODR could.
#define GPIO_BSRR(port) (*(volatile uint32_t *)((port) + 0x18u))

#define LD2_PIN 5u    // PA5
#define RED_PIN 8u    // PA8
#define GREEN_PIN 10u // PB10
#define BLUE_PIN 5u   // PB5
#define B1_PIN 13u    // PC13

// Cortex-M4 application interrupt and reset control register. Writes are
// ignored unless the upper half holds the key.
#define SCB_AIRCR (*(volatile uint32_t *)0xE000ED0Cu)
#define AIRCR_VECTKEY (0x05FAu << 16)
#define AIRCR_SYSRESETREQ (1u << 2)

// MODER holds two bits per pin: 00 input, 01 output. Every pin starts in
// analog mode (11) after reset, where the input reads as 0.
static void make_output(uint32_t port, uint32_t pin) {
    GPIO_MODER(port) &= ~(3u << (pin * 2u));
    GPIO_MODER(port) |= (1u << (pin * 2u));
}

static void make_input(uint32_t port, uint32_t pin) {
    GPIO_MODER(port) &= ~(3u << (pin * 2u));
}

static void write_pin(uint32_t port, uint32_t pin, uint32_t on) {
    GPIO_BSRR(port) = on ? (1u << pin) : (1u << (pin + 16u));
}

void board_init(void) {
    // Every peripheral is clock-gated off at reset. Writing to a port's
    // registers before its clock is on has no effect at all.
    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOABC;

    make_output(GPIOA, LD2_PIN);
    make_output(GPIOA, RED_PIN);
    make_output(GPIOB, GREEN_PIN);
    make_output(GPIOB, BLUE_PIN);
    // The board has its own resistor on B1, so no internal pull is needed
    make_input(GPIOC, B1_PIN);

    board_rgb(RGB_OFF);
}

void board_rgb(rgb_t colour) {
    uint32_t c = (uint32_t)colour;
    write_pin(GPIOA, RED_PIN, c & 1u);
    write_pin(GPIOB, GREEN_PIN, c & 2u);
    write_pin(GPIOB, BLUE_PIN, c & 4u);
}

void board_ld2_toggle(void) {
    write_pin(GPIOA, LD2_PIN, (GPIO_ODR(GPIOA) & (1u << LD2_PIN)) == 0u);
}

uint32_t board_button_pressed(void) {
    return (GPIO_IDR(GPIOC) & (1u << B1_PIN)) != 0u;
}

void board_reset(void) {
    // Outstanding writes complete first, so nothing is lost half-written
    __asm volatile("dsb" ::: "memory");
    SCB_AIRCR = AIRCR_VECTKEY | AIRCR_SYSRESETREQ;
    __asm volatile("dsb" ::: "memory");
    for (;;) {
    }
}
