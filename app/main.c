#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "board.h"
#include "boot_info.h"
#include "fault.h"
#include "flash_stm32.h"
#include "status.h"
#include "supervisor.h"

static flash_if_t g_flash;

// How this image was booted. Not static, so it can be read over SWD by symbol
// name: mdw &g_boot_info 8.
boot_info_t g_boot_info;

int main(void) {
    board_init();

    flash_stm32_bind(&g_flash);
    boot_info_load(&g_flash, &g_boot_info);

    // Every task that must keep running registers with the supervisor as it
    // is created, so the supervisor starts last
    status_start(&g_boot_info);
    fault_start();
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