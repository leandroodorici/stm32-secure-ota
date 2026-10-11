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
// Only STATUS_REQ and STATUS exist so far. The remaining messages
// arrive with the update agent.

#define PROTO_VERSION 1u
#define PROTO_HDR_LEN 4u

// A request addressed to every node
#define PROTO_NODE_BROADCAST 0xFFu

typedef enum {
    MSG_STATUS_REQ = 0x01u,
    MSG_STATUS = 0x02u,
} msg_type_t;

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

// A STATUS_REQ is a header alone. Returns PROTO_HDR_LEN, or 0 if out_size is
// too small.
size_t proto_encode_status_req(uint8_t node, uint8_t *out, size_t out_size);

#endif // PROTOCOL_H
