#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "boot_handoff.h"
#include "boot_info.h"
#include "boot_record.h"
#include "flash_stm32.h"
#include "supervisor.h"

// LD2, the user LED on the NUCLEO-G474RE, is on PA5
#define RCC_AHB2ENR (*(volatile uint32_t *)0x4002104Cu)
#define GPIOA_MODER (*(volatile uint32_t *)0x48000000u)
#define GPIOA_ODR (*(volatile uint32_t *)0x48000014u)

#define LED_PIN 5u

// Task priorities. Higher numbers run first. The supervisor takes the
// highest.
#define BLINK_PRIORITY 1u
#define BLINK_STACK_WORDS 128u

static StaticTask_t g_blink_tcb;
static StackType_t g_blink_stack[BLINK_STACK_WORDS];
static supervisor_id_t g_blink_id;

static flash_if_t g_flash;

// How this image was booted. Not static, so it can be read over SWD by symbol
// name: mdw &g_boot_info 8.
boot_info_t g_boot_info;

// Blinks LD2 at a rate that shows how the image is running, visible without a
// debugger:
//   fast   (125 ms)  the last reset was the watchdog
//   medium (250 ms)  this image is on trial, not yet confirmed
//   slow   (500 ms)  confirmed
// The magic check guards against an application started some other way,
// where the handoff RAM holds whatever was there before.
static void blink_task(void *arg) {
    (void)arg;
    uint32_t half_period_ms = 500u;
    if (g_boot_info.state == SLOT_TRIAL) {
        half_period_ms = 250u;
    }
    if (BOOT_HANDOFF->magic == BOOT_HANDOFF_MAGIC &&
        (BOOT_HANDOFF->reset_flags & RESET_FLAG_IWDG) != 0u) {
        half_period_ms = 125u;
    }

    TickType_t last = xTaskGetTickCount();
    for (;;) {
        GPIOA_ODR ^= (1u << LED_PIN);
        supervisor_beat(g_blink_id);
        xTaskDelayUntil(&last, pdMS_TO_TICKS(half_period_ms));
    }
}

int main(void) {
    // Every peripheral is clock-gated off at reset. Enable GPIOA first.
    // Writing to its registers before this has no effect at all.
    RCC_AHB2ENR |= (1u << 0);

    // MODER holds 2 bits per pin. 01 is general-purpose output
    GPIOA_MODER &= ~(3u << (LED_PIN * 2));
    GPIOA_MODER |= (1u << (LED_PIN * 2));

    flash_stm32_bind(&g_flash);
    boot_info_load(&g_flash, &g_boot_info);

    g_blink_id = supervisor_register();
    (void)xTaskCreateStatic(blink_task, "blink", BLINK_STACK_WORDS, NULL, BLINK_PRIORITY,
                            g_blink_stack, &g_blink_tcb);
    supervisor_start();

    vTaskStartScheduler();

    // Only reached if the scheduler could not start
    app_assert_failed(__FILE__, __LINE__);
    return 0;
}

// ---- FreeRTOS hooks ----

// With dynamic allocation disabled, the kernel asks the application for the
// idle task's memory
void vApplicationGetIdleTaskMemory(StaticTask_t **tcb, StackType_t **stack,
                                   configSTACK_DEPTH_TYPE *stack_words) {
    static StaticTask_t idle_tcb;
    static StackType_t idle_stack[configMINIMAL_STACK_SIZE];

    *tcb = &idle_tcb;
    *stack = idle_stack;
    *stack_words = configMINIMAL_STACK_SIZE;
}

void vApplicationStackOverflowHook(TaskHandle_t task, char *name) {
    (void)task;
    (void)name;
    app_assert_failed(__FILE__, __LINE__);
}

// See configASSERT in FreeRTOSConfig.h. Interrupts stay off, so nothing else
// runs and the supervisor stops feeding the watchdog. The arguments are left
// for a debugger to inspect.
void app_assert_failed(const char *file, int line) {
    (void)file;
    (void)line;
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}