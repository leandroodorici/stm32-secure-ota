"""Host side of the node link: framing, messages and the serial transport.

Mirrors common/frame.c and common/protocol.c. Both sides are tested against
the same fixed bytes (tools/test_link.py and tests/host/test_frame.c,
tests/host/test_protocol.c), so they cannot drift apart.
"""

import glob
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

SLOT_STATES = {0: "EMPTY", 1: "PENDING", 2: "TRIAL", 3: "CONFIRMED", 4: "REJECTED"}
RESULTS = {0: "NONE", 1: "OK", 10: "BAD_MAGIC", 11: "BAD_HEADER", 12: "BAD_HASH",
           13: "BAD_SIGNATURE", 14: "WRONG_SLOT", 15: "ROLLBACK_BLOCKED",
           16: "NOT_CONFIRMED", 17: "FLASH_ERROR", 18: "CRYPTO_ERROR"}
RESET_FLAGS = [(1 << 25, "OBL"), (1 << 26, "PIN"), (1 << 27, "BOR"), (1 << 28, "SW"),
               (1 << 29, "IWDG"), (1 << 30, "WWDG"), (1 << 31, "LPWR")]


def header(msg_type, node):
    return bytes([PROTO_VERSION, msg_type, node, 0])


def status_req(node=NODE_BROADCAST):
    return header(MSG_STATUS_REQ, node)


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
        self.ser.close()

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
        self.ser.reset_input_buffer()
        self.rx.clear()
        self.send(status_req(node))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            msg = self.receive(deadline - time.monotonic())
            if msg is not None:
                s = decode_status(msg)
                if s is not None:
                    return s
        return None
