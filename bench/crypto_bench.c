// Measures what signature verification costs on the G474, before
// any of it goes into the bootloader. Answers how much flash the
// Mbed TLS verify path needs, and how long verification takes at
// the reset clock.
//
// Flashed in place of the bootloader, at 0x08000000, so it runs from reset at
// 16 MHz exactly as the bootloader will. Results land in the first 32 bytes
// of SRAM, which the linker script keeps free, and are read over SWD with:
// mdw 0x20000000 8

#include <stdint.h>

#include "mbedtls/ecdsa.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/sha256.h"

// Cortex-M4 cycle counter (DWT). Counts core clock cycles, so at 16 MHz one
// count is 62.5 ns.
#define DEMCR (*(volatile uint32_t *)0xE000EDFCu)
#define DEMCR_TRCENA (1u << 24)
#define DWT_CTRL (*(volatile uint32_t *)0xE0001000u)
#define DWT_CTRL_CYCCNTENA (1u << 0)
#define DWT_CYCCNT (*(volatile uint32_t *)0xE0001004u)

// Worst case for hashing: a full slot, read straight from flash
#define SLOT_A_BASE 0x08010000u
#define SLOT_SIZE (192u * 1024u)

#define BENCH_MAGIC 0x48434E42u // "BNCH"
#define HEAP_SIZE 16384u

// Room for the allocator's first block header, which the paint must not touch
#define HEAP_HEADER_ROOM 64u
#define HEAP_PAINT 0xA5u

typedef struct {
    uint32_t magic;
    uint32_t sha_cycles;    // SHA-256 over SLOT_SIZE bytes
    uint32_t verify_cycles; // one ECDSA P-256 verify, curve load included
    int32_t good_result;    // must be 0: the vector verifies
    int32_t bad_result;     // must be non-zero: one flipped signature bit
    uint32_t heap_peak;     // bytes of HEAP_SIZE actually used
    uint32_t heap_size;
    uint32_t done;
} bench_results_t;

#define RESULTS ((volatile bench_results_t *)0x20000000u)

static unsigned char heap[HEAP_SIZE];

// Throwaway test vector, generated once for this bench. It is unrelated to
// the development signing key.
static const uint8_t k_pubkey[65] = {
    0x04, 0x49, 0x44, 0x03, 0x70, 0x09, 0xaf, 0xfe, 0xa3, 0x6d, 0x47, 0xa7, 0x92,
    0xe6, 0x64, 0xfe, 0x71, 0x7c, 0x4b, 0xdc, 0xb0, 0xee, 0x91, 0x2d, 0xa9, 0xba,
    0x6a, 0x5b, 0x89, 0xd0, 0x7d, 0xd6, 0x5a, 0x35, 0xec, 0x53, 0xe9, 0xc0, 0xd5,
    0xfa, 0x91, 0x6d, 0xd1, 0x84, 0x63, 0xda, 0xbe, 0xd2, 0x5f, 0x2e, 0xf9, 0xf6,
    0x6c, 0x49, 0x9d, 0x2e, 0x5b, 0x18, 0x39, 0xd5, 0xb1, 0x80, 0x02, 0xe7, 0x04,
};

static const uint8_t k_hash[32] = {
    0xb9, 0x05, 0x81, 0xc8, 0x2e, 0xf8, 0xb6, 0x32, 0x5c, 0x18, 0xed,
    0x06, 0xf3, 0xc8, 0x36, 0xac, 0x80, 0xbf, 0x3c, 0xb5, 0x9d, 0xfd,
    0x4c, 0x58, 0x21, 0x4b, 0x37, 0xf1, 0xc3, 0xed, 0x76, 0x68,
};

static const uint8_t k_sig[64] = {
    0xb5, 0x38, 0x47, 0xf4, 0x05, 0x7d, 0x6b, 0x49, 0xbb, 0xa9, 0xa1, 0x69, 0x40,
    0x4a, 0x72, 0x10, 0xd1, 0x31, 0x51, 0xed, 0x68, 0x7a, 0xf9, 0x33, 0x56, 0xad,
    0xd9, 0x2f, 0xd2, 0x15, 0xc2, 0x88, 0xa4, 0x4a, 0x99, 0x22, 0x54, 0xc8, 0x97,
    0x9c, 0x64, 0x41, 0x95, 0x2d, 0xe9, 0x93, 0x7e, 0x48, 0xb8, 0x27, 0x4b, 0x90,
    0x10, 0x6f, 0x8b, 0x9e, 0x72, 0x22, 0x42, 0xd2, 0x16, 0xa2, 0x06, 0x1e,
};

static int verify(const uint8_t sig[64]) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r;
    mbedtls_mpi s;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret == 0) {
        ret = mbedtls_ecp_point_read_binary(&grp, &q, k_pubkey, sizeof(k_pubkey));
    }
    if (ret == 0) {
        ret = mbedtls_mpi_read_binary(&r, sig, 32);
    }
    if (ret == 0) {
        ret = mbedtls_mpi_read_binary(&s, sig + 32, 32);
    }
    if (ret == 0) {
        ret = mbedtls_ecdsa_verify(&grp, k_hash, sizeof(k_hash), &q, &r, &s);
    }

    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    return ret;
}

int main(void) {
    RESULTS->magic = BENCH_MAGIC;
    RESULTS->done = 0;
    RESULTS->heap_size = HEAP_SIZE;

    DEMCR |= DEMCR_TRCENA;
    DWT_CYCCNT = 0;
    DWT_CTRL |= DWT_CTRL_CYCCNTENA;

    mbedtls_memory_buffer_alloc_init(heap, sizeof(heap));

    // Paint the buffer past the first block header. Every allocation is
    // zeroed, so the highest byte no longer holding the pattern marks peak
    // use. The allocator only reads headers it wrote itself, so it never sees
    // the paint.
    for (uint32_t i = HEAP_HEADER_ROOM; i < HEAP_SIZE; i++) {
        heap[i] = HEAP_PAINT;
    }

    unsigned char digest[32];
    uint32_t t0 = DWT_CYCCNT;
    (void)mbedtls_sha256((const unsigned char *)SLOT_A_BASE, SLOT_SIZE, digest, 0);
    RESULTS->sha_cycles = DWT_CYCCNT - t0;

    t0 = DWT_CYCCNT;
    RESULTS->good_result = verify(k_sig);
    RESULTS->verify_cycles = DWT_CYCCNT - t0;

    uint8_t bad[64];
    for (unsigned i = 0; i < sizeof(bad); i++) {
        bad[i] = k_sig[i];
    }
    bad[63] ^= 0x01u;
    RESULTS->bad_result = verify(bad);

    uint32_t peak = HEAP_SIZE;
    while (peak > HEAP_HEADER_ROOM && heap[peak - 1u] == HEAP_PAINT) {
        peak--;
    }
    RESULTS->heap_peak = peak;
    RESULTS->done = 1;

    for (;;) {
    }
}