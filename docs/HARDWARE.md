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

**Cost of verification at the reset clock.** Measured 2026-09-30 with
`bench/crypto_bench.c` flashed in place of the bootloader, timed with the
DWT cycle counter at 16 MHz. A worst-case verification (full-slot SHA-256
plus one ECDSA P-256 verify) takes 2.6 s. This fits one watchdog period,
whose shortest value is 3.76 s given the LSI tolerance, but two verifications 
in one boot do not. A boot that rejects one slot and then verifies the other 
would be reset partway through the second. If nothing recorded the first failure,
the next boot would repeat it indefinitely. The bootloader must therefore bound 
the work between watchdog refreshes. Peak Mbed TLS heap use was 4948 bytes.

**Signed image layout.** Verified 2026-09-30. Both slots hold images from
`tools/imgtool.py sign`: a 512-byte header at the slot base, the program
linked at slot + 0x200, and the signature trailer after it. The bootloader
sets VTOR to 0x08010200 or 0x08040200, read back over SWD, and the slot B
trial and rollback sequence produced the same records as with unsigned
images. The bootloader does not yet read the header.

**Verification before every jump.** Verified 2026-10-01. The bootloader runs
the full check from DESIGN.md section 5 before every jump. A confirmed slot A
booted after a visible pause of about 1.7 s (the ECDSA verify at 16 MHz) and
still wrote no record. A valid slot B marked PENDING passed verification and
entered TRIAL. Slot A's image, correctly signed but linked for slot A, was
then written into slot B. The next boot rejected it with reason 14
(WRONG_SLOT) and returned to slot A.

**Negative tests on target.** Verified 2026-10-02 with
`tools/hw_negative_test.py`. Each of the 13 images from `imgtool.py gen-bad`,
built for slot B and signed with the development key, was written into slot B
and marked PENDING over SWD, with a security floor of 5. Every bad image was
rejected with the reason the host test expects: BAD_MAGIC, BAD_HEADER (four
variants), BAD_HASH, BAD_SIGNATURE (four variants), WRONG_SLOT and
ROLLBACK_BLOCKED. The board came back up on slot A each time. The good image
entered TRIAL and ran from slot B.

**FreeRTOS and watchdog supervision.** Verified 2026-10-05. The application
runs FreeRTOS V11.3.1 with static allocation only. The kernel tick advanced
by about 1000 per second, measured by reading `xTickCount` over SWD, and a
halt landed in the idle task on the process stack. The watchdog is fed only
by a supervisor task, and only when every registered task has reported a
heartbeat. With the blinker's heartbeat removed but the task still running, 
the part reset after about 4 s with reset cause `0x24000000` (IWDG + PIN). 
With it restored, the application ran indefinitely.

**Status LED and fault button.** Verified 2026-10-07. A common-cathode RGB
LED on the breadboard shows the system state, one resistor per channel: red
on PA8 (D7), green on PB10 (D6), blue on PB5 (D4). Every pin and colour,
including the mixed ones, was checked over SWD before any firmware drove
them. B1 on PC13 reads 1 while pressed and 0 when released, measured by
sampling it over SWD. This is the opposite of what Zephyr's board file for
this board declares.

Pressing B1 makes the fault task stop reporting its heartbeat while it keeps
running. With slot B marked PENDING, one press on each of three trial boots
made the supervisor withhold the watchdog each time. The bootloader then
rejected slot B with reason 16 and the board returned to slot A, which reads
seq 11 and last result 16 from its boot record. The whole trial and rollback
sequence was observed on the LED, without a debugger.

**Serial link and STATUS.** Verified 2026-10-10. LPUART1 on PA2/PA3 reaches
the host through the ST-LINK's USB serial port at 115200 baud, receiving in
an interrupt at priority 6 into a FreeRTOS stream buffer. `tools/node.py
status` returned the node's state, which matched the boot record. The
reported reset cause followed real events: `PIN + SW` after an OpenOCD
reset, `PIN` after B2, and `PIN + IWDG` after a fault injected with B1.
Twenty consecutive requests all succeeded. After 4 KB of random bytes were
written to the port, the next request was answered normally. The receiver
drops invalid frames and resynchronises at the next delimiter.

## Do not do these

**RDP level 2** on the G474. Irreversible, and there is one board.

**eFuse burning** on the working ESP32-S3. One-time programmable by
definition. The second board exists for that.

**Changing `DBANK`.** It triggers a mass erase. Harmless now, worth knowing
before doing it with firmware on the chip.

## Measurements

Populated as milestones complete

| Measurement | Value | Date |
|-------------|-------|------|
| Bootloader size | 20176 B text, 8 B data, 6488 B bss (full image verification; 6 KB of bss is the Mbed TLS buffer) | 2026-10-01 |
| Application size | 9684 B text, 7360 B bss (adds LPUART1 driver, framing, protocol and comms task; all memory static) | 2026-10-10 |
| Mbed TLS verify path | about 15.6 KB code (ECP, bignum, curve data, SHA-256, allocator) | 2026-09-30 |
| Cold boot to application | | |
| SHA-256 over a full slot | 901 ms at 16 MHz (14.41 M cycles, 73 cycles/byte), `MBEDTLS_SHA256_SMALLER` | 2026-09-30 |
| ECDSA P-256 verify | 1707 ms at 16 MHz (27.30 M cycles), curve load included; 4948 B peak heap | 2026-09-30 |
| CAN throughput at 500 kbit/s | | |
