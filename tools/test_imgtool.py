#!/usr/bin/env python3
"""Tests for imgtool.py. Run: python3 tools/test_imgtool.py

The important assertions are not that signing works, but that every malformed
image is rejected by the intended check. A bootloader that rejects a tampered
image as malformed is hiding a bug, and so is a test that only checks the
category.
"""

import os
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import imgtool  # noqa: E402

from cryptography.hazmat.primitives.asymmetric import ec  # noqa: E402

failures = []


def check(cond, msg):
    print(f"  {'ok  ' if cond else 'FAIL'} {msg}")
    if not cond:
        failures.append(msg)


SLOT_A, SLOT_B = 0x08010000, 0x08040000
V1 = imgtool.parse_version("1.0.0")

# (defect, substring the rejection reason must contain)
# The substring pins the specific check, not just the category.
EXPECTED = [
    ("bad_magic",        "BAD_MAGIC"),
    ("bad_header_size",  "header_size"),
    ("size_overflow",    "out of bounds"),
    ("truncated",        "out of bounds"),
    ("malformed_tlv",    "runs past end"),
    ("bad_hash",         "BAD_HASH"),
    ("tampered_payload", "not authentic"),
    ("tampered_header",  "not authentic"),
    ("bad_signature",    "not authentic"),
    ("wrong_key",        "not authentic"),
]


def main():
    priv = ec.generate_private_key(ec.SECP256R1())
    pub = priv.public_key()
    payload = os.urandom(8192)

    print("== layout ==")
    # The bootloader parses these offsets and can never be updated in the
    # field. If any of these change, deployed devices cannot read new images.
    check(imgtool.HDR_FIXED_LEN == 32, "fixed header fields are 32 bytes")
    check(imgtool.IMG_HDR_SIZE == 0x200, "header is 512 bytes (VTOR alignment)")
    check(imgtool.SIG_LEN == 64, "signature is raw r||s, 64 bytes")
    good = imgtool.make_image(priv, payload, SLOT_A, V1, 7, 5)
    check(len(good) == 0x200 + len(payload) + imgtool.TRAILER_LEN,
          "image length is header + payload + trailer")

    print("== good image ==")
    ok, reason = imgtool.verify_image(pub, good)
    check(ok, f"verifies ({reason})")

    print("== each defect rejected by the intended check ==")
    for defect, needle in EXPECTED:
        img = imgtool.make_image(priv, payload, SLOT_A, V1, 7, 5, corrupt=defect)
        ok, reason = imgtool.verify_image(pub, img)
        check(not ok and needle in reason, f"{defect}: {reason}")

    print("== hash and signature failures are distinguishable ==")
    # The reason both TLVs exist: corruption in transit and deliberate
    # modification need different responses on a remote fleet.
    _, r_hash = imgtool.verify_image(
        pub, imgtool.make_image(priv, payload, SLOT_A, V1, 7, 5, "bad_hash"))
    _, r_sig = imgtool.verify_image(
        pub, imgtool.make_image(priv, payload, SLOT_A, V1, 7, 5, "tampered_payload"))
    check(r_hash.startswith("BAD_HASH") and r_sig.startswith("BAD_SIGNATURE"),
          "corruption and tampering report different reasons")

    print("== policy violations are NOT signature failures ==")
    # Slot and rollback checks compare against device state, so they belong
    # in the bootloader. If these ever start failing here, policy has leaked
    # into the signing tool.
    wrong_slot = imgtool.make_image(priv, payload, SLOT_B, V1, 7, 5)
    downgrade = imgtool.make_image(priv, payload, SLOT_A, V1, 7, 1)
    check(imgtool.verify_image(pub, wrong_slot)[0], "wrong_slot is validly signed")
    check(imgtool.verify_image(pub, downgrade)[0], "downgrade is validly signed")

    print("== unrelated key ==")
    other = ec.generate_private_key(ec.SECP256R1()).public_key()
    ok, reason = imgtool.verify_image(other, good)
    check(not ok and "key id" in reason, f"rejected by key id: {reason}")

    print("== CLI ==")
    with tempfile.TemporaryDirectory() as td:
        priv_path = os.path.join(td, "k.pem")
        pub_path = os.path.join(td, "k.pub")
        run = lambda *a: subprocess.run(  # noqa: E731
            [sys.executable, os.path.join(HERE, "imgtool.py"), *a],
            capture_output=True, text=True)

        r = run("keygen", "--out-priv", priv_path, "--out-pub", pub_path)
        check(r.returncode == 0, "keygen succeeds")
        mode = stat.S_IMODE(os.stat(priv_path).st_mode)
        check(mode == 0o600, f"private key is 0600 (got {mode:o})")

        r = run("keygen", "--out-priv", priv_path, "--out-pub", pub_path)
        check(r.returncode != 0, "keygen refuses to overwrite an existing key")

        app = os.path.join(td, "app.bin")
        img = os.path.join(td, "app.img")
        with open(app, "wb") as f:
            f.write(payload)
        r = run("sign", "--key", priv_path, "--in", app, "--out", img,
                "--load-addr", "0x08010000")
        check(r.returncode == 0, "sign succeeds")
        check(run("verify", "--key", pub_path, "--in", img).returncode == 0,
              "verify exits 0 on a good image")

        with open(img, "r+b") as f:
            f.seek(0x200 + 100)
            f.write(b"\x00")
        check(run("verify", "--key", pub_path, "--in", img).returncode != 0,
              "verify exits non-zero on a modified image")

        r = run("sign", "--key", priv_path, "--in", app, "--out", img,
                "--load-addr", "0x08123456")
        check(r.returncode != 0, "sign refuses an address that is not a slot base")

    print(f"\n{len(failures)} failure(s)")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())