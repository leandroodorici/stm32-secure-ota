#ifndef FRAME_H
#define FRAME_H

#include <stddef.h>
#include <stdint.h>

// Message framing for byte-stream transports such as the UART
//
// A byte stream has no message boundaries and no error detection, so each
// message is sent as a frame:
//
//     COBS( payload || CRC-32(payload) )  0x00
//
// COBS (consistent overhead byte stuffing) rewrites the bytes so that 0x00
// never appears inside the frame, at a cost of one byte per 254. The 0x00 that
// follows therefore always marks the end of a frame: a receiver that starts
// mid-stream, or loses bytes, resynchronises at the next one. The CRC-32 is
// the one used for boot records, stored little-endian, and catches corruption
// that leaves the framing intact.
//
// Framing belongs to the transport. The protocol above it sees only whole,
// checked messages, which is what lets CAN with ISO-TP replace this layer in .M4

// Large enough for a 1 KB data chunk plus its message header
#define FRAME_MAX_PAYLOAD 1088u
#define FRAME_CRC_LEN 4u

// Worst case on the wire: payload and CRC, one COBS code byte per 254 bytes
// plus one, and the delimiter
#define FRAME_MAX_ENCODED                                                                \
    (FRAME_MAX_PAYLOAD + FRAME_CRC_LEN + (FRAME_MAX_PAYLOAD + FRAME_CRC_LEN) / 254u + 2u)

// Encodes one frame into out, delimiter included. Returns the number of bytes
// written, or 0 if len is 0, len exceeds FRAME_MAX_PAYLOAD, or out_size is too
// small.
size_t frame_encode(const uint8_t *payload, size_t len, uint8_t *out, size_t out_size);

// Receiver state. Bytes are collected until a delimiter, then decoded in place.
typedef struct {
    uint8_t buf[FRAME_MAX_ENCODED];
    size_t len;
    uint32_t overflow; // the current frame is too long and will be dropped
    uint32_t dropped;  // frames discarded since init: bad COBS, CRC or length
} frame_rx_t;

void frame_rx_init(frame_rx_t *rx);

// Feeds one received byte. When it completes a valid frame, copies the payload
// to out and returns its length. Otherwise returns 0: either the frame is not
// complete yet, or it was invalid and has been discarded and counted.
size_t frame_rx_push(frame_rx_t *rx, uint8_t byte, uint8_t *out, size_t out_size);

#endif // FRAME_H
