#include <string.h>

#include "frame.h"
#include "test_util.h"

// The framing layer is checked three ways: against frames computed
// independently in Python, by round trips across the sizes where COBS
// changes behaviour, and by feeding it the damage a real serial line produces.

static uint8_t g_wire[FRAME_MAX_ENCODED * 3];
static uint8_t g_out[FRAME_MAX_PAYLOAD];

// Feeds bytes to a receiver and returns the length of the last frame it
// completed, or 0 if none
static size_t feed(frame_rx_t *rx, const uint8_t *bytes, size_t len) {
    size_t got = 0;
    for (size_t i = 0; i < len; i++) {
        size_t n = frame_rx_push(rx, bytes[i], g_out, sizeof(g_out));
        if (n > 0u) {
            got = n;
        }
    }
    return got;
}

// A small deterministic generator, so failures are reproducible
static uint32_t g_seed = 12345u;
static uint8_t next_byte(void) {
    g_seed = g_seed * 1103515245u + 12345u;
    return (uint8_t)(g_seed >> 16);
}

// ---- agreement with the Python implementation ----

static void test_known_frames(void) {
    static const struct {
        uint8_t payload[8];
        size_t payload_len;
        uint8_t wire[16];
        size_t wire_len;
    } vectors[] = {
        {{0x01, 0x00, 0x02},
         3,
         {0x02, 0x01, 0x06, 0x02, 0x09, 0xd2, 0x8d, 0x10, 0x00},
         9},
        {{0x00}, 1, {0x01, 0x05, 0x8d, 0xef, 0x02, 0xd2, 0x00}, 7},
        {{0x01, 0x02, 0x03, 0x04, 0x05},
         5,
         {0x0a, 0x01, 0x02, 0x03, 0x04, 0x05, 0xf4, 0x99, 0x0b, 0x47, 0x00},
         11},
    };

    for (size_t v = 0; v < sizeof(vectors) / sizeof(vectors[0]); v++) {
        size_t n = frame_encode(vectors[v].payload, vectors[v].payload_len, g_wire,
                                sizeof(g_wire));
        CHECK_EQ(n, vectors[v].wire_len);
        CHECK(memcmp(g_wire, vectors[v].wire, n) == 0);

        frame_rx_t rx;
        frame_rx_init(&rx);
        CHECK_EQ(feed(&rx, vectors[v].wire, vectors[v].wire_len), vectors[v].payload_len);
        CHECK(memcmp(g_out, vectors[v].payload, vectors[v].payload_len) == 0);
    }
}

// ---- round trips ----

static void roundtrip(const uint8_t *payload, size_t len) {
    size_t n = frame_encode(payload, len, g_wire, sizeof(g_wire));
    CHECK(n > 0u);
    CHECK(n <= FRAME_MAX_ENCODED);
    // The delimiter is the only zero on the wire
    for (size_t i = 0; i + 1u < n; i++) {
        CHECK(g_wire[i] != 0u);
    }
    CHECK_EQ(g_wire[n - 1u], 0);

    frame_rx_t rx;
    frame_rx_init(&rx);
    CHECK_EQ(feed(&rx, g_wire, n), len);
    CHECK(memcmp(g_out, payload, len) == 0);
    CHECK_EQ(rx.dropped, 0);
}

static void test_roundtrip_around_block_boundaries(void) {
    // A COBS block holds at most 254 bytes, and the CRC adds 4 to the
    // payload, so the interesting sizes cluster around multiples of 254
    static const size_t sizes[] = {1,   2,   249, 250, 251, 252, 253,  254,
                                   255, 503, 504, 505, 508, 509, 1087, 1088};
    static uint8_t p[FRAME_MAX_PAYLOAD];

    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        memset(p, 0x00, sizes[s]);
        roundtrip(p, sizes[s]);
        memset(p, 0xFF, sizes[s]);
        roundtrip(p, sizes[s]);
        for (size_t i = 0; i < sizes[s]; i++) {
            p[i] = next_byte();
        }
        roundtrip(p, sizes[s]);
    }
}

static void test_encode_rejects_bad_sizes(void) {
    static uint8_t p[FRAME_MAX_PAYLOAD + 1u];
    CHECK_EQ(frame_encode(p, 0, g_wire, sizeof(g_wire)), 0);
    CHECK_EQ(frame_encode(p, FRAME_MAX_PAYLOAD + 1u, g_wire, sizeof(g_wire)), 0);
    // An output buffer one byte short of the worst case for this payload
    memset(p, 0x11, 10);
    size_t need = frame_encode(p, 10, g_wire, sizeof(g_wire));
    CHECK_EQ(frame_encode(p, 10, g_wire, need - 1u), 0);
}

// ---- damage on the line ----

static void test_corrupted_byte_is_dropped(void) {
    const uint8_t p[] = {0x10, 0x20, 0x30, 0x40};
    size_t n = frame_encode(p, sizeof(p), g_wire, sizeof(g_wire));
    g_wire[2] ^= 0x01u;

    frame_rx_t rx;
    frame_rx_init(&rx);
    CHECK_EQ(feed(&rx, g_wire, n), 0);
    CHECK_EQ(rx.dropped, 1);
}

static void test_resynchronises_after_a_partial_frame(void) {
    // The receiver starts listening halfway through a frame, then a whole one
    // follows. The fragment is dropped and the next frame still arrives.
    const uint8_t p[] = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE};
    size_t n = frame_encode(p, sizeof(p), g_wire, sizeof(g_wire));
    memcpy(g_wire + n, g_wire, n);

    frame_rx_t rx;
    frame_rx_init(&rx);
    CHECK_EQ(feed(&rx, g_wire + 3, n - 3u), 0);
    CHECK_EQ(rx.dropped, 1);
    CHECK_EQ(feed(&rx, g_wire + n, n), sizeof(p));
    CHECK(memcmp(g_out, p, sizeof(p)) == 0);
}

static void test_overlong_frame_is_dropped(void) {
    // Noise with no delimiter must not overrun the buffer, and the receiver
    // recovers on the next frame
    frame_rx_t rx;
    frame_rx_init(&rx);
    for (size_t i = 0; i < FRAME_MAX_ENCODED + 100u; i++) {
        (void)frame_rx_push(&rx, 0x55u, g_out, sizeof(g_out));
    }
    CHECK_EQ(frame_rx_push(&rx, 0x00u, g_out, sizeof(g_out)), 0);
    CHECK_EQ(rx.dropped, 1);

    const uint8_t p[] = {0x01, 0x02};
    size_t n = frame_encode(p, sizeof(p), g_wire, sizeof(g_wire));
    CHECK_EQ(feed(&rx, g_wire, n), sizeof(p));
}

static void test_repeated_delimiters_are_ignored(void) {
    const uint8_t idle[] = {0x00, 0x00, 0x00};
    frame_rx_t rx;
    frame_rx_init(&rx);
    CHECK_EQ(feed(&rx, idle, sizeof(idle)), 0);
    CHECK_EQ(rx.dropped, 0);
}

static void test_payload_larger_than_caller_buffer_is_dropped(void) {
    static uint8_t p[100];
    memset(p, 0x42, sizeof(p));
    size_t n = frame_encode(p, sizeof(p), g_wire, sizeof(g_wire));

    frame_rx_t rx;
    frame_rx_init(&rx);
    size_t got = 0;
    uint8_t small[50];
    for (size_t i = 0; i < n; i++) {
        got = frame_rx_push(&rx, g_wire[i], small, sizeof(small));
    }
    CHECK_EQ(got, 0);
    CHECK_EQ(rx.dropped, 1);
}

TEST_MAIN_BEGIN("frame")
RUN(test_known_frames);
RUN(test_roundtrip_around_block_boundaries);
RUN(test_encode_rejects_bad_sizes);
RUN(test_corrupted_byte_is_dropped);
RUN(test_resynchronises_after_a_partial_frame);
RUN(test_overlong_frame_is_dropped);
RUN(test_repeated_delimiters_are_ignored);
RUN(test_payload_larger_than_caller_buffer_is_dropped);
TEST_MAIN_END()
