#ifndef LPUART1_H
#define LPUART1_H

#include <stddef.h>
#include <stdint.h>

// LPUART1 on PA2 (TX) and PA3 (RX)
//
// Receive is interrupt-driven: every byte is handed to the callback from the
// interrupt handler. Transmit is polled.

// Called from interrupt context, once per received byte
typedef void (*lpuart1_rx_fn)(uint8_t byte);

// Configures the pins, the baud rate and the interrupt. The interrupt runs at
// a priority that allows FreeRTOS FromISR calls.
void lpuart1_init(uint32_t baud, lpuart1_rx_fn on_rx);

// Blocks until every byte has been handed to the transmitter
void lpuart1_write(const uint8_t *data, size_t len);

#endif // LPUART1_H
