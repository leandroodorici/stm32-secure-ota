"""Host side of the node link: framing, messages and the serial transport.

Mirrors common/frame.c and common/protocol.c. Both sides are tested against
the same fixed bytes (tools/test_link.py and tests/host/test_frame.c,
tests/host/test_protocol.c), so they cannot drift apart.
"""

import glob
import hashlib
import struct
import time
import zlib

# ---- framing: COBS( payload || CRC-32 ) 0x00, see common/include/frame.h ----

FRAME_MAX_PAYLOAD = 1088


def cobs_encode(data):
    out = bytearray()
    block = bytearray()
    for b in data:
        if b == 0:
            out.append(len(block) + 1)
            out += block
            block = bytearray()
        else:
            block.append(b)
            if len(block) == 254:
                out.append(0xFF)
                out += block
                block = bytearray()
    out.append(len(block) + 1)
    out += block
    return bytes(out)


def cobs_decode(data):
    # Returns the decoded bytes, or None if the encoding is malformed.
    out = bytearray()
    i = 0
    while i < len(data):
        code = data[i]
        i += 1
        if code == 0 or i + code - 1 > len(data):
            return None
        out += data[i:i + code - 1]
        i += code - 1
        if code != 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def frame_encode(payload):
    if not 0 < len(payload) <= FRAME_MAX_PAYLOAD:
        raise ValueError("payload size out of range")
    return cobs_encode(payload + struct.pack("<I", zlib.crc32(payload))) + b"\x00"


def frame_decode(encoded):
    # Decodes one frame without its delimiter. Returns the payload, or None
    # if the frame is malformed or fails its CRC.
    raw = cobs_decode(encoded)
    if raw is None or len(raw) <= 4:
        return None
    payload, crc = raw[:-4], struct.unpack("<I", raw[-4:])[0]
    return payload if zlib.crc32(payload) == crc else None


# ---- messages, see common/include/protocol.h ----

PROTO_VERSION = 1
NODE_BROADCAST = 0xFF
MSG_STATUS_REQ = 0x01
MSG_STATUS = 0x02
MSG_OFFER = 0x10
MSG_ACCEPT = 0x11
MSG_REJECT = 0x12
MSG_CHUNK = 0x20
MSG_CHUNK_ACK = 0x21
MSG_COMMIT = 0x30
MSG_VERIFY_RESULT = 0x31
MSG_REBOOT = 0x40
MSG_ABORT = 0x7F

RESULT_OK = 1
UPD_ERR_CHUNK_CRC = 36

CHUNK_MAX = 1024

SLOT_STATES = {0: "EMPTY", 1: "PENDING", 2: "TRIAL", 3: "CONFIRMED", 4: "REJECTED"}
RESULTS = {0: "NONE", 1: "OK", 10: "BAD_MAGIC", 11: "BAD_HEADER", 12: "BAD_HASH",
           13: "BAD_SIGNATURE", 14: "WRONG_SLOT", 15: "ROLLBACK_BLOCKED",
           16: "NOT_CONFIRMED", 17: "FLASH_ERROR", 18: "CRYPTO_ERROR",
           # Update reasons, upd_reason_t in common/include/protocol.h
           32: "BUSY_TRIAL", 33: "TOO_LARGE", 34: "CHUNK_SIZE", 35: "NO_SESSION",
           36: "CHUNK_CRC", 37: "INCOMPLETE", 38: "CANCELLED"}
RESET_FLAGS = [(1 << 25, "OBL"), (1 << 26, "PIN"), (1 << 27, "BOR"), (1 << 28, "SW"),
               (1 << 29, "IWDG"), (1 << 30, "WWDG"), (1 << 31, "LPWR")]


def header(msg_type, node):
    return bytes([PROTO_VERSION, msg_type, node, 0])


def status_req(node=NODE_BROADCAST):
    return header(MSG_STATUS_REQ, node)


def offer(image_size, version, counter, target_slot, chunk_size, digest,
          node=NODE_BROADCAST):
    if len(digest) != 32:
        raise ValueError("digest must be 32 bytes")
    return header(MSG_OFFER, node) + struct.pack(
        "<IIIBxxxHxx", image_size, version, counter, target_slot, chunk_size) + digest


def chunk(offset, data, node=NODE_BROADCAST):
    if len(data) > CHUNK_MAX:
        raise ValueError("chunk too large")
    return header(MSG_CHUNK, node) + struct.pack(
        "<IHxxI", offset, len(data), zlib.crc32(data)) + data


def commit(node=NODE_BROADCAST):
    return header(MSG_COMMIT, node)


def reboot(delay_ms, node=NODE_BROADCAST):
    return header(MSG_REBOOT, node) + struct.pack("<I", delay_ms)


def abort(reason, node=NODE_BROADCAST):
    return header(MSG_ABORT, node) + struct.pack("<I", reason)


def decode_u32(msg):
    # Decodes a reply carrying a single value: ACCEPT, REJECT, CHUNK_ACK,
    # VERIFY_RESULT or ABORT. Returns (type, value), or None.
    if len(msg) != 8 or msg[0] != PROTO_VERSION:
        return None
    return msg[1], struct.unpack_from("<I", msg, 4)[0]


def decode_status(msg):
    #Returns a dict for a STATUS message, or None for anything else.
    if len(msg) != 32 or msg[0] != PROTO_VERSION or msg[1] != MSG_STATUS:
        return None
    (version, counter, floor, last, reset, seq) = struct.unpack_from("<IIIIII", msg, 8)
    return {"node": msg[2], "running_slot": msg[4], "slot_state": [msg[5], msg[6]],
            "trial_attempts": msg[7], "image_version": version, "image_counter": counter,
            "security_floor": floor, "last_result": last, "reset_flags": reset,
            "boot_seq": seq}


def format_version(v):
    return f"{v >> 24}.{(v >> 16) & 0xFF}.{v & 0xFFFF}"


def format_reset(flags):
    names = [name for bit, name in RESET_FLAGS if flags & bit]
    return " + ".join(names) if names else "none"


# ---- updates ----

IMG_MAGIC = 0x31474D49  # "IMG1"
SLOT_BASES = {0x08010000: 0, 0x08040000: 1}


def image_offer(image, chunk_size=CHUNK_MAX, node=NODE_BROADCAST):
    # The OFFER for a signed image, its fields read from the image's own
    # header. The target is the slot the image was signed for.
    if len(image) < 28:
        raise ValueError("too short to be a signed image")
    magic, _, _, load_addr, _, version, _, counter = struct.unpack_from("<IHHIIIII", image)
    if magic != IMG_MAGIC:
        raise ValueError("not a signed image")
    if load_addr not in SLOT_BASES:
        raise ValueError(f"signed for 0x{load_addr:08x}, which is not a slot")
    return offer(len(image), version, counter, SLOT_BASES[load_addr], chunk_size,
                 hashlib.sha256(image).digest(), node)


def result_name(value):
    return RESULTS.get(value, str(value))


def request(lk, payload, types, timeout=1.0, attempts=5):
    # Sends payload until a single-value reply of one of the given types
    # arrives, resending after each timeout. Returns (type, value), or None
    # once every attempt has timed out.
    for _ in range(attempts):
        lk.flush()
        lk.send(payload)
        deadline = time.monotonic() + timeout
        while True:
            msg = lk.receive(max(deadline - time.monotonic(), 0))
            if msg is None:
                break
            reply = decode_u32(msg)
            if reply is not None and reply[0] in types:
                return reply
    return None


class UpdateError(Exception):
    pass


def update(lk, image, chunk_size=CHUNK_MAX, progress=None, timeout=1.0,
           commit_timeout=5.0):
    # Sends a signed image: OFFER, then CHUNKs from the offset the node
    # accepts, then COMMIT. Returns the offset the node resumed from. Raises
    # UpdateError if the node refuses, or stops answering. COMMIT waits
    # longer, since the node verifies the whole image before it answers.
    reply = request(lk, image_offer(image, chunk_size), {MSG_ACCEPT, MSG_REJECT},
                    timeout)
    if reply is None:
        raise UpdateError("no answer to OFFER")
    if reply[0] == MSG_REJECT:
        raise UpdateError(f"OFFER rejected: {result_name(reply[1])}")
    resumed = offset = reply[1]

    damaged = 0
    while offset < len(image):
        data = image[offset:offset + chunk_size]
        reply = request(lk, chunk(offset, data), {MSG_CHUNK_ACK, MSG_REJECT, MSG_ABORT},
                        timeout)
        if reply is None:
            raise UpdateError(f"no answer to the chunk at {offset}")
        kind, value = reply
        if kind == MSG_CHUNK_ACK and value <= len(image):
            # The node names the offset it expects next. Following it, rather
            # than counting, also recovers from a reply that was lost.
            offset = value
            damaged = 0
        elif kind == MSG_REJECT and value == UPD_ERR_CHUNK_CRC and damaged < 5:
            damaged += 1  # damaged on the way: send it again
            continue
        else:
            raise UpdateError(f"chunk at {offset}: {result_name(value)}")
        if progress is not None:
            progress(offset, len(image))

    # Once only: the node ends the session on COMMIT, so a second COMMIT
    # would be answered NO_SESSION whatever the outcome of the first
    reply = request(lk, commit(), {MSG_VERIFY_RESULT, MSG_REJECT}, commit_timeout,
                    attempts=1)
    if reply is None:
        raise UpdateError("no answer to COMMIT")
    if reply[0] != MSG_VERIFY_RESULT or reply[1] != RESULT_OK:
        raise UpdateError(f"COMMIT: {result_name(reply[1])}")
    return resumed


# ---- serial transport ----

def default_port():
    # The Nucleo's ST-LINK serial port, by its stable name if it is present.
    ports = sorted(glob.glob("/dev/serial/by-id/*STLINK*-if02"))
    return ports[0] if ports else "/dev/ttyACM0"


class Link:
    def __init__(self, port, baud=115200):
        import serial  # only needed when talking to hardware
        self.ser = serial.Serial(port, baud, timeout=0.1)
        self.rx = bytearray()

    def close(self):
        self.ser.flush()  # a message just sent leaves before the port closes
        self.ser.close()

    def flush(self):
        # Drops anything received but not yet read, such as a late reply to
        # an earlier request
        self.ser.reset_input_buffer()
        self.rx.clear()

    def send(self, payload):
        # A leading delimiter ends any partial frame the node may hold
        self.ser.write(b"\x00" + frame_encode(payload))

    def receive(self, timeout):
        # Returns the next valid frame's payload, or None after timeout.
        # Invalid frames are skipped, as the node does.
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self.rx += self.ser.read(256)
            while b"\x00" in self.rx:
                encoded, _, rest = bytes(self.rx).partition(b"\x00")
                self.rx = bytearray(rest)
                if encoded:
                    payload = frame_decode(encoded)
                    if payload is not None:
                        return payload
        return None

    def status(self, node=NODE_BROADCAST, timeout=1.0):
        self.flush()
        self.send(status_req(node))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            msg = self.receive(deadline - time.monotonic())
            if msg is not None:
                s = decode_status(msg)
                if s is not None:
                    return s
        return None
