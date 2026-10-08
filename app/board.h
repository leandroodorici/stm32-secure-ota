#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>

// The NUCLEO-G474RE's user I/O and the breadboard status LED. Pins and the
// checks behind them are in docs/HARDWARE.md.
//
//   RGB LED, common cathode, one resistor per channel:
//      red D7, green D6, blue D4
//   LD2, the on-board green LED
//   B1, the blue user button: reads 1 while pressed

// Bit 0 red, bit 1 green, bit 2 blue, so mixed colours are the sum of their
// channels
typedef enum {
    RGB_OFF = 0u,
    RGB_RED = 1u,
    RGB_GREEN = 2u,
    RGB_YELLOW = 3u,
    RGB_BLUE = 4u,
    RGB_MAGENTA = 5u,
    RGB_CYAN = 6u,
    RGB_WHITE = 7u
} rgb_t;

// Clocks the GPIO ports and configures every pin above. Called once, before
// the scheduler starts.
void board_init(void);

void board_rgb(rgb_t colour);
void board_ld2_toggle(void);
uint32_t board_button_pressed(void);

#endif // BOARD_H
