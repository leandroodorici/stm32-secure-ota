#ifndef BOOT_HANDOFF_H
#define BOOT_HANDOFF_H

#include <stdint.h>

// Information the bootloader passes to the application across the jump.
//
// It lives in the first 32 bytes of SRAM. Both linker scripts start their RAM
// region after it, so neither startup routine zeroes it and no variable is
// ever placed on top of it.
#define BOOT_HANDOFF_ADDR 0x20000000u
#define BOOT_HANDOFF_MAGIC 0x31464F48u // "HOF1"

// Reset cause flags, in their RCC_CSR bit positions. Stored raw so the
// application and gateway see exactly what the hardware reported. More
// than one can be set: an internal reset also drives the NRST pin.
#define RESET_FLAG_OBL (1u << 25)  // option byte load
#define RESET_FLAG_PIN (1u << 26)  // NRST pin
#define RESET_FLAG_BOR (1u << 27)  // brownout, including power-on
#define RESET_FLAG_SW (1u << 28)   // software (SYSRESETREQ)
#define RESET_FLAG_IWDG (1u << 29) // independent watchdog
#define RESET_FLAG_WWDG (1u << 30) // window watchdog
#define RESET_FLAG_LPWR (1u << 31) // illegal low-power entry
#define RESET_FLAG_MASK 0xFE000000u

typedef struct {
    uint32_t magic; // BOOT_HANDOFF_MAGIC when the bootloader wrote this boot
    uint32_t reset_flags;
} boot_handoff_t;

#define BOOT_HANDOFF ((volatile boot_handoff_t *)BOOT_HANDOFF_ADDR)

#endif // BOOT_HANDOFF_H