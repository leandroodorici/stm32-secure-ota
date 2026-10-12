#!/usr/bin/env python3
"""Tests for link.py. Run: python3 tools/test_link.py

The fixed frames and the STATUS message are the same bytes the C tests use
(tests/host/test_frame.c and tests/host/test_protocol.c), so the host tool and
the node are held to one definition.
"""

import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import link  # noqa: E402

failures = []


def check(cond, msg):
    print(f"  {'ok  ' if cond else 'FAIL'} {msg}")
    if not cond:
        failures.append(msg)


# Same vectors as test_known_frames in tests/host/test_frame.c
FRAMES = [
    (bytes([0x01, 0x00, 0x02]), bytes.fromhex("0201060209d28d1000")),
    (bytes([0x00]), bytes.fromhex("01058def02d200")),
    (bytes([0x01, 0x02, 0x03, 0x04, 0x05]), bytes.fromhex("0a0102030405f4990b4700")),
]

# Same bytes as k_status in tests/host/test_protocol.c
STATUS = bytes([
    0x01, 0x02, 0x01, 0x00, 0x01, 0x03, 0x04, 0x02, 0x03, 0x00, 0x02,
    0x01, 0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x0b, 0x00, 0x00, 0x00,
])

print("framing")
for payload, wire in FRAMES:
    check(link.frame_encode(payload) == wire, f"encode {payload.hex()}")
    check(link.frame_decode(wire[:-1]) == payload, f"decode {payload.hex()}")

rng = random.Random(1)
ok = True
for size in (1, 2, 249, 250, 251, 252, 253, 254, 255, 503, 504, 505, 508, 509, 1087, 1088):
    for p in (bytes(size), b"\xff" * size, bytes(rng.randrange(256) for _ in range(size))):
        wire = link.frame_encode(p)
        ok &= 0 not in wire[:-1] and wire[-1] == 0 and link.frame_decode(wire[:-1]) == p
check(ok, "round trips around COBS block boundaries")

wire = bytearray(link.frame_encode(b"\x10\x20\x30\x40"))
wire[2] ^= 0x01
check(link.frame_decode(bytes(wire[:-1])) is None, "a corrupted frame is rejected")

print("messages")
check(link.status_req() == bytes([1, 1, 0xFF, 0]), "STATUS_REQ is a broadcast header")
s = link.decode_status(STATUS)
check(s is not None, "the reference STATUS decodes")
check(s == {"node": 1, "running_slot": 1, "slot_state": [3, 4], "trial_attempts": 2,
            "image_version": (1 << 24) | (2 << 16) | 3, "image_counter": 5,
            "security_floor": 4, "last_result": 16, "reset_flags": 0x24000000,
            "boot_seq": 11}, "every STATUS field decodes to the C test's value")
check(link.format_version(s["image_version"]) == "1.2.3", "version formatting")
check(link.format_reset(s["reset_flags"]) == "PIN + IWDG", "reset cause formatting")
check(link.decode_status(STATUS[:-1]) is None, "a short STATUS is rejected")

print("update messages")
# Same bytes as k_offer and k_chunk in tests/host/test_protocol.c
OFFER = bytes([
    0x01, 0x10, 0x01, 0x00, 0x4c, 0x28, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
    0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
])
CHUNK = bytes([
    0x01, 0x20, 0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0xc5, 0x88, 0xca, 0x3f, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
])
check(link.offer(10316, 0x00020000, 2, 1, 1024, bytes(range(32)), node=1) == OFFER,
      "OFFER encodes to the C test's bytes")
check(link.chunk(1024, bytes(range(1, 9)), node=1) == CHUNK,
      "CHUNK encodes to the C test's bytes")
check(link.commit(node=1) == bytes([1, 0x30, 1, 0]), "COMMIT is a bare header")
check(link.reboot(500, node=1) == bytes([1, 0x40, 1, 0, 0xf4, 0x01, 0, 0]),
      "REBOOT encodes to the C test's bytes")
check(link.abort(38, node=1) == bytes([1, 0x7f, 1, 0, 0x26, 0, 0, 0]),
      "ABORT encodes to the C test's bytes")

# Same bytes as the single-value references in tests/host/test_protocol.c
for name, msg_type, value, raw in [
        ("ACCEPT", link.MSG_ACCEPT, 2048, "0111010000080000"),
        ("REJECT", link.MSG_REJECT, 15, "011201000f000000"),
        ("CHUNK_ACK", link.MSG_CHUNK_ACK, 3072, "01210100000c0000"),
        ("VERIFY_RESULT", link.MSG_VERIFY_RESULT, 1, "0131010001000000")]:
    check(link.decode_u32(bytes.fromhex(raw)) == (msg_type, value), f"{name} decodes")
check(link.decode_u32(bytes.fromhex("01110100000800")) is None, "a short reply is rejected")

print(f"\n{len(failures)} failure(s)")
sys.exit(1 if failures else 0)
