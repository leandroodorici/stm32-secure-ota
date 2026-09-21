#!/usr/bin/env python3
"""Build, sign, verify and inspect firmware images.

Image layout (docs/DESIGN.md section 5)

The signature covers header + payload, so load_addr, version and
security_counter cannot be altered without invalidating it.
"""

import argparse
import hashlib
import os
import struct
import sys

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import (
    decode_dss_signature,
    encode_dss_signature,
)

IMG_MAGIC = 0x31474D49  # "IMG1"
IMG_HDR_SIZE = 0x200

# magic, header_version, header_size, load_addr, payload_size, version,
# build_number, security_counter, flags. Must match image_header_t exactly.
HDR_FMT = "<IHHIIIIII"
HDR_FIXED_LEN = struct.calcsize(HDR_FMT)  # 32
OFF_SECURITY_COUNTER = 24

TLV_SHA256 = 0x0010
TLV_KEY_ID = 0x0020
TLV_ECDSA_P256 = 0x0022
SIG_LEN = 64   # raw r||s
KEY_ID_LEN = 8
TRAILER_LEN = (4 + 32) + (4 + KEY_ID_LEN) + (4 + SIG_LEN)

SLOT_SIZE = 192 * 1024
SLOTS = {0x08010000: "A", 0x08040000: "B"}


# ---- helpers ----

def parse_version(s):
    major, minor, patch = (int(p) for p in s.split("."))
    if not (0 <= major < 256 and 0 <= minor < 256 and 0 <= patch < 65536):
        raise ValueError("version out of range")
    return (major << 24) | (minor << 16) | patch


def format_version(v):
    return f"{v >> 24}.{(v >> 16) & 0xFF}.{v & 0xFFFF}"


def tlv(tag, value):
    return struct.pack("<HH", tag, len(value)) + value


def parse_tlvs(blob):
    out, off = {}, 0
    while off + 4 <= len(blob):
        tag, length = struct.unpack_from("<HH", blob, off)
        off += 4
        if off + length > len(blob):
            raise ValueError("TLV length runs past end of trailer")
        out[tag] = blob[off:off + length]
        off += length
    return out


def key_id(pub):
    # First 8 bytes of SHA-256 over the uncompressed point. Lets a future
    # bootloader hold several keys and select by ID without a format change.
    raw = pub.public_bytes(serialization.Encoding.X962,
                           serialization.PublicFormat.UncompressedPoint)
    return hashlib.sha256(raw).digest()[:KEY_ID_LEN]


def load_priv(path):
    with open(path, "rb") as f:
        key = serialization.load_pem_private_key(f.read(), password=None)
    if not isinstance(key, ec.EllipticCurvePrivateKey) or key.curve.name != "secp256r1":
        raise ValueError("expected a P-256 private key")
    return key


def load_pub(path):
    with open(path, "rb") as f:
        key = serialization.load_pem_public_key(f.read())
    if key.curve.name != "secp256r1":
        raise ValueError("expected a P-256 public key")
    return key


def sign_raw(priv, data):
    # The library returns DER. The bootloader wants a fixed 64-byte r||s so it
    # never runs an ASN.1 parser on unauthenticated input.
    r, s = decode_dss_signature(priv.sign(data, ec.ECDSA(hashes.SHA256())))
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


def build_header(load_addr, payload_size, version, build, counter,
                 magic=IMG_MAGIC, header_size=IMG_HDR_SIZE):
    fixed = struct.pack(HDR_FMT, magic, 1, header_size, load_addr,
                        payload_size, version, build, counter, 0)
    return fixed + b"\x00" * (IMG_HDR_SIZE - HDR_FIXED_LEN)


# ---- image construction ----

def make_image(priv, payload, load_addr, version, build, counter, corrupt=None):
    # Builds a signed image. "corrupt" names a single deliberate defect, so
    # bad images go through the same code path as good ones and differ in
    # exactly one way.
    magic = 0xDEADBEEF if corrupt == "bad_magic" else IMG_MAGIC
    hdr_size = 0x100 if corrupt == "bad_header_size" else IMG_HDR_SIZE
    size = 0xFFFFFF00 if corrupt == "size_overflow" else len(payload)

    signed = build_header(load_addr, size, version, build, counter, magic, hdr_size) + payload
    shipped = bytearray(signed)

    # Tampering happens AFTER signing, and the attacker recomputes the hash to
    # match. That leaves a self-consistent image with a signature that fails,
    # which is exactly the case the separate hash TLV exists to distinguish.
    if corrupt == "tampered_payload":
        shipped[IMG_HDR_SIZE + 16] ^= 0xFF
    if corrupt == "tampered_header":
        struct.pack_into("<I", shipped, OFF_SECURITY_COUNTER, counter + 100)
    shipped = bytes(shipped)

    signer = ec.generate_private_key(ec.SECP256R1()) if corrupt == "wrong_key" else priv
    sig = sign_raw(signer, signed)
    if corrupt == "bad_signature":
        sig = bytes([sig[0] ^ 0xFF]) + sig[1:]

    digest = hashlib.sha256(shipped).digest()
    if corrupt == "bad_hash":
        digest = bytes([digest[0] ^ 0xFF]) + digest[1:]

    trailer = (tlv(TLV_SHA256, digest)
               + tlv(TLV_KEY_ID, key_id(priv.public_key()))
               + tlv(TLV_ECDSA_P256, sig))
    if corrupt == "malformed_tlv":
        # Keep the trailer full size so the bounds check passes, and corrupt
        # only the first TLV's length field. Otherwise the image is rejected
        # by bounds checking and the TLV parser is never exercised.
        t = bytearray(trailer)
        struct.pack_into("<H", t, 2, 0xFFFF)
        trailer = bytes(t)

    img = shipped + trailer
    if corrupt == "truncated":
        img = img[:len(img) // 2]
    return img


# ---- verification ----

def verify_image(pub, data):
    # Returns (ok, reason). Checks in the SAME order the bootloader will:
    # bounds first, then hash, then signature. A verifier that checks in a
    # different order than the device gives false confidence.
    if len(data) < IMG_HDR_SIZE:
        return False, "BAD_HEADER: shorter than a header"
    (magic, _hv, hdr_size, _load, payload_size,
     _ver, _build, _counter, _flags) = struct.unpack_from(HDR_FMT, data, 0)

    if magic != IMG_MAGIC:
        return False, f"BAD_MAGIC: {magic:#010x}"
    if hdr_size != IMG_HDR_SIZE:
        return False, f"BAD_HEADER: header_size {hdr_size:#x}"
    end = IMG_HDR_SIZE + payload_size
    if payload_size > SLOT_SIZE or end + TRAILER_LEN > len(data):
        return False, "BAD_HEADER: payload or trailer out of bounds"

    try:
        tlvs = parse_tlvs(data[end:])
    except ValueError as e:
        return False, f"BAD_HEADER: {e}"
    for tag, length in ((TLV_SHA256, 32), (TLV_KEY_ID, KEY_ID_LEN),
                        (TLV_ECDSA_P256, SIG_LEN)):
        if len(tlvs.get(tag, b"")) != length:
            return False, f"BAD_HEADER: TLV {tag:#06x} missing or wrong length"

    body = data[:end]
    if hashlib.sha256(body).digest() != tlvs[TLV_SHA256]:
        return False, "BAD_HASH: corrupt in transit"

    if tlvs[TLV_KEY_ID] != key_id(pub):
        return False, "BAD_SIGNATURE: key id does not match"
    sig = tlvs[TLV_ECDSA_P256]
    der = encode_dss_signature(int.from_bytes(sig[:32], "big"),
                               int.from_bytes(sig[32:], "big"))
    try:
        pub.verify(der, body, ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        return False, "BAD_SIGNATURE: not authentic"
    return True, "OK"


# ---- commands ----

def cmd_keygen(args):
    if os.path.exists(args.out_priv) and not args.force:
        sys.exit(f"refusing to overwrite {args.out_priv}: every image signed "
                 f"with the old key would be orphaned. Use --force.")
    priv = ec.generate_private_key(ec.SECP256R1())
    os.makedirs(os.path.dirname(args.out_priv) or ".", exist_ok=True)
    pem = priv.private_bytes(serialization.Encoding.PEM,
                             serialization.PrivateFormat.PKCS8,
                             serialization.NoEncryption())
    # Created 0600 from the start, never briefly world-readable.
    fd = os.open(args.out_priv, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(pem)
    with open(args.out_pub, "wb") as f:
        f.write(priv.public_key().public_bytes(
            serialization.Encoding.PEM,
            serialization.PublicFormat.SubjectPublicKeyInfo))
    print(f"key id {key_id(priv.public_key()).hex()}")


def cmd_sign(args):
    priv = load_priv(args.key)
    with open(args.infile, "rb") as f:
        payload = f.read()
    load_addr = int(args.load_addr, 0)
    if load_addr not in SLOTS:
        sys.exit(f"{load_addr:#010x} is not a slot base")
    if IMG_HDR_SIZE + len(payload) + TRAILER_LEN > SLOT_SIZE:
        sys.exit("image does not fit in a slot")
    img = make_image(priv, payload, load_addr, parse_version(args.version),
                     args.build_number, args.security_counter)
    with open(args.out, "wb") as f:
        f.write(img)
    print(f"{args.out}: slot {SLOTS[load_addr]}, {len(img)} bytes, "
          f"v{args.version}, counter {args.security_counter}")


def cmd_verify(args):
    with open(args.infile, "rb") as f:
        ok, reason = verify_image(load_pub(args.key), f.read())
    print(reason)
    sys.exit(0 if ok else 1)


def cmd_dump(args):
    with open(args.infile, "rb") as f:
        data = f.read()
    (magic, hv, hs, load, psize, ver, build, ctr, flags) = \
        struct.unpack_from(HDR_FMT, data, 0)
    print(f"magic            {magic:#010x}")
    print(f"header           v{hv}, {hs:#x} bytes")
    print(f"load_addr        {load:#010x} (slot {SLOTS.get(load, '?')})")
    print(f"payload_size     {psize}")
    print(f"version          {format_version(ver)} build {build}")
    print(f"security_counter {ctr}")


def cmd_pubkey_c(args):
    pub = load_pub(args.key)
    raw = pub.public_bytes(serialization.Encoding.X962,
                           serialization.PublicFormat.UncompressedPoint)
    lines = ["// Generated by tools/imgtool.py. Do not edit.",
             f"// Key ID: {key_id(pub).hex()}",
             "#include <stdint.h>",
             f"const uint8_t boot_pubkey[{len(raw)}] = {{"]
    for i in range(0, len(raw), 12):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in raw[i:i + 12]) + ",")
    lines.append("};")
    with open(args.out, "w") as f:
        f.write("\n".join(lines) + "\n")


BAD = [
    ("bad_magic", "BAD_MAGIC"),
    ("bad_header_size", "BAD_HEADER"),
    ("size_overflow", "BAD_HEADER"),
    ("truncated", "BAD_HEADER"),
    ("malformed_tlv", "BAD_HEADER"),
    ("bad_hash", "BAD_HASH"),
    ("tampered_payload", "BAD_SIGNATURE"),
    ("tampered_header", "BAD_SIGNATURE"),
    ("bad_signature", "BAD_SIGNATURE"),
    ("wrong_key", "BAD_SIGNATURE"),
]


def cmd_gen_bad(args):
    priv = load_priv(args.key)
    with open(args.infile, "rb") as f:
        payload = f.read()
    os.makedirs(args.outdir, exist_ok=True)
    slot_a, slot_b, v = 0x08010000, 0x08040000, parse_version("1.0.0")

    images = [("good", make_image(priv, payload, slot_a, v, 1, 5), "OK")]
    for name, expect in BAD:
        images.append((name, make_image(priv, payload, slot_a, v, 1, 5, name), expect))
    # These two are validly signed. They are POLICY violations, enforced by
    # the bootloader against device state, not cryptographic ones.
    images.append(("wrong_slot", make_image(priv, payload, slot_b, v, 1, 5),
                   "WRONG_SLOT (policy)"))
    images.append(("downgrade", make_image(priv, payload, slot_a, v, 1, 1),
                   "ROLLBACK_BLOCKED (policy)"))

    with open(os.path.join(args.outdir, "EXPECTATIONS.txt"), "w") as manifest:
        for name, img, expect in images:
            with open(os.path.join(args.outdir, f"{name}.img"), "wb") as f:
                f.write(img)
            manifest.write(f"{name}.img\t{expect}\n")
    print(f"wrote {len(images)} images to {args.outdir}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("keygen")
    p.add_argument("--out-priv", default="keys/dev_private.pem")
    p.add_argument("--out-pub", default="keys/dev_public.pem")
    p.add_argument("--force", action="store_true")
    p.set_defaults(func=cmd_keygen)

    p = sub.add_parser("sign")
    p.add_argument("--key", required=True)
    p.add_argument("--in", dest="infile", required=True)
    p.add_argument("--out", required=True)
    p.add_argument("--load-addr", required=True)
    p.add_argument("--version", default="0.1.0")
    p.add_argument("--build-number", type=int, default=0)
    p.add_argument("--security-counter", type=int, default=1)
    p.set_defaults(func=cmd_sign)

    p = sub.add_parser("verify")
    p.add_argument("--key", required=True)
    p.add_argument("--in", dest="infile", required=True)
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("dump")
    p.add_argument("--in", dest="infile", required=True)
    p.set_defaults(func=cmd_dump)

    p = sub.add_parser("pubkey-c")
    p.add_argument("--key", required=True)
    p.add_argument("--out", required=True)
    p.set_defaults(func=cmd_pubkey_c)

    p = sub.add_parser("gen-bad")
    p.add_argument("--key", required=True)
    p.add_argument("--in", dest="infile", required=True)
    p.add_argument("--outdir", default="tests/images")
    p.set_defaults(func=cmd_gen_bad)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()