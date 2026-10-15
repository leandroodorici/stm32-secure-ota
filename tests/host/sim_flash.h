#ifndef SIM_FLASH_H
#define SIM_FLASH_H

#include "flash_if.h"
#include <stdbool.h>
#include <stdint.h>

// Uses the real base address so addresses in tests match the flash map in
// docs/DESIGN.md. Eight pages are enough for the boot record and power-cut
// tests, which run thousands of times and reinitialise the simulator each
// time. A test that needs the real flash map builds it with SIM_PAGES=256,
// the whole 512 KB.
#define SIM_BASE 0x08000000u
#ifndef SIM_PAGES
#define SIM_PAGES 8u
#endif
#define SIM_SIZE (SIM_PAGES * FLASH_PAGE_SIZE)
#define SIM_DW_COUNT (SIM_SIZE / FLASH_DW_SIZE)

typedef struct {
    uint8_t mem[SIM_SIZE];
    // Per double-word: set when a program was interrupted. Any read touching
    // a flagged double-word returns FLASH_ERR_ECC.
    bool ecc_bad[SIM_DW_COUNT];

    long ops;         // operations since the last arm or reboot
    long cut_after;   // operation to interrupt. -1 disables
    bool tear_on_cut; // true = torn, false = clean
    bool powered;     // cleared once the cut happens

    long erases; // counters: a normal boot must perform zero writes
    long programs;
} sim_flash_t;

void sim_flash_init(sim_flash_t *sf);
void sim_flash_reboot(sim_flash_t *sf); // power back on, memory kept
void sim_flash_arm_cut(sim_flash_t *sf, long n, bool tear);
void sim_flash_bind(sim_flash_t *sf, flash_if_t *out);

#endif