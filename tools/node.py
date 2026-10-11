#!/usr/bin/env python3
"""Talk to a node over its serial link.

    .venv/bin/python tools/node.py status
    .venv/bin/python tools/node.py status --port /dev/ttyACM0
"""

import argparse
import os
import sys

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


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default=link.default_port())
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("status").set_defaults(func=cmd_status)
    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
