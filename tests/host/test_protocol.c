#include <string.h>

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

TEST_MAIN_BEGIN("protocol")
RUN(test_status_encodes_to_the_reference_bytes);
RUN(test_status_decodes_from_the_reference_bytes);
RUN(test_status_rejects_wrong_length_type_and_version);
RUN(test_status_req_is_a_bare_header);
RUN(test_encoders_refuse_short_buffers);
TEST_MAIN_END()
