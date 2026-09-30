/* Application, slot B: the first 192 KB of bank 2.
 *
 * This file and app_slot_a.ld differ only in ORIGIN. That single line is the
 * A/B design from section 8.2: each release is linked twice, and the
 * bootloader rejects any image whose load_addr does not match the slot it
 * physically occupies.
 */

MEMORY
{
    /* The slot starts with the 512-byte image header written by
     * tools/imgtool.py, so the program itself starts 0x200 in. The last
     * 256 bytes are left for the signature trailer. */
    FLASH (rx)  : ORIGIN = 0x08040000 + 0x200, LENGTH = 192K - 0x200 - 0x100
    /* The first 32 bytes of SRAM hold the boot handoff from
     * common/include/boot_handoff.h. Starting RAM after it means startup
     * never zeroes it and no variable is placed on top of it. */
    RAM   (rwx) : ORIGIN = 0x20000020, LENGTH = 96K - 32
}

/* Top of RAM. The stack grows downward from here. */
_estack = ORIGIN(RAM) + LENGTH(RAM);

/* The root for --gc-sections. Without it the collector finds nothing
 * referenced and discards the entire program. */
ENTRY(Reset_Handler)

SECTIONS
{
    /* Must be first. The bootloader sets VTOR to this address, and the core
     * reads the initial stack pointer and reset vector from the first two
     * words. */
    .isr_vector : {
        KEEP(*(.isr_vector))
    } > FLASH

    .text : {
        *(.text*)
        *(.rodata*)
    } > FLASH

    /* Initialised data lives in flash and is copied to RAM at startup.
     * _sidata is where it lives, _sdata/_edata where it goes. */
    _sidata = LOADADDR(.data);
    .data : {
        _sdata = .;
        *(.data*)
        . = ALIGN(4);
        _edata = .;
    } > RAM AT > FLASH

    /* Zero-initialised data occupies no flash, only an address range that
     * startup code clears. */
    .bss : {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM
}