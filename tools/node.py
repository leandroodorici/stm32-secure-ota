#!/usr/bin/env python3
"""Talk to a node over its serial link.

    .venv/bin/python tools/node.py status
    .venv/bin/python tools/node.py status --port /dev/ttyACM0
    .venv/bin/python tools/node.py update build-fw/app_slot_b.img
    .venv/bin/python tools/node.py update build-fw/app_slot_b.img --reboot
    .venv/bin/python tools/node.py reboot
"""

import argparse
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import link  # noqa: E402


def cmd_status(args):
    lk = link.Link(args.port)
    try:
        s = lk.status()
    finally:
        lk.close()
    if s is None:
        sys.exit("no STATUS reply within 1 s")

    slot = "AB"[s["running_slot"]] if s["running_slot"] < 2 else "?"
    states = [link.SLOT_STATES.get(x, str(x)) for x in s["slot_state"]]
    print(f"node            {s['node']}")
    print(f"running slot    {slot}")
    print(f"slot A          {states[0]}")
    print(f"slot B          {states[1]}")
    print(f"trial attempts  {s['trial_attempts']}")
    print(f"image           v{link.format_version(s['image_version'])}, "
          f"security counter {s['image_counter']}")
    print(f"security floor  {s['security_floor']}")
    print(f"last result     "
          f"{link.RESULTS.get(s['last_result'], str(s['last_result']))}")
    print(f"reset cause     {link.format_reset(s['reset_flags'])}")
    print(f"boot record     seq {s['boot_seq']}")


def cmd_update(args):
    with open(args.image, "rb") as f:
        image = f.read()

    def progress(offset, total):
        print(f"\r  {offset} of {total} bytes", end="", flush=True)

    lk = link.Link(args.port)
    try:
        start = time.monotonic()
        try:
            resumed = link.update(lk, image, args.chunk, progress)
        except link.UpdateError as e:
            print()
            sys.exit(f"update failed: {e}")
        print()
        if resumed:
            print(f"resumed from offset {resumed}")
        print(f"verified and marked PENDING in {time.monotonic() - start:.1f} s")
        if args.reboot:
            lk.send(link.reboot(0))
            print("REBOOT sent: the bootloader starts the new image on trial")
    finally:
        lk.close()


def cmd_reboot(args):
    lk = link.Link(args.port)
    try:
        lk.send(link.reboot(args.delay))
    finally:
        lk.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=link.default_port())
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status").set_defaults(func=cmd_status)

    p = sub.add_parser("update", help="send a signed image to the slot it was signed for")
    p.add_argument("image")
    p.add_argument("--chunk", type=int, default=link.CHUNK_MAX)
    p.add_argument("--reboot", action="store_true", help="reboot once it is PENDING")
    p.set_defaults(func=cmd_update)

    p = sub.add_parser("reboot")
    p.add_argument("--delay", type=int, default=0, help="milliseconds")
    p.set_defaults(func=cmd_reboot)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
