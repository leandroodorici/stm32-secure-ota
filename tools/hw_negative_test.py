#!/usr/bin/env python3
"""On-target negative tests for the bootloader

Writes every image from "imgtool.py gen-bad" into slot B in turn, marks it
PENDING with a boot record written over SWD, resets the board, and checks the
record the bootloader appends. Every bad image must be REJECTED with the same
reason the host test expects, and the board must come back up on slot A. The
good image must enter TRIAL and run from slot B.

The images are signed with the development key, so the bootloader on the board
must have been built with the matching public key. A security floor of 5 is
set for the run, which is what blocks the downgrade image. Slot A is re-signed
at security counter 5 so that it still passes against that floor.

DESTRUCTIVE: erases both boot state pages and overwrites both slots. At the
end it restores slot A and slot B from build-fw/ and writes a fresh initial
boot record: slot A primary and confirmed, slot B empty, floor 0.

"""

import os
import re
import struct
import subprocess
import sys
import time
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
BUILD = os.path.join(ROOT, "build-fw")
OUT = os.path.join(BUILD, "hw_images")
KEY = os.path.join(ROOT, "keys", "dev_private.pem")
IMGTOOL = [sys.executable, os.path.join(HERE, "imgtool.py")]

SLOT_A = 0x08010000
SLOT_B = 0x08040000
SLOT_SIZE = 192 * 1024
STATE_BASE = 0x0800F000  # two 2 KB pages
STATE_SIZE = 0x1000
FLOOR = 5  # gen-bad signs its good image at 5 and its downgrade at 1

# common/include/boot_record.h
BR_MAGIC = 0x31524F42
BR_COMMIT = 0xC0FFEE0042424242
BR_SIZE = 32
BR_PER_PAGE = 2048 // BR_SIZE
EMPTY, PENDING, TRIAL, CONFIRMED, REJECTED = range(5)
RESULT_OK = 1
REASONS = {"OK": 1, "BAD_MAGIC": 10, "BAD_HEADER": 11, "BAD_HASH": 12,
           "BAD_SIGNATURE": 13, "WRONG_SLOT": 14, "ROLLBACK_BLOCKED": 15}

# Generous: a rejection costs one verification of slot B and one of slot A,
# about 3.5 s at 16 MHz, plus the reset itself
BOOT_TIMEOUT_S = 15.0


# ---- OpenOCD ----

def openocd(*commands):
    args = ["openocd", "-f", "interface/stlink.cfg", "-f", "target/stm32g4x.cfg"]
    for c in commands:
        args += ["-c", c]
    r = subprocess.run(args, capture_output=True, text=True)
    out = r.stdout + r.stderr
    if r.returncode != 0:
        sys.exit(f"openocd failed:\n{out[-2000:]}")
    return out


def read_state():
    """Returns (pc, bytes of both boot state pages)."""
    out = openocd(f"init; halt; echo [mdw {STATE_BASE:#x} {STATE_SIZE // 4}]; "
                  "resume; exit")
    pc = re.search(r"pc: (0x[0-9a-f]+)", out)
    words = {}
    for m in re.finditer(r"^(0x[0-9a-f]{8}):((?: [0-9a-f]{8})+)", out, re.M):
        addr = int(m.group(1), 16)
        for i, w in enumerate(m.group(2).split()):
            words[addr + 4 * i] = int(w, 16)
    data = b"".join(struct.pack("<I", words[STATE_BASE + 4 * i])
                    for i in range(STATE_SIZE // 4))
    return (int(pc.group(1), 16) if pc else None), data


# ---- boot records ----

def make_record(seq, primary, s0, s1, attempts, floor, last):
    body = struct.pack("<II", BR_MAGIC, seq) + bytes([primary, s0, s1, attempts])
    body += struct.pack("<II", floor, last)
    return body + struct.pack("<IQ", zlib.crc32(body), BR_COMMIT)


def parse_record(raw):
    magic, seq = struct.unpack_from("<II", raw, 0)
    crc, commit = struct.unpack_from("<IQ", raw, 20)
    if magic != BR_MAGIC or commit != BR_COMMIT or crc != zlib.crc32(raw[:20]):
        return None
    floor, last = struct.unpack_from("<II", raw, 12)
    return {"seq": seq, "primary": raw[8], "state": [raw[9], raw[10]],
            "attempts": raw[11], "floor": floor, "last": last}


def newest(data):
    """Returns (index, record) of the highest-seq valid record."""
    best = (None, None)
    for i in range(STATE_SIZE // BR_SIZE):
        rec = parse_record(data[i * BR_SIZE:(i + 1) * BR_SIZE])
        if rec and (best[1] is None or rec["seq"] > best[1]["seq"]):
            best = (i, rec)
    return best


def write_record_file(name, rec_bytes):
    path = os.path.join(OUT, name)
    with open(path, "wb") as f:
        f.write(rec_bytes)
    return path


def slot_of(pc):
    if pc is None:
        return "?"
    if SLOT_A <= pc < SLOT_A + SLOT_SIZE:
        return "A"
    if SLOT_B <= pc < SLOT_B + SLOT_SIZE:
        return "B"
    return "?"


def wait_for_boot(seq):
    # Polls until a record with at least this seq exists and the core is
    # running from a slot rather than still inside the bootloader. After a
    # rejection the bootloader goes on to verify slot A, which takes another
    # 1.7 s, so the record alone does not mean the boot has finished.
    # Returns (pc, newest record), or (None, None) on timeout.
    deadline = time.monotonic() + BOOT_TIMEOUT_S
    while time.monotonic() < deadline:
        time.sleep(1.0)
        pc, data = read_state()
        _, rec = newest(data)
        if rec and rec["seq"] >= seq and slot_of(pc) != "?":
            return pc, rec
    return None, None


# ---- the run ----

def generate():
    os.makedirs(OUT, exist_ok=True)
    subprocess.run(IMGTOOL + ["gen-bad", "--key", KEY,
                              "--in", os.path.join(BUILD, "app_slot_b.bin"),
                              "--load-addr", hex(SLOT_B), "--outdir", OUT],
                   check=True)
    subprocess.run(IMGTOOL + ["sign", "--key", KEY,
                              "--in", os.path.join(BUILD, "app_slot_a.bin"),
                              "--out", os.path.join(OUT, "slot_a_floor.img"),
                              "--load-addr", hex(SLOT_A),
                              "--security-counter", str(FLOOR)],
                   check=True)
    with open(os.path.join(OUT, "EXPECTATIONS.txt")) as f:
        return [line.rstrip("\n").split("\t") for line in f if "\t" in line]


def reset_state(slot_a_img, slot_b_img, floor):
    """Fresh boot state: slot A primary and confirmed, slot B empty."""
    first = write_record_file("rec_initial.bin",
                              make_record(1, 0, CONFIRMED, EMPTY, 0, floor, RESULT_OK))
    openocd("init", "reset halt",
            f"flash write_image erase {slot_a_img} {SLOT_A:#x} bin",
            f"flash write_image erase {slot_b_img} {SLOT_B:#x} bin",
            f"flash erase_address {STATE_BASE:#x} {STATE_SIZE:#x}",
            f"flash write_image {first} {STATE_BASE:#x} bin",
            "reset run", "exit")


def run_one(name, expect):
    _, data = read_state()
    idx, cur = newest(data)
    # The run writes two records per image, far fewer than a page holds, so
    # the log never rotates. Anything else means the state is not what the
    # run set up.
    if cur is None or idx + 1 >= BR_PER_PAGE:
        sys.exit("boot state log is not in the expected shape; aborting")

    # Mark slot B PENDING the way the update agent will: a full snapshot of
    # the current state with one field changed
    seq = cur["seq"] + 1
    pending = write_record_file(
        f"rec_{name}.bin",
        make_record(seq, cur["primary"], cur["state"][0], PENDING,
                    cur["attempts"], cur["floor"], cur["last"]))
    openocd("init", "reset halt",
            f"flash write_image erase {os.path.join(OUT, name)} {SLOT_B:#x} bin",
            f"flash write_image {pending} {STATE_BASE + (idx + 1) * BR_SIZE:#x} bin",
            "reset run", "exit")

    pc, rec = wait_for_boot(seq + 1)
    if rec is None:
        return False, "the board did not finish booting within the timeout"

    want = REASONS[expect.split()[0]]
    if want == RESULT_OK:
        ok = rec["state"][1] == TRIAL and rec["attempts"] == 1 and slot_of(pc) == "B"
        got = f"slot B state {rec['state'][1]}, attempts {rec['attempts']}, " \
              f"running from {slot_of(pc)}"
    else:
        ok = (rec["state"][1] == REJECTED and rec["last"] == want
              and slot_of(pc) == "A")
        got = f"slot B state {rec['state'][1]}, reason {rec['last']}, " \
              f"running from {slot_of(pc)}"
    return ok, got


def main():
    os.chdir(ROOT)
    for path in (KEY, os.path.join(BUILD, "app_slot_a.img"),
                 os.path.join(BUILD, "app_slot_b.img")):
        if not os.path.exists(path):
            sys.exit(f"missing {path}: build the firmware first")

    expectations = generate()

    print(f"\nPreparing: slot A at security counter {FLOOR}, floor {FLOOR}")
    reset_state(os.path.join(OUT, "slot_a_floor.img"),
                os.path.join(BUILD, "app_slot_b.img"), FLOOR)
    pc, rec = wait_for_boot(1)
    if rec is None or rec["seq"] != 1 or slot_of(pc) != "A":
        sys.exit("board did not come up on slot A with a single record; aborting")

    failures = 0
    print(f"\n{'image':<24} {'expected':<18} result")
    for name, expect in expectations:
        ok, got = run_one(name, expect)
        failures += not ok
        print(f"{name:<24} {expect.split()[0]:<18} {'ok  ' if ok else 'FAIL'} {got}")

    print("\nRestoring: build-fw images, fresh boot state, floor 0")
    reset_state(os.path.join(BUILD, "app_slot_a.img"),
                os.path.join(BUILD, "app_slot_b.img"), 0)

    print(f"\n{len(expectations)} images, {failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
