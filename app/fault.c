#include "fault.h"

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "supervisor.h"

// See fault.h
#define FAULT_POLL_MS 50u
#define FAULT_PRIORITY 2u
#define FAULT_STACK_WORDS 128u

// Two consecutive samples, 50 ms apart, so contact bounce or a glitch on the
// pin is not taken for a press
#define FAULT_PRESS_SAMPLES 2u

static StaticTask_t g_tcb;
static StackType_t g_stack[FAULT_STACK_WORDS];
static supervisor_id_t g_id;
static volatile uint32_t g_injected;

uint32_t fault_injected(void) {
    return g_injected;
}

static void fault_task(void *arg) {
    (void)arg;
    uint32_t pressed_samples = 0;

    TickType_t last = xTaskGetTickCount();
    while (pressed_samples < FAULT_PRESS_SAMPLES) {
        supervisor_beat(g_id);
        pressed_samples = board_button_pressed() ? pressed_samples + 1u : 0u;
        xTaskDelayUntil(&last, pdMS_TO_TICKS(FAULT_POLL_MS));
    }

    // Still scheduled and running, but no longer reporting
    g_injected = 1u;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000u));
    }
}

void fault_start(void) {
    g_id = supervisor_register();
    (void)xTaskCreateStatic(fault_task, "fault", FAULT_STACK_WORDS, NULL, FAULT_PRIORITY,
                            g_stack, &g_tcb);
}
