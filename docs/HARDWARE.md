# Hardware notes

Board configuration and the checks behind it. Everything here is either
observed on the bench or explicitly marked as unverified. Claims taken from a
datasheet and never confirmed are noted as such, because the flash simulator
in `tests/host/` models several of these behaviours and its fidelity is what
every downstream correctness claim rests on.

## Boards

| Board | Role | Status |
|-------|------|--------|
| NUCLEO-G474RE | Node | In hand, option bytes read |
| ESP32-S3 dev board | Gateway | In hand, not yet used |
| Second ESP32-S3 | Reserved for irreversible experiments | In hand, untouched |

## Option bytes

Read 2026-09-25 with STM32CubeProgrammer. Full export in
`docs/option-bytes-2026-09-25.json`. Device ID 1129 (0x469), which is the
STM32G47x/G48x family.

### Confirmed for the design

**`DBANK = 1`.** Dual-bank mode is active. This is the one that had to be
right: the flash layout in section 4 of DESIGN.md assumes two banks of 256 KB
with 2 KB pages, and places one application slot in each bank so the update
agent always writes to a bank it is not executing from. No option byte change
was needed, so no mass erase.

**`RDP = 0xAA`.** Readout protection level 0. Debug access is open, which is
correct for development.

**`BFB2 = 0`.** Boot from bank 1. The hardware bank swap was rejected in
section 8.2 in favour of A/B slots with slot-specific link addresses, so this
stays at 0.

**`IWDG_SW = 1`.** The independent watchdog is software-started. The boot flow
in section 7 starts it explicitly as the first action after reset.

### Protection features, all unconfigured

| Feature | Value | Meaning |
|---------|-------|---------|
| WRP1A, WRP1B, WRP2A, WRP2B | start 127, end 0 | Empty regions: start above end means no area is protected |
| PCROP1, PCROP2 | start 32767, end 0 | Same, no read-out protected code region |
| SEC_SIZE1, SEC_SIZE2 | 0 | Securable memory area unused |
| BOOT_LOCK | 0 | Boot entry not locked to main flash |

A clean slate, which is what the production hardening in section 10 would
later configure: write protection over the bootloader region, and the
securable memory area or OTP holding the public key and the security floor.

### Low-power behaviour, relevant later

`IWDG_STOP = 1` and `IWDG_STDBY = 1` mean the independent watchdog keeps
counting in Stop and Standby rather than freezing. Irrelevant to the current
design, which never enters those modes, but these are the exact bits that
matter for the wake-from-Standby verification policy discussed in section 12.

## Unverified

**ECC fault on a torn double-word.** The flash simulator models an interrupted
program as leaving a double-word that faults on read rather than returning
partial data. This is taken from RM0440 and has not been observed on silicon.
Section 12 of DESIGN.md tracks it as an open question. Confirming it means
deliberately interrupting a program operation and observing the resulting
fault.

**CAN transceiver logic level.** Not yet checked. Read the part number off the chip and confirm against its
datasheet before wiring anything in M4.

## Confirmed on hardware

**Boot from main flash.** Verified 2026-09-25 by flashing the bootloader and a
blinking application, detaching the debugger, and power cycling from USB alone.
The LED blinked, so the part boots from main flash without a probe attached and
the `nSWBOOT0 = 0`, `nBOOT0 = 1`, `NRST_MODE = 3` combination needs no change.

**Dual-bank mode and RDP level, independently.** OpenOCD reports
`flash mode : dual-bank` and `RDP level 0 (0xAA)` on every connect, which
matches the option byte export from a different tool.

**Application handover.** With the bootloader at 0x08000000 and an application
at 0x08010000, GDB confirms the core executing in the application's code region
with VTOR reading 0x08010000 and the main stack pointer at the application's
`_estack`. Vector table relocation and the stack pointer switch both work.

**OTP.** 1024 bytes at 0x1fff7000, per OpenOCD. Unused. Section 10 of
DESIGN.md names this as where a production device would store the public key
and the anti-rollback floor.

**Boot record on target.** Verified 2026-09-28. With blank boot-state pages
and a bootable slot A, the bootloader took the factory path and wrote one
record at 0x0800F000: seq 1, slot A primary and CONFIRMED, CRC `0x5eec803b`.
That CRC was computed on the host beforehand, so `common/boot_record.c`
serializes identically on target and in the host tests. After several resets
and a power cycle the record was unchanged and the next entry still erased,
confirming that a normal boot of a confirmed image performs no flash writes.

**Watchdog and reset cause.** Verified 2026-09-28. The bootloader starts the
IWDG at a nominal 4 s timeout and hands the RCC_CSR reset flags to the
application in the first 32 bytes of SRAM. With the application's refresh
removed, the part reset after about 4 s and the next boot reported
`0x24000000` (IWDG + PIN); with it restored, the application ran
indefinitely. A button press reports `0x04000000` (PIN), and an OpenOCD
reset reports `0x14000000` (SW + PIN): OpenOCD resets through SYSRESETREQ.
Flags are cleared on every boot, so each boot sees only its own cause.

**The on-board ST-LINK resets the target after power-up.** Observed
2026-09-28. After a power cycle the handoff reported only `0x04000000` (PIN).
A temporary boot counter showed two boots since power-on, with `0x0C000000`
(BOR + PIN) seen across them: the power-on reset is detected correctly, then
the ST-LINK pulses NRST once it starts, and that second boot overwrites the
reported cause. Two consequences on this board: every power cycle consumes
two trial attempts rather than one, and reset-cause logic cannot rely on
seeing a power-on as the most recent cause. Neither applies to a board
without an on-board probe, but both matter for the repeated-reset tests.

**Trial boot and rollback.** Verified 2026-09-29. With slot B marked PENDING
by a boot record written over SWD, the bootloader moved it to TRIAL and
booted it three times, recording one attempt before each jump, then marked
it REJECTED with reason 16 (NOT_CONFIRMED) and fell back to slot A with no
further writes. Every record matched a value computed on the host beforehand,
CRC included. Repeated with a slot B image that never refreshes the
watchdog: the three trial boots ended in watchdog resets with no
intervention, and the rollback followed. The PENDING record in that run
deliberately carried a stale attempt count of 3. The bootloader restarted
it at 1, so the image received its full three attempts.

## Do not do these

**RDP level 2** on the G474. Irreversible, and there is one board.

**eFuse burning** on the working ESP32-S3. One-time programmable by
definition. The second board exists for that.

**Changing `DBANK`.** It triggers a mass erase. Harmless now, worth knowing
before doing it with firmware on the chip.

## Measurements

Populated as milestones complete. See `docs/RESULTS.md` for the full table.

| Measurement | Value | Date |
|-------------|-------|------|
| Bootloader size | 2604 B text, 4 B bss (slot selection, watchdog, reset cause; no crypto yet) | 2026-09-28 |
| Cold boot to application | | |
| SHA-256 over a full slot | | |
| ECDSA P-256 verify | | |
| CAN throughput at 500 kbit/s | | |
