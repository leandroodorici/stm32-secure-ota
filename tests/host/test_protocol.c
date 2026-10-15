#include <string.h>

#include "boot_record.h"
#include "protocol.h"
#include "test_util.h"

// The node encodes STATUS in C and the host tool decodes it in Python. Both
// are checked against the same fixed message, so the two cannot drift apart.

// Slot B on its second trial attempt, image 1.2.3 at counter 5, floor 4,
// last result 16, reset by the watchdog, boot record seq 11
static const uint8_t k_status[PROTO_STATUS_LEN] = {
    0x01, 0x02, 0x01, 0x00, 0x01, 0x03, 0x04, 0x02, 0x03, 0x00, 0x02,
    0x01, 0x05, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x10, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x24, 0x0b, 0x00, 0x00, 0x00,
};

static proto_status_t sample(void) {
    proto_status_t s;
    memset(&s, 0, sizeof(s));
    s.running_slot = 1;
    s.slot_state[0] = 3;
    s.slot_state[1] = 4;
    s.trial_attempts = 2;
    s.image_version = (1u << 24) | (2u << 16) | 3u;
    s.image_counter = 5;
    s.security_floor = 4;
    s.last_result = 16;
    s.reset_flags = 0x24000000u;
    s.boot_seq = 11;
    return s;
}

static void test_status_encodes_to_the_reference_bytes(void) {
    proto_status_t s = sample();
    uint8_t out[PROTO_STATUS_LEN + 4u];
    CHECK_EQ(proto_encode_status(&s, 1, out, sizeof(out)), PROTO_STATUS_LEN);
    CHECK(memcmp(out, k_status, PROTO_STATUS_LEN) == 0);
}

static void test_status_decodes_from_the_reference_bytes(void) {
    proto_status_t s;
    CHECK(proto_decode_status(k_status, sizeof(k_status), &s));
    proto_status_t want = sample();
    CHECK(memcmp(&s, &want, sizeof(s)) == 0);
}

static void test_status_rejects_wrong_length_type_and_version(void) {
    uint8_t m[PROTO_STATUS_LEN];
    proto_status_t s;

    memcpy(m, k_status, sizeof(m));
    CHECK(!proto_decode_status(m, sizeof(m) - 1u, &s));

    m[1] = MSG_STATUS_REQ;
    CHECK(!proto_decode_status(m, sizeof(m), &s));

    memcpy(m, k_status, sizeof(m));
    m[0] = PROTO_VERSION + 1u;
    CHECK(!proto_decode_status(m, sizeof(m), &s));
}

static void test_status_req_is_a_bare_header(void) {
    uint8_t out[PROTO_HDR_LEN];
    CHECK_EQ(proto_encode_status_req(PROTO_NODE_BROADCAST, out, sizeof(out)),
             PROTO_HDR_LEN);

    proto_hdr_t h;
    CHECK(proto_parse_header(out, sizeof(out), &h));
    CHECK_EQ(h.type, MSG_STATUS_REQ);
    CHECK_EQ(h.node, PROTO_NODE_BROADCAST);
    CHECK_EQ(out[3], 0);
}

static void test_encoders_refuse_short_buffers(void) {
    proto_status_t s = sample();
    uint8_t out[PROTO_STATUS_LEN];
    CHECK_EQ(proto_encode_status(&s, 1, out, PROTO_STATUS_LEN - 1u), 0);
    CHECK_EQ(proto_encode_status_req(1, out, PROTO_HDR_LEN - 1u), 0);
}

// ---- the update messages ----
//
// Each direction is checked against reference bytes that tools/test_link.py
// uses too: the node decodes what the host tool encodes, and the other way
// round.

// Slot B offered v0.2.0 at counter 2, 10316 bytes in 1 KB chunks, with a
// digest of 0x00..0x1f
static const uint8_t k_offer[PROTO_OFFER_LEN] = {
    0x01, 0x10, 0x01, 0x00, 0x4c, 0x28, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x02, 0x00,
    0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11,
    0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};

// Eight bytes 0x01..0x08 at offset 1024
static const uint8_t k_chunk[PROTO_CHUNK_HDR_LEN + 8u] = {
    0x01, 0x20, 0x01, 0x00, 0x00, 0x04, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
    0xc5, 0x88, 0xca, 0x3f, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
};

static void test_offer_decodes_from_the_reference_bytes(void) {
    proto_offer_t o;
    CHECK(proto_decode_offer(k_offer, sizeof(k_offer), &o));
    CHECK_EQ(o.image_size, 10316);
    CHECK_EQ(o.version, 0x00020000);
    CHECK_EQ(o.security_counter, 2);
    CHECK_EQ(o.target_slot, 1);
    CHECK_EQ(o.chunk_size, 1024);
    for (uint32_t i = 0; i < sizeof(o.digest); i++) {
        CHECK_EQ(o.digest[i], i);
    }
    CHECK(!proto_decode_offer(k_offer, sizeof(k_offer) - 1u, &o));
}

static void test_chunk_decodes_from_the_reference_bytes(void) {
    proto_chunk_t c;
    CHECK(proto_decode_chunk(k_chunk, sizeof(k_chunk), &c));
    CHECK_EQ(c.offset, 1024);
    CHECK_EQ(c.length, 8);
    CHECK(c.data == k_chunk + PROTO_CHUNK_HDR_LEN);
    // The CRC is the same CRC-32 the frames and boot records use
    CHECK_EQ(c.crc, br_crc32(c.data, c.length));
}

static void test_chunk_length_must_match_the_data(void) {
    proto_chunk_t c;
    // One byte missing, one byte too many
    CHECK(!proto_decode_chunk(k_chunk, sizeof(k_chunk) - 1u, &c));
    static uint8_t m[PROTO_CHUNK_HDR_LEN + PROTO_CHUNK_MAX + 1u];
    memcpy(m, k_chunk, sizeof(k_chunk));
    CHECK(!proto_decode_chunk(m, sizeof(k_chunk) + 1u, &c));

    // A length beyond the maximum, even when the data is all there
    m[8] = (uint8_t)((PROTO_CHUNK_MAX + 1u) & 0xFFu);
    m[9] = (uint8_t)((PROTO_CHUNK_MAX + 1u) >> 8);
    CHECK(!proto_decode_chunk(m, PROTO_CHUNK_HDR_LEN + PROTO_CHUNK_MAX + 1u, &c));
}

static void test_single_value_messages_match_the_reference_bytes(void) {
    static const struct {
        uint8_t type;
        uint32_t value;
        uint8_t bytes[PROTO_U32_LEN];
    } refs[] = {
        {MSG_ACCEPT, 2048, {0x01, 0x11, 0x01, 0x00, 0x00, 0x08, 0x00, 0x00}},
        {MSG_REJECT, 15, {0x01, 0x12, 0x01, 0x00, 0x0f, 0x00, 0x00, 0x00}},
        {MSG_CHUNK_ACK, 3072, {0x01, 0x21, 0x01, 0x00, 0x00, 0x0c, 0x00, 0x00}},
        {MSG_VERIFY_RESULT, 1, {0x01, 0x31, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00}},
        {MSG_ABORT, 38, {0x01, 0x7f, 0x01, 0x00, 0x26, 0x00, 0x00, 0x00}},
        {MSG_REBOOT, 500, {0x01, 0x40, 0x01, 0x00, 0xf4, 0x01, 0x00, 0x00}},
    };

    for (size_t i = 0; i < sizeof(refs) / sizeof(refs[0]); i++) {
        uint8_t out[PROTO_U32_LEN];
        CHECK_EQ(proto_encode_u32(refs[i].type, 1, refs[i].value, out, sizeof(out)),
                 PROTO_U32_LEN);
        CHECK(memcmp(out, refs[i].bytes, PROTO_U32_LEN) == 0);

        uint32_t v = 0;
        CHECK(proto_decode_u32(refs[i].bytes, PROTO_U32_LEN, refs[i].type, &v));
        CHECK_EQ(v, refs[i].value);
        // The same bytes under any other type are not accepted
        CHECK(!proto_decode_u32(refs[i].bytes, PROTO_U32_LEN, MSG_STATUS, &v));
    }
}

TEST_MAIN_BEGIN("protocol")
RUN(test_status_encodes_to_the_reference_bytes);
RUN(test_status_decodes_from_the_reference_bytes);
RUN(test_status_rejects_wrong_length_type_and_version);
RUN(test_status_req_is_a_bare_header);
RUN(test_encoders_refuse_short_buffers);
RUN(test_offer_decodes_from_the_reference_bytes);
RUN(test_chunk_decodes_from_the_reference_bytes);
RUN(test_chunk_length_must_match_the_data);
RUN(test_single_value_messages_match_the_reference_bytes);
TEST_MAIN_END()
