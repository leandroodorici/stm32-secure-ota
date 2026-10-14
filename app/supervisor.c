#include "supervisor.h"

#include "FreeRTOS.h"
#include "task.h"

#define IWDG_KR (*(volatile uint32_t *)0x40003000u)
#define IWDG_KEY_REFRESH 0xAAAAu

// The watchdog times out after at least 3.76 s. Checking every
// second means a stuck task is caught within about four to five seconds,
// while a task that misses one check through bad luck in timing is not.
#define SUPERVISOR_PERIOD_MS 1000u
#define SUPERVISOR_STACK_WORDS 128u
#define SUPERVISOR_PRIORITY (configMAX_PRIORITIES - 1u)

// One flag per task. Each is a single byte, written whole by its task and
// cleared by the supervisor, so no locking is needed.
static volatile uint8_t g_alive[SUPERVISOR_MAX_TASKS];
static uint32_t g_count;

// Checks a task may still miss after supervisor_excuse. Set by the task and
// counted down by the supervisor. The supervisor runs at a higher priority,
// so the task can never interrupt the count down halfway.
static volatile uint8_t g_grace[SUPERVISOR_MAX_TASKS];

static StaticTask_t g_tcb;
static StackType_t g_stack[SUPERVISOR_STACK_WORDS];

supervisor_id_t supervisor_register(void) {
    configASSERT(g_count < SUPERVISOR_MAX_TASKS);
    return g_count++;
}

void supervisor_beat(supervisor_id_t id) {
    g_alive[id] = 1u;
    g_grace[id] = 0u;
}

void supervisor_excuse(supervisor_id_t id, uint32_t ms) {
    // An operation of ms can span one check more than ms holds periods
    uint32_t checks = ms / SUPERVISOR_PERIOD_MS + 1u;
    g_grace[id] = (uint8_t)(checks < 255u ? checks : 255u);
}

static void supervisor_task(void *arg) {
    (void)arg;
    TickType_t last = xTaskGetTickCount();

    for (;;) {
        xTaskDelayUntil(&last, pdMS_TO_TICKS(SUPERVISOR_PERIOD_MS));

        uint32_t all_alive = 1u;
        for (uint32_t i = 0; i < g_count; i++) {
            if (g_alive[i] == 0u && g_grace[i] == 0u) {
                all_alive = 0u;
            }
        }
        if (all_alive != 0u) {
            for (uint32_t i = 0; i < g_count; i++) {
                if (g_alive[i] == 0u) {
                    g_grace[i]--; // an excused check, used up
                }
                g_alive[i] = 0u;
            }
            IWDG_KR = IWDG_KEY_REFRESH;
        }
    }
}

void supervisor_start(void) {
    // Highest application priority, so a busy lower-priority task cannot
    // starve the check itself
    (void)xTaskCreateStatic(supervisor_task, "supervisor", SUPERVISOR_STACK_WORDS, NULL,
                            SUPERVISOR_PRIORITY, g_stack, &g_tcb);
}
