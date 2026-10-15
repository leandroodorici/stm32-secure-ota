# Fail-safe signed firmware updates

**Owner:** Leandro Odorici  
**Status**: draft, implementation in progress  
**Last updated**: 2026-10-02  
**Target**: STM32G474RE (node), ESP32-S3 (gateway)

## 1. Scope

This document describes a firmware update system for a device that cannot be
physically reached after deployment. A gateway delivers signed firmware images
to a node over a low-bandwidth bus. The node verifies each image itself,
installs it into an inactive flash slot while continuing to run, boots it on
trial, and reverts to the previous image if the new one does not prove itself
healthy.

The design problem is not delivery. It is what happens when delivery goes
wrong. Three failure classes drive every decision:

- Power loss at an arbitrary instant, including mid-erase and mid-program.
  Flash operations are not atomic and the device has no reliable warning.
- A signed image that is authentic but broken. Signatures prove origin, not
  correctness.
- An attacker who controls the gateway, the link, or both.

A device that is reachable can recover from all three with a technician and a
programmer. This design assumes no such recovery exists.

## 2. Hardware

The node is a NUCLEO-G474RE. The G474 was chosen for three properties. Its
flash can be configured as two independent banks, which allows code executing
from one bank to continue running while the other is erased or programmed.
It has an independent watchdog clocked from its own oscillator, which software
cannot stop once started. And it has an FDCAN controller, giving access to a
constrained multi-drop bus without additional hardware.

The gateway is an ESP32-S3 development board. It provides a Wi-Fi backhaul and
a TWAI controller, which is CAN 2.0 compatible. ESP-IDF ships Mbed TLS, so the
gateway can verify images with the same algorithm the node uses.

The two are connected by a CAN bus with a transceiver at each end and 120 ohm
termination at both ends of the pair.

## 3. Architecture

Three components, with the release server running on a development machine.

```
release server              gateway (ESP32-S3)           node (STM32G474)
  sign images       Wi-Fi     fetch manifest      CAN      bootloader
  serve manifest   ------->   verify signature   ------>   application
                   <-------   store and forward  <------
                    status                        ISO-TP
```

The node runs two distinct programs. The **bootloader** occupies a fixed region
at the start of flash and is never updated in the field. It verifies images,
selects which slot to run, and jumps to it. The **application** does everything
else, including receiving updates and writing them to flash.

This division is the most consequential decision in the design and is discussed
in section 8.1.

The gateway holds a complete image in a staging partition before offering it to
any node, and verifies the signature before doing so. Nodes verify
independently regardless.

## 4. Flash layout

The G474 in dual-bank mode presents two banks of 256 KB with 2 KB pages. One
application slot is placed in each bank.

| Region     | Address range           | Size   | Bank |
|------------|-------------------------|--------|------|
| Bootloader | 0x08000000 - 0x0800EFFF | 60 KB  | 1    |
| Boot state | 0x0800F000 - 0x0800FFFF | 4 KB   | 1    |
| Slot A     | 0x08010000 - 0x0803FFFF | 192 KB | 1    |
| Slot B     | 0x08040000 - 0x0806FFFF | 192 KB | 2    |
| App data   | 0x08070000 - 0x0807FFFF | 64 KB  | 2    |

The bootloader region is write protected through the option bytes in the
production configuration. Its budget is 32 KB of code against 60 KB reserved.
The bootloader cannot be updated, so it has to have room to grow before anything
ships.

Boot state occupies two 2 KB pages used as a ping-pong pair. The reason for two
pages is explained in section 6.

Placing the slots in different banks means the application always writes to a
bank it is not executing from. Erasing a 2 KB page takes on the order of
milliseconds, and a single-bank configuration would stall the CPU for that
entire period. Stalls of that length cause dropped CAN frames and missed
sensor samples during an update.

Reading one bank while writing the other needs one precaution on this part.
Erratum 2.2.2 of ES0430 lets the flash data cache be corrupted while the
other bank is programmed or erased, so the driver turns the data cache off
for each operation. Two kinds of write do stall the CPU: boot records, in bank 1, 
and the progress log, in bank 2, which is the running bank when the application 
runs from slot B. Both are short, written at most once per 2 KB received, and the 
stop-and-wait protocol has nothing in flight while they happen.

Within a slot, the image header occupies the first 512 bytes and the vector
table begins at offset 0x200. The offset is not arbitrary. The Cortex-M4 vector
table offset register requires the table to be aligned to a power of two at
least as large as the table itself, and the G474 table needs 512 byte
alignment.

## 5. Image format

An image is a 512 byte header, a payload, and a trailer of type-length-value
records.

```c
typedef struct {
    uint32_t magic;            // 0x31474D49, "IMG1"
    uint16_t header_version;
    uint16_t header_size;      // must equal 0x200
    uint32_t load_addr;        // must equal the base of the slot it occupies
    uint32_t payload_size;
    uint32_t version;          // major<<24 | minor<<16 | patch
    uint32_t build_number;
    uint32_t security_counter;
    uint32_t flags;
    uint8_t  reserved[480];    // zero filled, covered by the signature
} image_header_t;
```

Three TLVs follow the payload: a SHA-256 digest, a key identifier, and an
ECDSA P-256 signature stored as raw r||s. The signature covers the header and
payload together, so load address, version and security counter cannot be
altered without invalidating it. The trailer is not covered because it contains
the signature.

The key identifier is the first eight bytes of SHA-256 over the uncompressed
public key point. The bootloader currently trusts exactly one key. The field
exists so a later bootloader can hold several and select by identifier without
an image format change.

### Parsing order

The bootloader must parse header fields before it can verify anything, which
means acting on unauthenticated data. The rule is that header fields are used
only for bounds checking until the signature has been verified.

Specifically: check magic, check that header_size matches, check that
load_addr equals the base of the slot the image physically occupies, check that
payload and trailer fit inside the slot. Then hash the header and payload and
compare against the SHA-256 TLV. Then verify the signature. Only after that may
version or security_counter influence any decision.

Keeping the hash TLV as well as the signature is not redundant. A hash mismatch
means corruption in transit. A signature mismatch with a matching hash means
deliberate modification. The node reports these differently, which matters when
diagnosing a fleet remotely.

Storing the signature as raw r||s rather than DER removes an ASN.1 parser from
the pre-authentication attack surface. The bootloader parses a fixed-size pair
of integers instead.

## 6. Boot state

The bootloader and application share persistent state: which slot is primary,
what condition each slot is in, how many trial boots have been attempted, and
the anti-rollback floor.

The obvious implementation is a struct in flash, updated in place. That does
not work on this part. The G474 programs flash in 64-bit double-words and
stores an ECC syndrome with each one. A double-word that has been programmed
cannot be programmed again, because the new data would not match the stored
syndrome. The common trick of clearing a flag bit in place to mark an image
confirmed is unavailable.

The state is therefore append-only. Each change writes a new 32 byte record.
The newest record with a valid CRC wins.

```c
typedef struct {              // 32 bytes, 4 double-words
    uint32_t magic;           // DW0
    uint32_t seq;
    uint8_t  primary_slot;    // DW1
    uint8_t  slot_state[2];
    uint8_t  trial_attempts;
    uint32_t security_floor;
    uint32_t last_result;     // DW2
    uint32_t crc32;           // covers bytes 0..19
    uint64_t commit;          // DW3, programmed last
} boot_record_t;
```

Each record is a complete snapshot rather than a delta. Recovery is then a
scan for the newest valid record, with no event replay to get wrong.

A record is rejected unless its CRC matches. Because the CRC covers bytes 0
through 19 and is itself stored at offset 20, any double-word that failed to
program reads back as 0xFF and changes either the covered bytes or the stored
CRC. This is what makes torn records detectable, and it holds regardless of the
order the double-words are written in.

The commit double-word is written last and checked first. It is a cheap early
reject for the common case of a wholly erased entry, and a second line of
defence if the CRC coverage is ever changed by mistake. It is not the primary
protection.

A 2 KB page holds 64 records. When a page fills, the other page is erased and
the next record is written there. The erase is safe at any instant because the
newest record still lives in the page being left. A power loss during the erase
leaves the old page intact and the next boot simply retries.

Slot states are EMPTY, PENDING, TRIAL, CONFIRMED and REJECTED. Ownership of
transitions is split. The application sets a slot EMPTY before erasing it and
PENDING once a complete image has been written and verified. The bootloader
moves PENDING to TRIAL, and TRIAL to REJECTED on failed verification or
exhausted attempts. The application moves TRIAL to CONFIRMED, in one record
that also makes that slot primary and raises the security floor to the
image's counter (section 8.5). The other slot keeps its state, so a
confirmed previous image stays a fallback for as long as the floor allows it.

Two invariants the update agent enforces: it never writes to the primary slot,
and it refuses a new update while the running image is still in TRIAL, because
the other slot is the only fallback.

## 7. Boot flow

```
reset
  start the independent watchdog
  capture and clear the reset cause, pass it to the application in noinit RAM
  load the newest valid boot record
    none found -> factory path: verify slot A, write an initial record

  if a slot is PENDING or TRIAL:
      TRIAL with attempts >= 3  -> mark REJECTED, continue
      verification fails        -> mark REJECTED with a reason, continue
      otherwise                 -> increment attempts, mark TRIAL, write, jump

  if the primary slot is CONFIRMED and verifies  -> jump, writing nothing
  if the other slot is CONFIRMED and verifies    -> make it primary, mark a
                                                    primary that failed
                                                    verification REJECTED,
                                                    write, jump
  otherwise                                      -> halt, writing nothing; the
                                                    watchdog resets and retries
```

A normal boot of a confirmed image performs no flash writes at all. On a device
that reboots frequently this matters for both flash wear and energy.

The jump itself disables SysTick, disables and clears all NVIC interrupts,
restores the reset clock configuration, sets the vector table offset register,
sets the main stack pointer from the first word of the slot, and branches to
the reset vector. Before any of that it checks that the stack pointer value
lies within SRAM and that the reset vector lies within the slot with its Thumb
bit set. Those two checks are cheap and catch a slot that passed verification
but wasn't linked correctly.

The watchdog is started before anything else so that a hang anywhere in the
bootloader, including inside verification, still produces a reset the next boot
can count. Its timeout is at least 3.76 s, and one worst-case verification
takes 2.6 s, so a boot that checks more than one slot would not fit in a single
period. The watchdog is therefore refreshed before each verification, which
bounds the work between refreshes to one verification while a hang inside
any of them still resets the part.

## 8. Design decisions

### 8.1 The update agent lives in the application

The bootloader verifies and jumps. It contains no transport stack, no protocol
parser, and no RTOS. Receiving an image and writing it to flash is the
application's job.

The bootloader cannot be updated in the field, so every line in it is permanent
risk. Transport and protocol code is where bugs concentrate. Putting it in the
application means those bugs are fixable over the air.

The alternative is the pattern where the device reboots into the bootloader to
download. That makes the device unavailable for the whole transfer, grows the
bootloader substantially, and makes a protocol bug permanent. Keeping the
download in the application also means the device continues its actual work
during an update, which matters when transfers take minutes.

### 8.2 A/B slots executed in place, not swapped

Each release is linked twice, once for each slot base address. The node
executes directly from whichever slot is primary. Changing versions is a single
boot record write, nothing is copied.

This makes power-fail safety close to trivial. An update never touches the
running image, and the switch is one atomic record. It also means one erase and
program pass per update rather than two or three, and rollback is instant.

The alternative is a swap-based scheme with a single link address, where the
bootloader copies images between slots. MCUboot's swap modes work this way. It
avoids building two binaries, but requires a resumable multi-step swap with its
own power-fail state machine, triples the flash writes, and creates a long
window where neither slot holds a complete image.

A third option is the G474's hardware bank swap, which remaps the banks through
an option bit. That gives a single link address with no copying, but the
behaviour is chip-specific and the bootloader has to be duplicated in both
banks.

The cost of the chosen approach is two binaries per release and a requirement
that the gateway sends the one built for the node's inactive slot. The
bootloader rejects any image whose load_addr does not match the slot it
occupies, so a mistake here cannot result in executing a wrongly linked image.

### 8.3 ECDSA P-256 with SHA-256

Verification requires no secrets on the device and no source of randomness,
which suits a bootloader. P-256 is what MCU secure-boot ecosystems standardise
on and what hardware accelerators support, so the design ports to parts with
acceleration without changing the format.

Mbed TLS provides the implementation. It is maintained, widely deployed, and
the same library family ESP-IDF already ships, so the gateway verifies with the
same algorithm at no additional cost. The classic ECDSA interface is used
rather than the PSA interface, because PSA expects an entropy source that a
verify-only bootloader has no reason to provide. The library is pinned to a
long-term-support release as a submodule.

Ed25519 was the main alternative and is a better algorithm in several respects,
but Mbed TLS does not implement it, and adding a second crypto library to a
bootloader for one primitive is a poor trade. RSA verifies quickly but has
signatures of 256 to 384 bytes and larger code. A shared HMAC key was rejected
outright: every device would hold the secret, so extracting it from one device
would compromise the entire fleet.

### 8.4 Trial boot with network-based confirmation

A new image boots in TRIAL state. The bootloader increments the attempt counter
before jumping, so a crash at any point still consumes an attempt. After three
unconfirmed attempts the image is rejected and the previous one boots.

The application confirms only after a health check that includes a successful
status exchange with the gateway. Confirming on the basis that startup
completed is not sufficient. An image that boots but cannot reach its gateway
is unusable in the field, and if its update path is broken it can never be
repaired remotely. Tying confirmation to a working update path means the device
can only commit to an image it could subsequently replace.

The health check is two conditions. Every supervised task has stayed alive
for five consecutive supervisor checks, about 5s, and the gateway has then
sent STATUS_REQ. The node confirms before it replies, so the gateway learns
of the confirmation from that same STATUS. The gateway drives this: after
REBOOT it polls STATUS until the image is confirmed or rolled back. A trial
image that has not confirmed 60s after starting resets itself. Without that
deadline, an image that cannot reach its gateway would stay on trial, and
refuse every update, until something else reset it. With it, three such
boots end in a rollback with no help from outside.

The independent watchdog is fed by a supervisor task that checks a heartbeat
from every other task. It is never fed from the idle hook, which would allow a
deadlocked task to go unnoticed while the watchdog stays happy.

A task busy with one long operation cannot report a heartbeat partway
through. COMMIT verifies the whole image in a single call, about 2 s for a
30 KB image and 2.6 s for a full slot. Before it starts, the comms task
excuses itself from the supervisor's checks for a bounded time, 4 s, and its
next heartbeat ends the excuse. A verification that hangs still stops the
watchdog being fed once the excuse runs out. The comms task runs at the
status task's priority, so during verification the two share the processor
rather than one starving the other.

### 8.5 Anti-rollback with a separate counter

Each image carries a security_counter independent of its version. The
bootloader rejects images whose counter is below a floor stored in the boot
record. The floor rises to the image's counter in the same record write that
confirms it.

Keeping the counter separate from the version means routine releases can ship
without consuming the rollback budget. Only releases that fix a vulnerability
raise the counter.

Raising the floor on confirmation rather than on installation is what keeps
rollback working. If a new release fails its trial, the previous image is still
above the floor and can be booted. Doing both in one record makes the
transition atomic.

The consequence is that once a floor-raising release is confirmed, the previous
slot falls below the floor and is marked rejected, leaving no fallback until
the next update. This is intended. The floor was raised specifically to prevent
that version from running again.

The floor lives in ordinary flash, which an attacker with debug access can
erase. A production device would store it in one-time-programmable memory with
debug access locked. See section 10.

### 8.6 CAN with ISO-TP

Node and gateway communicate over classic CAN at 500 kbit/s. The ESP32-S3's
TWAI controller does not support CAN FD, so the G474's FDCAN peripheral runs in
classic mode.

CAN frames carry at most 8 bytes, so messages are segmented using ISO-TP
(ISO 15765-2), implemented here rather than taken from a library. The protocol
above it sees only a send and receive interface.

The choice of CAN is deliberate rather than incidental. A point-to-point serial
link would exercise none of the problems a real deployment has: it has no
addressing, no meaningful frame size limit, and no shared medium. CAN forces
segmentation, flow control, per-node addressing and resume, which are the
mechanisms that would also be needed over a sub-GHz radio. ISO-TP is also the
transport used for automotive ECU updates, so this is an implementation of a
deployed standard rather than a stand-in.

A UART transport exists alongside it and is used for bring-up. Having two
transports is what keeps the protocol layer honest about its dependencies.

## 9. Update protocol

Transport-independent. All messages carry a node identifier.

| Message      | Direction | Purpose                                              |
|--------------|-----------|------------------------------------------------------|
| STATUS_REQ   | GW -> N   | Request current state                                |
| STATUS       | N -> GW   | Running slot, version, slot states, floor, last result, reset cause |
| OFFER        | GW -> N   | Size, version, counter, target slot, digest, chunk size |
| ACCEPT       | N -> GW   | Resume offset                                        |
| REJECT       | N -> GW   | Reason code                                          |
| CHUNK        | GW -> N   | Offset, length, data, CRC32                          |
| CHUNK_ACK    | N -> GW   | Next expected offset                                 |
| COMMIT       | GW -> N   | Request full verification                            |
| VERIFY_RESULT| N -> GW   | Outcome of hashing and verifying the whole slot      |
| REBOOT       | GW -> N   | Optional delay                                       |
| ABORT        | either    | Reason code                                          |

OFFER exists so the node can refuse early. Wrong target slot, image too large,
counter below the floor, or an update already in trial are all rejected before
any data is transferred.

Chunks default to 1 KB, half a flash page. This bounds retransmission cost and
keeps the receive buffer small. The per-chunk CRC catches reassembly errors
without assuming the transport provides a strong check.

Acknowledgement is stop-and-wait per chunk. A sliding window would help on a
slow link but is unnecessary at CAN speeds, and adding it now would be
optimizing something that has not been measured.

Resume works because chunks are written directly to flash as they arrive.
After a reset the node reports the highest contiguous offset it has written and
the gateway continues from there. The partial slot is never marked PENDING, so
the bootloader never considers it.

COMMIT triggers a full hash and signature check over the assembled slot. This
is the end-to-end guarantee. Per-chunk CRCs catch transport errors but say
nothing about the image as a whole.

## 10. Security model

The node treats the gateway and the link as untrusted. An attacker controlling
either can deliver arbitrary bytes. The properties that must hold under that
assumption:

- Only images signed by the holder of the private key will execute.
- An image below the stored security floor will not execute.
- Malformed input is rejected during bounds checking, before any header field
  influences behaviour.

The gateway's signature check is an optimization, not a security boundary. It
prevents a bad image from consuming node energy and airtime. A compromised
gateway can refuse to deliver updates, which is a denial of service, but it
cannot cause code to run.

The private key exists only on the development machine. It is excluded from
version control and CI enforces that no key material is committed. CI signs
with a throwaway key generated on the runner.

### Out of scope

Three physical-access threats, excluded for three different reasons.

**Debug access.** An attacker with SWD can read flash, rewrite the security
floor, and replace the bootloader. This is a provisioning problem rather than a
design problem: readout protection locks debug, write protection covers the
bootloader region, and OTP or the securable memory area holds the public key
and floor. Not applied here because RDP level 2 is irreversible and there is
one board.

**Fault injection.** Glitching the branch that acts on a verification result
would run an unsigned image. This is a real gap. The side-channel argument
below does not cover it, since the attacker corrupts control flow rather than
recovering a secret. Countermeasures such as redundant comparisons, separate
verification paths and randomized delays all need a glitching rig to validate,
and none of them changes anything else in this design. Deferred on cost.

**Side channels.** Not applicable. ECDSA verification consumes only public
values: the key, the hash, the signature. There is no secret to recover, which
is why verification needs no constant-time implementation while signing does.
This holds only because the node stores no secrets at all. Adding one, such as
a decryption key, would make side-channel resistance a requirement (see
section 13).

## 11. Testing

Everything under `common/` is free of hardware dependencies. That constraint
exists so the boot logic can run on a host against a simulated flash.

The simulator models the controller's actual behaviour: erase sets a page to
0xFF, programming happens in 64-bit double-words, a programmed double-word
cannot be reprogrammed, and an interrupted program leaves a double-word that
faults on read rather than returning partial data. Modelling a more permissive
flash would let incorrect code pass.

Power cuts are injected by operation number in two modes. A clean cut means the
operation never starts. A torn cut means it starts and is interrupted, leaving
a damaged double-word. The torn mode is the one that matters. Code handling
only clean cuts will pass its tests and still brick a device.

The power-fail test enumerates every flash operation in an update lifecycle
and cuts at each one in both modes. After each cut it reboots and asserts four
things: a valid record is still readable, the surviving state is one the code
intended to write, a bootable slot exists, and the device can still make
progress. Enumeration rather than random sampling is the point. Random testing
leaves untested windows, and the whole premise here is that untested windows
are unacceptable on a device nobody can reach.

The simulator is itself tested, since every claim downstream depends on its
fidelity. Those tests were written before any code that uses it.

Negative tests cover tampered payloads and headers, wrong keys, corrupt
digests, truncated and oversized images, malformed TLVs, images linked for the
wrong slot, and downgrades. Each must be rejected with the correct reason code.
A bootloader that rejects a tampered image as malformed is hiding a bug.

The negative tests run at two levels. On the host, the C verifier must give the
same reason as `tools/imgtool.py` for every image in a generated set
(`tests/host/test_image_verify.c`), so the two verifiers cannot drift apart.
On the board, `tools/hw_negative_test.py` writes each image into slot B, marks
it PENDING, resets, and checks the reason the bootloader records.

`tools/hw_update_test.py` drives the update path on the board the way the
gateway will, over the serial link: updates in both directions with
confirmation, a refused downgrade, a refused update during a trial, and
rollback both after repeated resets and at the trial deadline.

On hardware, the update path is exercised repeatedly with a reset asserted at a
random point in each cycle, with the outcome logged per cycle.

Host tests run in CI against GCC and Clang, with and without address and
undefined-behaviour sanitizers. Warnings are errors, including conversion and
shadow warnings, which catch the implicit integer conversions that produce
silent address corruption.

## 12. Open questions

Items that need confirming on hardware before the design can be considered
settled.

The ECC fault behaviour on a torn double-word is taken from the reference
manual and has not been observed directly. The simulator's behaviour depends on
it. This is confirmed by interrupting a program operation deliberately and
observing the resulting fault.

Verification cost was measured at the reset clock: about 1.7 s for a small
image and 2.6 s in the worst case, a full slot (docs/HARDWARE.md). Full
verification on every boot at 16 MHz is kept for now. It fits the watchdog
because the watchdog is refreshed before each verification. If boot time matters, 
the bootloader can raise the clock for verification and restore the reset 
configuration before jumping. Still open is the policy for a device that 
resets on every wake from low power: full verification on power-on, brownout 
and watchdog resets, and a token in a backup register for wake-from-standby. 
That token can only be forged by code already executing on the device, at 
which point verification is irrelevant.

The 60 KB bootloader reservation is ample. With the full verification path the
bootloader is about 20 KB of code against a 32 KB budget (docs/HARDWARE.md).

Bootloader behaviour when both slots fail verification is currently a halt with
an error indication. Whether that is preferable to a minimal recovery mode is
not settled. A recovery mode means transport code in the bootloader, which
section 8.1 argues against.

## 13. Deferred and declined

### Deferred

Delta and compressed updates, multi-node update campaigns, a sub-GHz radio,
and USB DFU.

None of these changes a decision in this document. Each could be added later
without revisiting the flash layout, the image format, the boot state machine
or the trust model, which is the test applied here: not whether something is
hard, but whether having it would have produced a different design.

Delta updates and multicast delivery would be the first additions for a real
low-bandwidth fleet, where image size dominates transfer time and one
transmission could serve many nodes. USB DFU is excluded on different grounds:
it is a second update path into the device, which means more attack surface to
solve a problem this design assumes away.

### Declined

An RTOS in the bootloader, for the reason given in section 8.1. The bootloader
cannot be repaired in the field, so the only large dependency it carries is the
signature verification it cannot function without.

### Firmware encryption

Signed but unencrypted is a deliberate choice rather than an omission, and is
treated here as a stretch goal with known prerequisites.

Encryption protects intellectual property. It does not protect device
integrity, which is what signatures do. Secure boot and image encryption are
independently selectable features in every major embedded stack for this
reason, and plenty of shipped products enable the first without the second. It
is expected in payments, media and defence, and routinely omitted in industrial
and consumer devices where the transport is already encrypted.

What adding it would require:

- A decryption key stored on a device an attacker can hold. That means OTP, a
  secure element, or wrapping through a hardware unique key, and a decision
  about whether keys are per-device or fleet-wide.
- Constant-time AES. Once the device holds a secret, the side-channel argument
  in section 10 no longer applies.
- A different install path. An encrypted image cannot execute in place, so it
  would have to be decrypted into the slot on write, leaving plaintext in flash
  regardless. Without encrypted execution, encryption protects the image in
  transit and at rest on the gateway, not on the node itself.

That last point is the reason it is a stretch goal rather than a priority.

## 14. Milestones

| ID | Content                                                              |
|----|----------------------------------------------------------------------|
| M0 | Image tooling, boot records, flash simulator, power-fail test, CI     |
| M1 | Boot core: slot selection, jump, trial counting, watchdog             |
| M2 | Signature verification on device, negative tests on hardware          |
| M3 | Updates over UART: application, update agent, resume, anti-rollback   |
| M4 | CAN link with ISO-TP on both ends                                     |
| M5 | Gateway: Wi-Fi fetch, staging, pre-verification, per-node state machine |
| M6 | Repeated-reset testing, throughput and timing measurements            |

M3 is the first point at which the system is complete end to end. M4 and M5
replace the transport without changing the protocol.

## 15. References

- RM0440, STM32G4 series reference manual. Embedded flash memory, option
  bytes, FDCAN.
- PM0214, Cortex-M4 programming manual. Vector table offset register.
- ISO 15765-2, road vehicles, diagnostic communication over CAN, transport
  protocol and network layer services.
- MCUboot documentation, in particular the direct-XIP mode, which is the
  closest published analogue to the approach in section 8.2.
