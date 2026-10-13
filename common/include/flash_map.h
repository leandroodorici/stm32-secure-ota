#ifndef FLASH_MAP_H
#define FLASH_MAP_H

// The flash map from docs/DESIGN.md section 4, shared by the bootloader and
// the application. The linker scripts repeat the slot addresses, since a
// linker script cannot include a C header: a change here must be made there
// too.

// Slot A is in bank 1 and slot B in bank 2, so the update agent always writes
// to a bank it is not executing from
#define SLOT_A_BASE 0x08010000u
#define SLOT_B_BASE 0x08040000u
#define SLOT_SIZE (192u * 1024u)

// Two 2 KB pages used as a ping-pong pair: when one fills, the other is
// erased and writing continues there
#define BOOT_STATE_PAGE_A 0x0800F000u
#define BOOT_STATE_PAGE_B 0x0800F800u
// The first two pages of the App data region hold the update agent's
// progress log, the same ping-pong arrangement as the boot state
#define PROGRESS_PAGE_A 0x08070000u
#define PROGRESS_PAGE_B 0x08070800u

#endif // FLASH_MAP_H
