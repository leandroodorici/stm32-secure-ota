/* Bootloader: first 60 KB of bank 1.
 *
 * The region is deliberately larger than the current code. The bootloader
 * cannot be updated in the field, so it needs room to grow before anything
 * ships. Budget is 32 KB of code against 60 KB reserved.
 */

MEMORY
{
    FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 60K
    /* The first 32 bytes of SRAM hold the boot handoff from
     * common/include/boot_handoff.h. Starting RAM after it means startup
     * never zeroes it and no variable is placed on top of it. */
    RAM   (rwx) : ORIGIN = 0x20000020, LENGTH = 96K - 32
}

/* Top of RAM. The stack grows downward from here. */
_estack = ORIGIN(RAM) + LENGTH(RAM);

ENTRY(Reset_Handler)

SECTIONS
{
    /* The vector table must be first: the core reads the initial stack
     * pointer and reset vector from the very start of flash at reset. */
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

    /* Zero-initialised data occupies no space in flash, only an address
     * range that startup code clears. */
    .bss : {
        _sbss = .;
        *(.bss*)
        *(COMMON)
        . = ALIGN(4);
        _ebss = .;
    } > RAM
}