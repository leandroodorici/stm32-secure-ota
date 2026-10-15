#!/usr/bin/env python3
"""On-target tests for updates over UART and confirmation

Plays the gateway over the serial link. SWD is used only to set the board up
and restore it, to reset it where a test needs a reset the node did not ask
for, and to watch the boot record while nothing may be sent over the link.

  1. an update to slot B confirms once the gateway asks for STATUS, making
     slot B primary and raising the floor to its security counter
  2. an image below that floor is refused at OFFER
  3. an update back to slot A confirms, and slot A is primary again
  4. a node on trial refuses an update
  5. a trial reset three times before it confirms is rolled back
  6. a trial the gateway never asks about resets itself at the deadline, and
     is rolled back after three attempts with no intervention. About three
     minutes. --quick skips it.

Images are signed again from build-fw/ with the development key, at rising
security counters, so the bootloader on the board must have been built with
the matching public key.

DESTRUCTIVE: erases both boot state pages and overwrites both slots. At the
end it restores slot A and slot B from build-fw/ and writes a fresh initial
boot record: slot A primary and confirmed, slot B empty, floor 0.

    .venv/bin/python tools/hw_update_test.py [--quick]
"""

import argparse
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import hw_negative_test as hw  # noqa: E402
import link  # noqa: E402

OUT = os.path.join(hw.BUILD, "hw_update")
CONFIRMED, REJECTED = link.SLOT_CONFIRMED, link.SLOT_REJECTED
NOT_CONFIRMED = 16
ROLLBACK_BLOCKED = 15
BUSY_TRIAL = 32

# app/comms.c TRIAL_DEADLINE_MS, and how long three trial boots may take
DEADLINE_S = 60.0
ROLLBACK_TIMEOUT_S = 3 * DEADLINE_S + 60.0

# A boot verifies one image, about 2 s at 16 MHz, before the node answers
BOOT_S = 4.0


def sign(name, slot, counter):
    # The build's binary for that slot, signed at this counter, with the
    # counter as the minor version so STATUS tells the images apart
    out = os.path.join(OUT, name)
    subprocess.run(hw.IMGTOOL + [
        "sign", "--key", hw.KEY, "--in", os.path.join(hw.BUILD, f"app_slot_{slot}.bin"),
        "--out", out, "--load-addr", hex(hw.SLOT_A if slot == "a" else hw.SLOT_B),
        "--version", f"0.{counter}.0", "--security-counter", str(counter)],
        check=True, capture_output=True)
    with open(out, "rb") as f:
        return f.read()


def wait_status(lk, timeout=15.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        s = lk.status()
        if s is not None:
            return s
    return None


def describe(s):
    states = [link.SLOT_STATES.get(x, str(x)) for x in s["slot_state"]]
    return (f"running {'AB'[s['running_slot']]}, A {states[0]}, B {states[1]}, "
            f"counter {s['image_counter']}, floor {s['security_floor']}, "
            f"last {link.result_name(s['last_result'])}, "
            f"reset {link.format_reset(s['reset_flags'])}")


def reset_board():
    hw.openocd("init", "reset run", "exit")


def install(lk, image):
    # Update, REBOOT, then ask for STATUS until the trial ends either way
    link.update(lk, image)
    lk.send(link.reboot(0))
    return link.await_outcome(lk.status, link.image_header(image)[0])


def offer_reply(lk, image):
    return link.request(lk, link.image_offer(image), {link.MSG_ACCEPT, link.MSG_REJECT})


# ---- the tests ----
# Each returns (ok, what was seen) and leaves the board for the next

def test_confirm_b(lk, imgs):
    confirmed, s = install(lk, imgs["b2"])
    ok = (confirmed and s["running_slot"] == 1 and s["slot_state"] == [CONFIRMED, CONFIRMED]
          and s["image_counter"] == 2 and s["security_floor"] == 2)
    return ok, describe(s)


def test_below_floor_refused(lk, imgs):
    reply = offer_reply(lk, imgs["a1"])
    ok = reply == (link.MSG_REJECT, ROLLBACK_BLOCKED)
    return ok, f"OFFER at counter 1 answered {reply}"


def test_confirm_a(lk, imgs):
    confirmed, s = install(lk, imgs["a3"])
    ok = (confirmed and s["running_slot"] == 0 and s["slot_state"] == [CONFIRMED, CONFIRMED]
          and s["image_counter"] == 3 and s["security_floor"] == 3)
    return ok, describe(s)


def test_trial_refuses_and_rolls_back(lk, imgs):
    # Nothing but an OFFER is sent while slot B is on trial, so it is never
    # confirmed. Three trial boots, then the bootloader returns to slot A.
    link.update(lk, imgs["b4"])
    lk.send(link.reboot(0))
    time.sleep(BOOT_S)
    reply = offer_reply(lk, imgs["a3"])
    for _ in range(3):
        reset_board()
        time.sleep(BOOT_S)
    s = wait_status(lk)
    if s is None:
        return False, "no STATUS after the resets"
    ok = (reply == (link.MSG_REJECT, BUSY_TRIAL) and s["running_slot"] == 0
          and s["slot_state"] == [CONFIRMED, REJECTED]
          and s["last_result"] == NOT_CONFIRMED and s["security_floor"] == 3)
    return ok, f"OFFER on trial answered {reply}; then {describe(s)}"


def test_deadline_rolls_back(lk, imgs):
    # Silence on the link: the node can only leave the trial by its own
    # deadline. Watched over SWD, because a STATUS_REQ would confirm it.
    link.update(lk, imgs["b5"])
    _, data = hw.read_state()
    _, before = hw.newest(data)
    lk.send(link.reboot(0))
    start = time.monotonic()
    rec = None
    while time.monotonic() - start < ROLLBACK_TIMEOUT_S:
        time.sleep(5.0)
        _, data = hw.read_state()
        _, rec = hw.newest(data)
        if rec["seq"] > before["seq"] and rec["state"][1] == REJECTED:
            break
    took = time.monotonic() - start
    time.sleep(BOOT_S)
    s = wait_status(lk)
    if s is None:
        return False, "no STATUS after the rollback"
    flags = link.format_reset(s["reset_flags"])
    # Three deadlines, so at least 3 x 60 s. A watchdog reset would have
    # rolled back in seconds, and shown IWDG.
    ok = (rec["state"][1] == REJECTED and took >= 3 * DEADLINE_S
          and s["running_slot"] == 0 and s["last_result"] == NOT_CONFIRMED
          and "SW" in flags and "IWDG" not in flags)
    return ok, f"rolled back after {took:.0f} s; {describe(s)}"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=link.default_port())
    ap.add_argument("--quick", action="store_true", help="skip the deadline test")
    args = ap.parse_args()

    os.chdir(hw.ROOT)
    for path in (hw.KEY, os.path.join(hw.BUILD, "app_slot_a.bin"),
                 os.path.join(hw.BUILD, "app_slot_b.bin"),
                 os.path.join(hw.BUILD, "app_slot_a.img"),
                 os.path.join(hw.BUILD, "app_slot_b.img")):
        if not os.path.exists(path):
            sys.exit(f"missing {path}: build the firmware first")
    os.makedirs(OUT, exist_ok=True)
    os.makedirs(hw.OUT, exist_ok=True)  # reset_state writes its record there
    imgs = {"a1": sign("a1.img", "a", 1), "b2": sign("b2.img", "b", 2),
            "a3": sign("a3.img", "a", 3), "b4": sign("b4.img", "b", 4),
            "b5": sign("b5.img", "b", 5)}

    tests = [("update B, confirm", test_confirm_b),
             ("below the floor", test_below_floor_refused),
             ("update A, confirm", test_confirm_a),
             ("trial: refuse, roll back", test_trial_refuses_and_rolls_back)]
    if not args.quick:
        tests.append(("trial deadline", test_deadline_rolls_back))

    print("Preparing: slot A at security counter 1, slot B empty, floor 0")
    hw.reset_state(os.path.join(OUT, "a1.img"), os.path.join(hw.BUILD, "app_slot_b.img"), 0)
    lk = link.Link(args.port)
    failures = 0
    try:
        time.sleep(BOOT_S)
        s = wait_status(lk)
        if s is None or s["running_slot"] != 0 or s["security_floor"] != 0:
            sys.exit("board did not come up on slot A with floor 0; aborting")

        for name, test in tests:
            start = time.monotonic()
            try:
                ok, got = test(lk, imgs)
            except link.UpdateError as e:
                ok, got = False, str(e)
            failures += not ok
            took = time.monotonic() - start
            print(f"{'ok  ' if ok else 'FAIL'} {name:<26} {took:5.0f} s  {got}")
            if not ok:
                break  # each test starts from the state the previous one left
    finally:
        print("\nRestoring: build-fw images, fresh boot state, floor 0")
        hw.reset_state(os.path.join(hw.BUILD, "app_slot_a.img"),
                       os.path.join(hw.BUILD, "app_slot_b.img"), 0)
        lk.close()

    print(f"\n{len(tests)} tests, {failures} failure(s)")
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
