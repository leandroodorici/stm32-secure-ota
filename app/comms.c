#include "comms.h"

#include "FreeRTOS.h"
#include "stream_buffer.h"
#include "task.h"

#include "frame.h"
#include "lpuart1.h"
#include "protocol.h"
#include "supervisor.h"

// See comms.h
#define COMMS_BAUD 115200u
#define COMMS_PRIORITY 2u
#define COMMS_STACK_WORDS 256u

// The task wakes on received data, or after this long to report its
// heartbeat while the line is idle
#define COMMS_IDLE_MS 100u

// Bytes the interrupt can queue before the task reads them: about 22 ms at
// 115200 baud, far longer than the task ever waits to run
#define COMMS_RX_BUFFER_BYTES 256u

// This node's identifier. Fixed until there is more than one node on a bus.
#define NODE_ID 1u

static StaticTask_t g_tcb;
static StackType_t g_stack[COMMS_STACK_WORDS];
static supervisor_id_t g_id;
static const boot_info_t *g_info;

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

    uint8_t msg[PROTO_STATUS_LEN];
    size_t len = proto_encode_status(&s, NODE_ID, msg, sizeof(msg));
    if (len > 0u) {
        send_message(msg, len);
    }
}

static void handle_message(const uint8_t *msg, size_t len) {
    proto_hdr_t h;
    if (!proto_parse_header(msg, len, &h)) {
        return;
    }
    if (h.node != NODE_ID && h.node != PROTO_NODE_BROADCAST) {
        return;
    }
    if (h.type == MSG_STATUS_REQ) {
        send_status();
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
    }
}

void comms_start(const boot_info_t *info) {
    g_info = info;
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
