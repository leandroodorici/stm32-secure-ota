#include "image_verify.h"

#include <string.h>

#include "mbedtls/ecdsa.h"
#include "mbedtls/memory_buffer_alloc.h"
#include "mbedtls/sha256.h"

// Mbed TLS allocates its bignums from this buffer rather than a heap, which
// the bootloader does not have. Peak use measured on the G474 is 4948 bytes.
// A 64-bit host needs more for the same computation, since pointers and allocator
// headers double in size, so the host build overrides this.
#ifndef IMAGE_VERIFY_HEAP_SIZE
#define IMAGE_VERIFY_HEAP_SIZE 6144u
#endif

static unsigned char g_heap[IMAGE_VERIFY_HEAP_SIZE];

// Hashing reads the slot through the flash interface in pieces of this size,
// so a damaged double-word is reported as a read error rather than faulting.
#define HASH_CHUNK 256u

static uint16_t get_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void parse_header(const uint8_t raw[32], image_header_t *h) {
    h->magic = get_u32(raw + 0);
    h->header_version = get_u16(raw + 4);
    h->header_size = get_u16(raw + 6);
    h->load_addr = get_u32(raw + 8);
    h->payload_size = get_u32(raw + 12);
    h->version = get_u32(raw + 16);
    h->build_number = get_u32(raw + 20);
    h->security_counter = get_u32(raw + 24);
    h->flags = get_u32(raw + 28);
}

typedef struct {
    const uint8_t *hash;
    const uint8_t *key_id;
    const uint8_t *sig;
} trailer_t;

// Walks the TLVs inside the fixed-size trailer. A length that runs past the
// end is malformed. Unknown tags are skipped, and the three required tags
// must each be present with exactly their expected length.
static br_result_t parse_trailer(const uint8_t buf[IMG_TRAILER_LEN], trailer_t *t) {
    uint32_t hash_len = 0;
    uint32_t key_id_len = 0;
    uint32_t sig_len = 0;
    uint32_t off = 0;

    memset(t, 0, sizeof(*t));
    while (off + 4u <= IMG_TRAILER_LEN) {
        uint16_t tag = get_u16(buf + off);
        uint16_t len = get_u16(buf + off + 2u);
        off += 4u;
        if (len > IMG_TRAILER_LEN - off) {
            return BR_RESULT_BAD_HEADER;
        }
        if (tag == IMG_TLV_SHA256) {
            t->hash = buf + off;
            hash_len = len;
        } else if (tag == IMG_TLV_KEY_ID) {
            t->key_id = buf + off;
            key_id_len = len;
        } else if (tag == IMG_TLV_ECDSA_P256) {
            t->sig = buf + off;
            sig_len = len;
        }
        off += len;
    }

    if (t->hash == NULL || hash_len != IMG_HASH_LEN || t->key_id == NULL ||
        key_id_len != IMG_KEY_ID_LEN || t->sig == NULL || sig_len != IMG_SIG_LEN) {
        return BR_RESULT_BAD_HEADER;
    }
    return BR_RESULT_OK;
}

// SHA-256 over len bytes of flash starting at addr, read in chunks
static br_result_t hash_flash(const flash_if_t *flash, uint32_t addr, uint32_t len,
                              uint8_t digest[IMG_HASH_LEN]) {
    mbedtls_sha256_context ctx;
    uint8_t chunk[HASH_CHUNK];
    br_result_t result = BR_RESULT_OK;

    mbedtls_sha256_init(&ctx);
    if (mbedtls_sha256_starts(&ctx, 0) != 0) {
        result = BR_RESULT_CRYPTO_ERROR;
    }
    while (result == BR_RESULT_OK && len > 0u) {
        uint32_t n = (len < HASH_CHUNK) ? len : HASH_CHUNK;
        if (flash_read(flash, addr, chunk, n) != FLASH_OK) {
            result = BR_RESULT_FLASH_ERROR;
        } else if (mbedtls_sha256_update(&ctx, chunk, n) != 0) {
            result = BR_RESULT_CRYPTO_ERROR;
        }
        addr += n;
        len -= n;
    }
    if (result == BR_RESULT_OK && mbedtls_sha256_finish(&ctx, digest) != 0) {
        result = BR_RESULT_CRYPTO_ERROR;
    }
    mbedtls_sha256_free(&ctx);
    return result;
}

// The key ID is the first eight bytes of SHA-256 over the uncompressed public
// key point.
static br_result_t check_key_id(const uint8_t pubkey[IMG_PUBKEY_LEN],
                                const uint8_t key_id[IMG_KEY_ID_LEN]) {
    uint8_t digest[32];
    if (mbedtls_sha256(pubkey, IMG_PUBKEY_LEN, digest, 0) != 0) {
        return BR_RESULT_CRYPTO_ERROR;
    }
    return (memcmp(digest, key_id, IMG_KEY_ID_LEN) == 0) ? BR_RESULT_OK
                                                         : BR_RESULT_BAD_SIGNATURE;
}

// A signature that does not verify is BAD_SIGNATURE. Any other failure, such
// as an allocation failure or a public key that is not a valid curve point,
// is a bug in this device's own build, and gets its own code so it is never
// mistaken for an attack.
static br_result_t check_signature(const uint8_t pubkey[IMG_PUBKEY_LEN],
                                   const uint8_t hash[IMG_HASH_LEN],
                                   const uint8_t sig[IMG_SIG_LEN]) {
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r;
    mbedtls_mpi s;

    mbedtls_memory_buffer_alloc_init(g_heap, sizeof(g_heap));
    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    int ret = mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1);
    if (ret == 0) {
        ret = mbedtls_ecp_point_read_binary(&grp, &q, pubkey, IMG_PUBKEY_LEN);
    }
    if (ret == 0) {
        ret = mbedtls_mpi_read_binary(&r, sig, IMG_SIG_LEN / 2u);
    }
    if (ret == 0) {
        ret = mbedtls_mpi_read_binary(&s, sig + IMG_SIG_LEN / 2u, IMG_SIG_LEN / 2u);
    }

    br_result_t result = BR_RESULT_CRYPTO_ERROR;
    if (ret == 0) {
        ret = mbedtls_ecdsa_verify(&grp, hash, IMG_HASH_LEN, &q, &r, &s);
        if (ret == 0) {
            result = BR_RESULT_OK;
        } else if (ret == MBEDTLS_ERR_ECP_VERIFY_FAILED) {
            result = BR_RESULT_BAD_SIGNATURE;
        }
    }

    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);
    mbedtls_memory_buffer_alloc_free();
    return result;
}

br_result_t image_verify(const flash_if_t *flash, uint32_t slot_base, uint32_t slot_size,
                         const uint8_t pubkey[IMG_PUBKEY_LEN], uint32_t security_floor,
                         image_header_t *out) {
    // ---- 1. bounds ----
    //
    // Nothing here is authenticated yet. Header fields may only reject the
    // image, never steer anything else.
    uint8_t raw[32];
    if (flash_read(flash, slot_base, raw, sizeof(raw)) != FLASH_OK) {
        return BR_RESULT_FLASH_ERROR;
    }
    image_header_t hdr;
    parse_header(raw, &hdr);

    if (hdr.magic != IMG_MAGIC) {
        return BR_RESULT_BAD_MAGIC;
    }
    if (hdr.header_size != IMG_HDR_SIZE) {
        return BR_RESULT_BAD_HEADER;
    }
    // An image linked for the other slot would run at the wrong addresses.
    // Checked before anything expensive, since it needs no cryptography.
    if (hdr.load_addr != slot_base) {
        return BR_RESULT_WRONG_SLOT;
    }
    // Written as a subtraction from the slot size so that a huge payload_size
    // cannot overflow the comparison.
    if (slot_size < IMG_HDR_SIZE + IMG_TRAILER_LEN ||
        hdr.payload_size > slot_size - IMG_HDR_SIZE - IMG_TRAILER_LEN) {
        return BR_RESULT_BAD_HEADER;
    }
    uint32_t signed_len = IMG_HDR_SIZE + hdr.payload_size;

    uint8_t trailer_buf[IMG_TRAILER_LEN];
    if (flash_read(flash, slot_base + signed_len, trailer_buf, sizeof(trailer_buf)) !=
        FLASH_OK) {
        return BR_RESULT_FLASH_ERROR;
    }
    trailer_t trailer;
    br_result_t result = parse_trailer(trailer_buf, &trailer);
    if (result != BR_RESULT_OK) {
        return result;
    }

    // ---- 2. integrity ----
    //
    // Separate from the signature so the two failures are reported apart: a
    // hash mismatch means corruption, a signature failure with a matching
    // hash means deliberate modification.
    uint8_t digest[IMG_HASH_LEN];
    result = hash_flash(flash, slot_base, signed_len, digest);
    if (result != BR_RESULT_OK) {
        return result;
    }
    if (memcmp(digest, trailer.hash, IMG_HASH_LEN) != 0) {
        return BR_RESULT_BAD_HASH;
    }

    // ---- 3. authenticity ----
    result = check_key_id(pubkey, trailer.key_id);
    if (result != BR_RESULT_OK) {
        return result;
    }
    result = check_signature(pubkey, digest, trailer.sig);
    if (result != BR_RESULT_OK) {
        return result;
    }

    // ---- 4. policy ----
    //
    // The header is authenticated from here on, so its fields may now
    // influence decisions.
    if (hdr.security_counter < security_floor) {
        return BR_RESULT_ROLLBACK_BLOCKED;
    }

    if (out != NULL) {
        *out = hdr;
    }
    return BR_RESULT_OK;
}
