#include "comms.h"

#include "FreeRTOS.h"
#include "stream_buffer.h"
#include "task.h"

#include "board.h"
#include "boot_record.h"
#include "flash_map.h"
#include "frame.h"
#include "lpuart1.h"
#include "protocol.h"
#include "status.h"
#include "supervisor.h"
#include "update_agent.h"

// See comms.h
#define COMMS_BAUD 115200u

// The status task's priority. Verifying an image keeps this task busy for
// up to 2.6 s, and at a higher priority it would starve the status task of
// its heartbeat. At the same priority the two share the processor.
#define COMMS_PRIORITY 1u

// Image verification runs on this stack. It peaked at 1.8 KB on target
// so this leaves more than half spare.
#define COMMS_STACK_WORDS 1024u

// The task wakes on received data, or after this long to report its
// heartbeat while the line is idle
#define COMMS_IDLE_MS 100u

// Bytes the interrupt can queue before the task reads them: about 22 ms at
// 115200 baud, far longer than the task ever waits to run
#define COMMS_RX_BUFFER_BYTES 256u

// The longest a COMMIT can take: verifying a full slot is 2.6 s at 16 MHz
// with margin
#define VERIFY_BUDGET_MS 4000u

// A transfer that hears nothing for this long is ended, so the LED does not
// show one in progress forever. Its progress stays in flash, so the gateway
// can still resume it with a new OFFER.
#define TRANSFER_TIMEOUT_MS 30000u

// Longer REBOOT delays are shortened to this
#define REBOOT_MAX_DELAY_MS 60000u

// This node's identifier. Fixed until there is more than one node on a bus.
#define NODE_ID 1u

// Mbed TLS checks signatures against this key, generated from
// keys/dev_public.pem at build time, the same key the bootloader uses
extern const uint8_t boot_pubkey[IMG_PUBKEY_LEN];

static StaticTask_t g_tcb;
static StackType_t g_stack[COMMS_STACK_WORDS];
static supervisor_id_t g_id;
static const boot_info_t *g_info;
static const flash_if_t *g_flash;

static upd_agent_t g_agent;
static TickType_t g_last_update; // when the last update message arrived
static uint32_t g_reboot_pending;
static TickType_t g_reboot_at;

// Bytes travel from the interrupt to the task through a stream buffer: one
// writer, one reader, which is exactly what a stream buffer is for
static StaticStreamBuffer_t g_rx_control;
static uint8_t g_rx_storage[COMMS_RX_BUFFER_BYTES + 1u];
static StreamBufferHandle_t g_rx;

static frame_rx_t g_frame;
static uint8_t g_msg[FRAME_MAX_PAYLOAD];
static uint8_t g_tx[FRAME_MAX_ENCODED];

// Interrupt context
static void on_rx_byte(uint8_t byte) {
    BaseType_t woken = pdFALSE;
    // If the buffer is full the byte is lost, and the frame it belonged to
    // fails its CRC and is dropped
    (void)xStreamBufferSendFromISR(g_rx, &byte, 1, &woken);
    portYIELD_FROM_ISR(woken);
}

static void send_message(const uint8_t *msg, size_t len) {
    size_t n = frame_encode(msg, len, g_tx, sizeof(g_tx));
    if (n > 0u) {
        lpuart1_write(g_tx, n);
    }
}

static void send_reply(upd_reply_t r) {
    uint8_t msg[PROTO_U32_LEN];
    size_t len = proto_encode_u32(r.type, NODE_ID, r.value, msg, sizeof(msg));
    if (len > 0u) {
        send_message(msg, len);
    }
}

static void send_status(void) {
    proto_status_t s;
    s.running_slot = (uint8_t)g_info->slot;
    s.slot_state[g_info->slot] = (uint8_t)g_info->state;
    s.slot_state[g_info->slot ^ 1u] = (uint8_t)g_info->other_state;
    s.trial_attempts = (uint8_t)g_info->trial_attempts;
    s.image_version = g_info->image_version;
    s.image_counter = g_info->image_counter;
    s.security_floor = g_info->security_floor;
    s.last_result = g_info->last_result;
    s.reset_flags = g_info->reset_flags;
    s.boot_seq = g_info->seq;

    // The boot record as it is now, since an update changes it after boot
    br_ctx_t ctx;
    boot_record_t rec;
    boot_record_init(&ctx, g_flash, BOOT_STATE_PAGE_A, BOOT_STATE_PAGE_B);
    if (boot_record_load(&ctx, &rec)) {
        s.slot_state[0] = rec.slot_state[0];
        s.slot_state[1] = rec.slot_state[1];
        s.trial_attempts = rec.trial_attempts;
        s.security_floor = rec.security_floor;
        s.last_result = rec.last_result;
        s.boot_seq = rec.seq;
    }

    uint8_t msg[PROTO_STATUS_LEN];
    size_t len = proto_encode_status(&s, NODE_ID, msg, sizeof(msg));
    if (len > 0u) {
        send_message(msg, len);
    }
}

static void commit(void) {
    status_set_activity(ACTIVITY_VERIFYING);
    // Verification is one call that cannot report a heartbeat partway
    supervisor_excuse(g_id, VERIFY_BUDGET_MS);
    upd_reply_t r = upd_commit(&g_agent);
    supervisor_beat(g_id);
    send_reply(r);
}

// Malformed messages are dropped without a reply, as a frame failing its CRC
// is: the gateway times out and sends again
static void handle_message(const uint8_t *msg, size_t len) {
    proto_hdr_t h;
    if (!proto_parse_header(msg, len, &h)) {
        return;
    }
    if (h.node != NODE_ID && h.node != PROTO_NODE_BROADCAST) {
        return;
    }

    proto_offer_t offer;
    proto_chunk_t chunk;
    uint32_t value;
    switch (h.type) {
    case MSG_STATUS_REQ:
        send_status();
        return;
    case MSG_OFFER:
        if (proto_decode_offer(msg, len, &offer)) {
            send_reply(upd_offer(&g_agent, &offer));
        }
        break;
    case MSG_CHUNK:
        if (proto_decode_chunk(msg, len, &chunk)) {
            send_reply(upd_chunk(&g_agent, &chunk));
        }
        break;
    case MSG_COMMIT:
        if (len == PROTO_HDR_LEN) {
            commit();
        }
        break;
    case MSG_ABORT:
        if (proto_decode_u32(msg, len, MSG_ABORT, &value)) {
            upd_abort(&g_agent);
        }
        break;
    case MSG_REBOOT:
        if (proto_decode_u32(msg, len, MSG_REBOOT, &value)) {
            if (value > REBOOT_MAX_DELAY_MS) {
                value = REBOOT_MAX_DELAY_MS;
            }
            g_reboot_at = xTaskGetTickCount() + pdMS_TO_TICKS(value);
            g_reboot_pending = 1u;
        }
        return;
    default:
        return;
    }

    // Only the update messages get here
    g_last_update = xTaskGetTickCount();
    status_set_activity(g_agent.active ? ACTIVITY_RECEIVING : ACTIVITY_NONE);
}

static void check_timers(void) {
    TickType_t now = xTaskGetTickCount();
    if (g_agent.active && now - g_last_update >= pdMS_TO_TICKS(TRANSFER_TIMEOUT_MS)) {
        upd_abort(&g_agent);
        status_set_activity(ACTIVITY_NONE);
    }
    if (g_reboot_pending && (int32_t)(now - g_reboot_at) >= 0) {
        board_reset();
    }
}

static void comms_task(void *arg) {
    (void)arg;
    uint8_t chunk[32];

    for (;;) {
        supervisor_beat(g_id);
        size_t n = xStreamBufferReceive(g_rx, chunk, sizeof(chunk),
                                        pdMS_TO_TICKS(COMMS_IDLE_MS));
        for (size_t i = 0; i < n; i++) {
            size_t len = frame_rx_push(&g_frame, chunk[i], g_msg, sizeof(g_msg));
            if (len > 0u) {
                handle_message(g_msg, len);
            }
        }
        check_timers();
    }
}

void comms_start(const boot_info_t *info, const flash_if_t *flash) {
    g_info = info;
    g_flash = flash;
    upd_init(&g_agent, flash, boot_pubkey, (uint8_t)info->slot);
    frame_rx_init(&g_frame);
    g_rx =
        xStreamBufferCreateStatic(COMMS_RX_BUFFER_BYTES, 1, g_rx_storage, &g_rx_control);
    g_id = supervisor_register();

    // The interrupt is enabled here, but FreeRTOS keeps interrupts at this
    // priority masked from the first task creation until the scheduler
    // starts, so no byte reaches the stream buffer before a task can read it
    lpuart1_init(COMMS_BAUD, on_rx_byte);

    (void)xTaskCreateStatic(comms_task, "comms", COMMS_STACK_WORDS, NULL, COMMS_PRIORITY,
                            g_stack, &g_tcb);
}
