#include "protocol.h"

#include <string.h>
// See protocol.h

static void put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static void put_header(uint8_t *out, uint8_t type, uint8_t node) {
    out[0] = PROTO_VERSION;
    out[1] = type;
    out[2] = node;
    out[3] = 0u;
}

uint32_t proto_parse_header(const uint8_t *msg, size_t len, proto_hdr_t *out) {
    if (len < PROTO_HDR_LEN || msg[0] != PROTO_VERSION) {
        return 0;
    }
    out->version = msg[0];
    out->type = msg[1];
    out->node = msg[2];
    return 1;
}

size_t proto_encode_status_req(uint8_t node, uint8_t *out, size_t out_size) {
    if (out_size < PROTO_HDR_LEN) {
        return 0;
    }
    put_header(out, MSG_STATUS_REQ, node);
    return PROTO_HDR_LEN;
}

size_t proto_encode_status(const proto_status_t *s, uint8_t node, uint8_t *out,
                           size_t out_size) {
    if (out_size < PROTO_STATUS_LEN) {
        return 0;
    }
    put_header(out, MSG_STATUS, node);
    out[4] = s->running_slot;
    out[5] = s->slot_state[0];
    out[6] = s->slot_state[1];
    out[7] = s->trial_attempts;
    put_u32(out + 8, s->image_version);
    put_u32(out + 12, s->image_counter);
    put_u32(out + 16, s->security_floor);
    put_u32(out + 20, s->last_result);
    put_u32(out + 24, s->reset_flags);
    put_u32(out + 28, s->boot_seq);
    return PROTO_STATUS_LEN;
}

uint32_t proto_decode_offer(const uint8_t *msg, size_t len, proto_offer_t *out) {
    proto_hdr_t h;
    if (!proto_parse_header(msg, len, &h) || h.type != MSG_OFFER ||
        len != PROTO_OFFER_LEN) {
        return 0;
    }
    out->image_size = get_u32(msg + 4);
    out->version = get_u32(msg + 8);
    out->security_counter = get_u32(msg + 12);
    out->target_slot = msg[16];
    out->chunk_size = get_u16(msg + 20);
    memcpy(out->digest, msg + 24, sizeof(out->digest));
    return 1;
}

uint32_t proto_decode_chunk(const uint8_t *msg, size_t len, proto_chunk_t *out) {
    proto_hdr_t h;
    if (!proto_parse_header(msg, len, &h) || h.type != MSG_CHUNK ||
        len < PROTO_CHUNK_HDR_LEN) {
        return 0;
    }
    uint16_t length = get_u16(msg + 8);
    if (length > PROTO_CHUNK_MAX || len != PROTO_CHUNK_HDR_LEN + length) {
        return 0;
    }
    out->offset = get_u32(msg + 4);
    out->length = length;
    out->crc = get_u32(msg + 12);
    out->data = msg + PROTO_CHUNK_HDR_LEN;
    return 1;
}

size_t proto_encode_u32(uint8_t type, uint8_t node, uint32_t value, uint8_t *out,
                        size_t out_size) {
    if (out_size < PROTO_U32_LEN) {
        return 0;
    }
    put_header(out, type, node);
    put_u32(out + 4, value);
    return PROTO_U32_LEN;
}

uint32_t proto_decode_u32(const uint8_t *msg, size_t len, uint8_t type, uint32_t *value) {
    proto_hdr_t h;
    if (!proto_parse_header(msg, len, &h) || h.type != type || len != PROTO_U32_LEN) {
        return 0;
    }
    *value = get_u32(msg + 4);
    return 1;
}

uint32_t proto_decode_status(const uint8_t *msg, size_t len, proto_status_t *out) {
    proto_hdr_t h;
    if (!proto_parse_header(msg, len, &h) || h.type != MSG_STATUS ||
        len != PROTO_STATUS_LEN) {
        return 0;
    }
    out->running_slot = msg[4];
    out->slot_state[0] = msg[5];
    out->slot_state[1] = msg[6];
    out->trial_attempts = msg[7];
    out->image_version = get_u32(msg + 8);
    out->image_counter = get_u32(msg + 12);
    out->security_floor = get_u32(msg + 16);
    out->last_result = get_u32(msg + 20);
    out->reset_flags = get_u32(msg + 24);
    out->boot_seq = get_u32(msg + 28);
    return 1;
}
