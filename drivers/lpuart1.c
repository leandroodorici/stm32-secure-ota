#include "lpuart1.h"

// See lpuart1.h. Register layout from RM0440 and ST's stm32g474xx.h

#define RCC_AHB2ENR (*(volatile uint32_t *)0x4002104Cu)
#define RCC_APB1ENR2 (*(volatile uint32_t *)0x4002105Cu)
#define RCC_AHB2ENR_GPIOA (1u << 0)
#define RCC_APB1ENR2_LPUART1 (1u << 0)

#define GPIOA_MODER (*(volatile uint32_t *)0x48000000u)
#define GPIOA_AFRL (*(volatile uint32_t *)0x48000020u)

#define LPUART1_BASE 0x40008000u
#define LPUART_CR1 (*(volatile uint32_t *)(LPUART1_BASE + 0x00u))
#define LPUART_BRR (*(volatile uint32_t *)(LPUART1_BASE + 0x0Cu))
#define LPUART_ISR (*(volatile uint32_t *)(LPUART1_BASE + 0x1Cu))
#define LPUART_ICR (*(volatile uint32_t *)(LPUART1_BASE + 0x20u))
#define LPUART_RDR (*(volatile uint32_t *)(LPUART1_BASE + 0x24u))
#define LPUART_TDR (*(volatile uint32_t *)(LPUART1_BASE + 0x28u))

#define CR1_UE (1u << 0)
#define CR1_RE (1u << 2)
#define CR1_TE (1u << 3)
#define CR1_RXNEIE (1u << 5)

#define ISR_FE (1u << 1)
#define ISR_NE (1u << 2)
#define ISR_ORE (1u << 3)
#define ISR_RXNE (1u << 5)
#define ISR_TXE (1u << 7)

#define ICR_FECF (1u << 1)
#define ICR_NECF (1u << 2)
#define ICR_ORECF (1u << 3)

#define NVIC_ISER(n) (*(volatile uint32_t *)(0xE000E100u + 4u * (n)))
#define NVIC_IPR(irq) (*(volatile uint8_t *)(0xE000E400u + (irq)))

#define LPUART1_IRQ 91u
// Numerically at or above configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY (5),
// so the handler may call FreeRTOS FromISR functions. The G474 implements
// the top 4 bits of each priority byte.
#define LPUART1_PRIORITY 6u

// The kernel clock is PCLK1, the reset default, which runs at the 16 MHz
// reset clock. 16 MHz x 256 still fits in 32 bits for the divider below.
#define LPUART1_CLOCK_HZ 16000000u

// PA2 and PA3 use alternate function 12 for LPUART1
#define PA2_PA3_AF 12u

static lpuart1_rx_fn g_on_rx;

void lpuart1_init(uint32_t baud, lpuart1_rx_fn on_rx) {
    g_on_rx = on_rx;

    RCC_AHB2ENR |= RCC_AHB2ENR_GPIOA;
    RCC_APB1ENR2 |= RCC_APB1ENR2_LPUART1;

    // Four bits of AFRL per pin, two bits of MODER per pin (10 = alternate)
    GPIOA_AFRL = (GPIOA_AFRL & ~(0xFFu << 8)) | (PA2_PA3_AF << 8) | (PA2_PA3_AF << 12);
    GPIOA_MODER = (GPIOA_MODER & ~(0xFu << 4)) | (0xAu << 4);

    // The LPUART divider is 256 x clock / baud, rounded to nearest
    LPUART_CR1 = 0;
    LPUART_BRR = (LPUART1_CLOCK_HZ * 256u + baud / 2u) / baud;
    LPUART_CR1 = CR1_TE | CR1_RE | CR1_RXNEIE;
    LPUART_CR1 |= CR1_UE;

    NVIC_IPR(LPUART1_IRQ) = (uint8_t)(LPUART1_PRIORITY << 4);
    NVIC_ISER(LPUART1_IRQ / 32u) = 1u << (LPUART1_IRQ % 32u);
}

// Overrides the weak default in the application's startup.c
void LPUART1_IRQHandler(void) {
    uint32_t isr = LPUART_ISR;

    // An overrun, framing or noise error loses or damages a byte. Clear it
    // and carry on: the frame CRC above catches the damage, and with the
    // receive interrupt enabled an uncleared overrun would fire forever.
    if ((isr & (ISR_ORE | ISR_FE | ISR_NE)) != 0u) {
        LPUART_ICR = ICR_ORECF | ICR_FECF | ICR_NECF;
    }
    if ((isr & ISR_RXNE) != 0u) {
        uint8_t byte = (uint8_t)LPUART_RDR; // reading clears RXNE
        if (g_on_rx != 0) {
            g_on_rx(byte);
        }
    }
}

void lpuart1_write(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        while ((LPUART_ISR & ISR_TXE) == 0u) {
        }
        LPUART_TDR = data[i];
    }
}
