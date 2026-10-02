# stm32-secure-ota

[![CI](https://github.com/leandroodorici/stm32-secure-ota/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/leandroodorici/stm32-secure-ota/actions/workflows/ci.yml)

A secure bootloader and fail-safe firmware update system for an STM32 device
that cannot be reached once deployed.

The node verifies every image itself before running it, installs updates into
an inactive flash slot while it keeps working, boots a new image on trial, and
falls back to the previous one if the new image never proves itself healthy.
Three failure classes drive the design: power loss at any instant, including
mid-erase and mid-program; an image that is authentic but broken; and an
attacker who controls the gateway, the link, or both.

The full reasoning is in [docs/DESIGN.md](docs/DESIGN.md). Everything claimed
about the hardware, and how it was checked, is in
[docs/HARDWARE.md](docs/HARDWARE.md).

## Status

| Milestone | Content | State |
|-----------|---------|-------|
| M0 | Image tooling, boot records, flash simulator, power-fail test, CI | Done |
| M1 | Boot core: slot selection, jump, trial counting, watchdog | Done, verified on hardware |
| M2 | Signature verification on the device, negative tests on hardware | Done, verified on hardware |
| M3 | Updates over UART: application, update agent, resume, anti-rollback | Next |
| M4 | CAN link with ISO-TP on both ends | Planned |
| M5 | Gateway: Wi-Fi fetch, staging, pre-verification, per-node state | Planned |
| M6 | Repeated-reset testing, throughput and timing measurements | Planned |

Each completed milestone is tagged on `main`.

## What works today

- **Signed images.** ECDSA P-256 over SHA-256, raw r||s signatures so no ASN.1
  parser runs on unauthenticated input. The bootloader checks bounds, hash,
  key ID, signature and the anti-rollback floor, in that order, before every
  jump. Header fields are only used for bounds checks until the signature has
  been verified.
- **A/B slots executed in place.** One slot per flash bank, each release linked
  for both. Switching versions is a single boot record write; nothing is
  copied, and an update never touches the running image.
- **Power-fail-safe boot state.** Append-only 32-byte records, CRC-protected,
  with a commit word written last and ping-pong page rotation. An exhaustive
  sweep cuts power at every flash operation of the update lifecycle, both
  cleanly and mid-operation, against a simulator that models the G4's ECC
  double-word semantics.
- **Trial boot and rollback.** A new image gets three boots to confirm itself.
  Every attempt is recorded before the jump, so a crash still consumes one.
  The independent watchdog turns a hang into a counted attempt.
- **Distinct rejection reasons.** Corruption (bad hash), forgery (bad
  signature), a misplaced image (wrong slot) and a downgrade (rollback blocked)
  are recorded differently, so a fleet can be diagnosed remotely.

### Verified on the NUCLEO-G474RE

| Measurement | Value |
|-------------|-------|
| Bootloader size | 19.7 KB of code against a 32 KB budget, of which the Mbed TLS verify path is about 15.6 KB |
| ECDSA P-256 verify at the 16 MHz reset clock | 1.7 s |
| Worst-case verification, full 192 KB slot | 2.6 s |
| Negative test images rejected on target with the expected reason | 13 of 13 |

## Hardware

- **Node:** NUCLEO-G474RE (STM32G474RE, Cortex-M4F, 512 KB flash in dual-bank
  mode, independent watchdog, FDCAN)
- **Gateway:** ESP32-S3 development board (planned, M5)
- **Link:** classic CAN at 500 kbit/s with ISO-TP (planned, M4). UART for bring-up (M3)

### Flash layout

| Region | Address range | Size | Bank |
|--------|---------------|------|------|
| Bootloader | 0x08000000 - 0x0800EFFF | 60 KB | 1 |
| Boot state | 0x0800F000 - 0x0800FFFF | 4 KB | 1 |
| Slot A | 0x08010000 - 0x0803FFFF | 192 KB | 1 |
| Slot B | 0x08040000 - 0x0806FFFF | 192 KB | 2 |
| App data | 0x08070000 - 0x0807FFFF | 64 KB | 2 |

Each slot holds a 512-byte header, the application linked at slot + 0x200, and
the signature trailer.

## Repository layout

```
bootloader/   bootloader: boot flow, flash driver, Mbed TLS configuration
app/          application and its slot A / slot B linker scripts
common/       hardware-independent core shared by bootloader and host tests:
              boot records and image verification
tests/host/   host test suite: flash simulator, boot records, power-fail
              sweep, verifier cross-check against imgtool
tools/        imgtool.py (keys, signing, verification, bad-image generation)
              and the on-target negative test script
bench/        on-target cost measurement of the verification path
third_party/  Mbed TLS 3.6 LTS, pinned as a submodule
docs/         design document and hardware notes
```

## Building

### Prerequisites

- CMake 3.16 or later and a host C compiler
- Arm GNU toolchain (`arm-none-eabi-gcc`)
- OpenOCD 0.12 and an ST-LINK (on the Nucleo board) for flashing
- Python 3 with `tools/requirements.txt`

### Setup

```bash
git clone https://github.com/leandroodorici/stm32-secure-ota.git
cd stm32-secure-ota
git submodule update --init third_party/mbedtls

python3 -m venv .venv
.venv/bin/pip install -r tools/requirements.txt

# Development signing key. keys/*.pem is ignored by git, and CI fails if a
# key file is ever committed.
.venv/bin/python tools/imgtool.py keygen
```

### Host tests

No embedded toolchain needed. The build generates a throwaway key and a set of
deliberately broken images, and the C verifier must agree with `imgtool.py` on
every one of them.

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

### Firmware

```bash
cmake -S . -B build-fw -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build-fw
```

This produces `bootloader.elf`, with the public key compiled in, and signed
images `app_slot_a.img` and `app_slot_b.img`.

### Flashing

The images are raw binaries placed at the slot base. Flash them first and the
bootloader last, so the bootloader never starts against old slot contents:

```bash
openocd -f interface/stlink.cfg -f target/stm32g4x.cfg \
  -c "program build-fw/app_slot_a.img verify 0x08010000" \
  -c "program build-fw/app_slot_b.img verify 0x08040000" \
  -c "program build-fw/bootloader.elf verify reset exit"
```

On a device with no boot record, the bootloader verifies slot A, adopts it as
the confirmed image and writes the first record.

### On-target negative tests

Writes every image from the bad-image set into slot B, marks it pending, and
checks the reason the bootloader records. Destructive to the boot state;
restores a clean state when done.

```bash
.venv/bin/python tools/hw_negative_test.py
```

## License

MIT. See [LICENSE](LICENSE).
