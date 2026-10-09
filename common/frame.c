#include "frame.h"

#include <string.h>

#include "boot_record.h"

// See frame.h.

// ---- COBS encoding ----
//
// The output is a sequence of blocks. Each starts with a code byte n, followed
// by n - 1 data bytes, and stands for those bytes plus a 0x00 that is not
// transmitted. A code of 0xFF means 254 data bytes and no implied zero. The
// encoder therefore reserves a code byte, copies data until it meets a zero or
// fills a block, then goes back and fills the code in.

typedef struct {
    uint8_t *out;
    size_t size;
    size_t code_at; // where the current block's code byte goes
    size_t len;     // bytes written so far
    uint8_t code;
    uint32_t full; // ran out of space
} cobs_enc_t;

static void enc_start(cobs_enc_t *e, uint8_t *out, size_t size) {
    e->out = out;
    e->size = size;
    e->code_at = 0;
    e->len = 1;
    e->code = 1;
    e->full = (size == 0u);
}

static void enc_close_block(cobs_enc_t *e) {
    e->out[e->code_at] = e->code;
    e->code_at = e->len;
    e->code = 1;
    if (e->len >= e->size) {
        e->full = 1;
    } else {
        e->len++;
    }
}

static void enc_byte(cobs_enc_t *e, uint8_t b) {
    if (e->full) {
        return;
    }
    if (b == 0u) {
        enc_close_block(e);
        return;
    }
    if (e->len >= e->size) {
        e->full = 1;
        return;
    }
    e->out[e->len++] = b;
    e->code++;
    if (e->code == 0xFFu) {
        enc_close_block(e);
    }
}

size_t frame_encode(const uint8_t *payload, size_t len, uint8_t *out, size_t out_size) {
    if (len == 0u || len > FRAME_MAX_PAYLOAD) {
        return 0;
    }

    uint32_t crc = br_crc32(payload, len);
    cobs_enc_t e;
    enc_start(&e, out, out_size);
    for (size_t i = 0; i < len; i++) {
        enc_byte(&e, payload[i]);
    }
    for (uint32_t i = 0; i < FRAME_CRC_LEN; i++) {
        enc_byte(&e, (uint8_t)(crc >> (8u * i)));
    }
    if (e.full) {
        return 0;
    }
    e.out[e.code_at] = e.code;

    // The delimiter
    if (e.len >= e.size) {
        return 0;
    }
    e.out[e.len++] = 0u;
    return e.len;
}

// ---- COBS decoding ----
//
// Decodes in place: every block shrinks by its code byte and then gains at
// most the one implied zero, so the write position never passes the read
// position. Returns the decoded length, or 0 for malformed input: a zero code
// byte, or a block that runs past the end.
static size_t cobs_decode_in_place(uint8_t *buf, size_t len) {
    size_t in = 0;
    size_t out = 0;
    while (in < len) {
        uint8_t code = buf[in++];
        if (code == 0u) {
            return 0;
        }
        size_t n = (size_t)code - 1u;
        if (n > len - in) {
            return 0;
        }
        memmove(buf + out, buf + in, n);
        in += n;
        out += n;
        // The implied zero, except after a full block and at the very end
        if (code != 0xFFu && in < len) {
            buf[out++] = 0u;
        }
    }
    return out;
}

// ---- receiving ----

void frame_rx_init(frame_rx_t *rx) {
    rx->len = 0;
    rx->overflow = 0;
    rx->dropped = 0;
}

static size_t finish_frame(frame_rx_t *rx, uint8_t *out, size_t out_size) {
    size_t n = cobs_decode_in_place(rx->buf, rx->len);
    if (n <= FRAME_CRC_LEN) {
        return 0; // malformed, or no payload
    }
    size_t payload_len = n - FRAME_CRC_LEN;
    if (payload_len > FRAME_MAX_PAYLOAD || payload_len > out_size) {
        return 0;
    }

    const uint8_t *c = rx->buf + payload_len;
    uint32_t crc = (uint32_t)c[0] | ((uint32_t)c[1] << 8) | ((uint32_t)c[2] << 16) |
                   ((uint32_t)c[3] << 24);
    if (crc != br_crc32(rx->buf, payload_len)) {
        return 0;
    }

    memcpy(out, rx->buf, payload_len);
    return payload_len;
}

size_t frame_rx_push(frame_rx_t *rx, uint8_t byte, uint8_t *out, size_t out_size) {
    if (byte != 0u) {
        if (rx->len < sizeof(rx->buf)) {
            rx->buf[rx->len++] = byte;
        } else {
            rx->overflow = 1;
        }
        return 0;
    }

    // A delimiter. Back-to-back delimiters, as a sender may emit to flush a
    // receiver, end an empty frame and are not counted as errors.
    if (rx->len == 0u && !rx->overflow) {
        return 0;
    }

    size_t n = rx->overflow ? 0u : finish_frame(rx, out, out_size);
    if (n == 0u) {
        rx->dropped++;
    }
    rx->len = 0;
    rx->overflow = 0;
    return n;
}
