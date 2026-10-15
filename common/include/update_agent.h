#ifndef UPDATE_AGENT_H
#define UPDATE_AGENT_H

#include <stdint.h>

#include "boot_record.h"
#include "flash_if.h"
#include "image_verify.h"
#include "progress.h"
#include "protocol.h"

// The node's side of an update
//
// Receives an image into the slot the node is not running from, and marks it
// PENDING once the whole slot verifies. Independent of the transport: the
// caller decodes each message, passes it here, and sends back the reply.
//
// The invariants it keeps, whatever the gateway sends and wherever power is
// lost:
//
//  - it never writes to the running slot or the primary slot
//  - it refuses an update while the running image is on trial, because the
//    other slot is then its only fallback
//  - the target slot is marked EMPTY before its first erase, so the
//    bootloader never considers a slot that is partly written
//  - the target slot is marked PENDING only after image_verify() accepts it
//  - an interrupted transfer resumes only for the same image, and only from
//    a page that was completely written
//
// Replies, each carrying one value:
//
//   OFFER   ACCEPT (the offset to send from) or REJECT (a reason)
//   CHUNK   CHUNK_ACK (the next offset expected), REJECT (a reason; the
//           session continues, and the gateway may resend), or ABORT
//           (FLASH_ERROR; the session ends)
//   COMMIT  VERIFY_RESULT (a br_result_t), or REJECT (a reason)
//
// A CHUNK at any offset other than the one expected, such as a resend after
// a lost CHUNK_ACK, is not written, and is answered with the offset expected.

typedef struct {
    uint8_t type; // a msg_type_t
    uint32_t value;
} upd_reply_t;

typedef struct {
    const flash_if_t *flash;
    const uint8_t *pubkey;
    uint8_t running_slot;
    br_ctx_t boot;
    progress_log_t progress;

    // The transfer in progress, valid while active is 1
    uint32_t active;
    uint8_t target_slot;
    uint16_t chunk_size;
    uint32_t image_size;
    uint32_t next_offset;
    uint8_t digest[PG_DIGEST_LEN];
} upd_agent_t;

void upd_init(upd_agent_t *a, const flash_if_t *flash,
              const uint8_t pubkey[IMG_PUBKEY_LEN], uint8_t running_slot);

upd_reply_t upd_offer(upd_agent_t *a, const proto_offer_t *offer);
upd_reply_t upd_chunk(upd_agent_t *a, const proto_chunk_t *chunk);
upd_reply_t upd_commit(upd_agent_t *a);

// The gateway's ABORT: ends the transfer, keeping its progress for a resume
void upd_abort(upd_agent_t *a);

#endif // UPDATE_AGENT_H
