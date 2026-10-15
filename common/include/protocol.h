#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

// The update protocol between gateway and node. Transport-independent: each
// message arrives whole, already checked by the transport's framing.
//
// Every message starts with a 4-byte header, little-endian throughout:
//
//   0  protocol version (PROTO_VERSION)
//   1  message type
//   2  node identifier: the node addressed, or the node replying
//   3  reserved, sent as 0
//
// Every message in section 9 is defined here. Each side implements only what
// it needs: the node decodes what the gateway sends and encodes its replies,
// and tools/link.py does the reverse.

#define PROTO_VERSION 1u
#define PROTO_HDR_LEN 4u

// A request addressed to every node
#define PROTO_NODE_BROADCAST 0xFFu

typedef enum {
    MSG_STATUS_REQ = 0x01u,    // gateway -> node
    MSG_STATUS = 0x02u,        // node -> gateway
    MSG_OFFER = 0x10u,         // gateway -> node
    MSG_ACCEPT = 0x11u,        // node -> gateway
    MSG_REJECT = 0x12u,        // node -> gateway
    MSG_CHUNK = 0x20u,         // gateway -> node
    MSG_CHUNK_ACK = 0x21u,     // node -> gateway
    MSG_COMMIT = 0x30u,        // gateway -> node
    MSG_VERIFY_RESULT = 0x31u, // node -> gateway
    MSG_REBOOT = 0x40u,        // gateway -> node
    MSG_ABORT = 0x7Fu,         // either direction
} msg_type_t;

// Why a node refuses an OFFER or abandons an update, carried by REJECT and ABORT
typedef enum {
    UPD_ERR_BUSY_TRIAL = 32u, // the running image is on trial: the other slot
                              // is its only fallback and must not be touched
    UPD_ERR_TOO_LARGE = 33u,  // the image does not fit in a slot
    UPD_ERR_CHUNK_SIZE = 34u, // chunk size zero, too large, or not a multiple
                              // of the 8-byte flash programming unit
    UPD_ERR_NO_SESSION = 35u, // CHUNK or COMMIT without an accepted OFFER
    UPD_ERR_CHUNK_CRC = 36u,  // a chunk's data does not match its CRC
    UPD_ERR_INCOMPLETE = 37u, // COMMIT before every byte has arrived
    UPD_ERR_CANCELLED = 38u,  // the gateway sent ABORT
} upd_reason_t;

typedef struct {
    uint8_t version;
    uint8_t type;
    uint8_t node;
} proto_hdr_t;

// Returns 1 if msg holds a header of this protocol version, 0 otherwise
uint32_t proto_parse_header(const uint8_t *msg, size_t len, proto_hdr_t *out);

// STATUS: the node's answer to STATUS_REQ
//
//   4  running slot (0 = A, 1 = B)
//   5  slot A state
//   6  slot B state
//   7  trial attempts
//   8  running image version (major << 24 | minor << 16 | patch)
//  12  running image security counter
//  16  security floor
//  20  last result, a br_result_t
//  24  reset cause, RCC_CSR flags as in boot_handoff.h
//  28  sequence number of the newest boot record
typedef struct {
    uint8_t running_slot;
    uint8_t slot_state[2];
    uint8_t trial_attempts;
    uint32_t image_version;
    uint32_t image_counter;
    uint32_t security_floor;
    uint32_t last_result;
    uint32_t reset_flags;
    uint32_t boot_seq;
} proto_status_t;

#define PROTO_STATUS_LEN (PROTO_HDR_LEN + 28u)

// Returns the encoded length, or 0 if out_size is too small
size_t proto_encode_status(const proto_status_t *s, uint8_t node, uint8_t *out,
                           size_t out_size);

// Returns 1 if msg is a well-formed STATUS, 0 otherwise
uint32_t proto_decode_status(const uint8_t *msg, size_t len, proto_status_t *out);

// OFFER: the gateway proposes an image
//
//   4  image size: header, payload and trailer, as it will sit in the slot
//   8  image version
//  12  image security counter
//  16  target slot (0 = A, 1 = B), then 3 bytes of padding
//  20  chunk size, then 2 bytes of padding
//  24  SHA-256 of the whole image, 32 bytes. Identifies the image, so an
//      interrupted transfer is only ever resumed with the same image.
typedef struct {
    uint32_t image_size;
    uint32_t version;
    uint32_t security_counter;
    uint8_t target_slot;
    uint16_t chunk_size;
    uint8_t digest[32];
} proto_offer_t;

#define PROTO_OFFER_LEN (PROTO_HDR_LEN + 52u)

// Returns 1 if msg is a well-formed OFFER, 0 otherwise. Checks the format
// only: whether the offer is acceptable is the update agent's decision.
uint32_t proto_decode_offer(const uint8_t *msg, size_t len, proto_offer_t *out);

// CHUNK: a piece of the image
//
//   4  offset in the image
//   8  length, then 2 bytes of padding
//  12  CRC-32 of the data
//  16  the data
typedef struct {
    uint32_t offset;
    uint16_t length;
    uint32_t crc;
    const uint8_t *data; // points into the message
} proto_chunk_t;

#define PROTO_CHUNK_MAX 1024u
#define PROTO_CHUNK_HDR_LEN (PROTO_HDR_LEN + 12u)

// Returns 1 if msg is a well-formed CHUNK whose length field matches the data
// that follows and is at most PROTO_CHUNK_MAX, 0 otherwise. The CRC is not
// checked here: a mismatch is reported to the gateway, so it is the update
// agent's to detect.
uint32_t proto_decode_chunk(const uint8_t *msg, size_t len, proto_chunk_t *out);

// ACCEPT, REJECT, CHUNK_ACK, VERIFY_RESULT, REBOOT and ABORT carry a single
// value: a resume offset, a reason, the next expected offset, a br_result_t,
// a delay in milliseconds, a reason.
#define PROTO_U32_LEN (PROTO_HDR_LEN + 4u)

// Returns PROTO_U32_LEN, or 0 if out_size is too small
size_t proto_encode_u32(uint8_t type, uint8_t node, uint32_t value, uint8_t *out,
                        size_t out_size);

// Returns 1 if msg is a well-formed message of the given type with a single
// value, 0 otherwise
uint32_t proto_decode_u32(const uint8_t *msg, size_t len, uint8_t type, uint32_t *value);

// A STATUS_REQ is a header alone. Returns PROTO_HDR_LEN, or 0 if out_size is
// too small.
size_t proto_encode_status_req(uint8_t node, uint8_t *out, size_t out_size);

#endif // PROTOCOL_H
