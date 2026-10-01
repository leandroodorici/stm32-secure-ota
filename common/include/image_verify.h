#ifndef IMAGE_VERIFY_H
#define IMAGE_VERIFY_H

#include <stdint.h>

#include "boot_record.h"
#include "flash_if.h"

// Verification of a signed image in place in a slot

#define IMG_MAGIC 0x31474D49u // "IMG1"
#define IMG_HDR_SIZE 0x200u

// The trailer is three TLVs: a 4-byte tag/length, then the value
#define IMG_TLV_SHA256 0x0010u
#define IMG_TLV_KEY_ID 0x0020u
#define IMG_TLV_ECDSA_P256 0x0022u
#define IMG_HASH_LEN 32u
#define IMG_KEY_ID_LEN 8u
#define IMG_SIG_LEN 64u
#define IMG_TRAILER_LEN ((4u + IMG_HASH_LEN) + (4u + IMG_KEY_ID_LEN) + (4u + IMG_SIG_LEN))

// Uncompressed P-256 point
#define IMG_PUBKEY_LEN 65u

// The header fields the bootloader acts on. Parsed from the first 32 bytes
// of the 512-byte header, little-endian, so the layout is independent of
// compiler padding.
typedef struct {
    uint32_t magic;
    uint16_t header_version;
    uint16_t header_size;
    uint32_t load_addr;
    uint32_t payload_size;
    uint32_t version;
    uint32_t build_number;
    uint32_t security_counter;
    uint32_t flags;
} image_header_t;

// Verifies the image in the slot at slot_base. Returns BR_RESULT_OK only if
// every check passes, in this order:
//
//   1. bounds: magic, header size, load_addr == slot_base, payload and
//      trailer inside the slot, well-formed trailer
//   2. SHA-256 over header and payload against the hash TLV
//   3. key ID, then the ECDSA P-256 signature, against pubkey
//   4. security_counter against security_floor
br_result_t image_verify(const flash_if_t *flash, uint32_t slot_base, uint32_t slot_size,
                         const uint8_t pubkey[IMG_PUBKEY_LEN], uint32_t security_floor,
                         image_header_t *out);

#endif // IMAGE_VERIFY_H