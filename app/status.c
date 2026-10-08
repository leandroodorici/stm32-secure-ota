#include "status.h"

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "boot_handoff.h"
#include "boot_record.h"
#include "fault.h"
#include "supervisor.h"

// See status.h. Everything is timed in steps of one tick of this task.
#define STATUS_TICK_MS 125u
#define STATUS_PRIORITY 1u
#define STATUS_STACK_WORDS 128u

#define LD2_STEPS 4u            // 500 ms per toggle
#define TRIAL_STEPS 2u          // 250 ms on, 250 ms off
#define REJECT_PERIOD_STEPS 16u // a red flash every 2 s
#define REJECT_FLASH_STEPS 2u   // lasting 250 ms

static StaticTask_t g_tcb;
static StackType_t g_stack[STATUS_STACK_WORDS];
static supervisor_id_t g_id;
static const boot_info_t *g_info;

static void status_task(void *arg) {
    (void)arg;

    rgb_t base = (g_info->slot == 1u) ? RGB_BLUE : RGB_GREEN;
    uint32_t trial = (g_info->state == SLOT_TRIAL);
    // NONE means no result was recorded, OK means the last decision accepted
    // an image. Anything else is a rejection reason.
    uint32_t rejected = g_info->record_found && g_info->last_result != BR_RESULT_OK &&
                        g_info->last_result != BR_RESULT_NONE;
    // The magic check guards against an application started some other way,
    // where the handoff RAM holds whatever was there before
    uint32_t after_watchdog = BOOT_HANDOFF->magic == BOOT_HANDOFF_MAGIC &&
                              (BOOT_HANDOFF->reset_flags & RESET_FLAG_IWDG) != 0u;

    TickType_t last = xTaskGetTickCount();
    for (uint32_t step = 0;; step++) {
        supervisor_beat(g_id);

        if (after_watchdog || (step % LD2_STEPS) == 0u) {
            board_ld2_toggle();
        }

        rgb_t colour = base;
        if (fault_injected()) {
            colour = RGB_WHITE;
        } else if (rejected && (step % REJECT_PERIOD_STEPS) < REJECT_FLASH_STEPS) {
            colour = RGB_RED;
        } else if (trial && ((step / TRIAL_STEPS) % 2u) == 1u) {
            colour = RGB_OFF;
        }
        board_rgb(colour);

        xTaskDelayUntil(&last, pdMS_TO_TICKS(STATUS_TICK_MS));
    }
}

void status_start(const boot_info_t *info) {
    g_info = info;
    g_id = supervisor_register();
    (void)xTaskCreateStatic(status_task, "status", STATUS_STACK_WORDS, NULL,
                            STATUS_PRIORITY, g_stack, &g_tcb);
}
