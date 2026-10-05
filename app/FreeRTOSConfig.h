#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

// FreeRTOS configuration for the application. The bootloader does not use an
// RTOS (docs/DESIGN.md section 8.1).
//
// Everything is statically allocated: no heap implementation is linked, so
// every task's memory is visible in the map file and nothing can fail to
// allocate at run time.

// ---- clock and tick ----

// The bootloader hands over at the reset clock: HSI16, no PLL
#define configCPU_CLOCK_HZ 16000000UL
#define configTICK_RATE_HZ 1000U
#define configTICK_TYPE_WIDTH_IN_BITS TICK_TYPE_WIDTH_32_BITS
#define configUSE_PREEMPTION 1
#define configUSE_TIME_SLICING 1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE 0

// ---- tasks ----

#define configMAX_PRIORITIES 5U
#define configMINIMAL_STACK_SIZE 128U // words
#define configMAX_TASK_NAME_LEN 12U
#define configIDLE_SHOULD_YIELD 1
#define configUSE_TASK_NOTIFICATIONS 1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES 1U

// ---- memory ----

#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 0
#define configKERNEL_PROVIDED_STATIC_MEMORY 0

// ---- features not used yet ----

#define configUSE_TIMERS 0
#define configUSE_MUTEXES 0
#define configUSE_RECURSIVE_MUTEXES 0
#define configUSE_COUNTING_SEMAPHORES 0
#define configUSE_QUEUE_SETS 0
#define configUSE_CO_ROUTINES 0
#define configQUEUE_REGISTRY_SIZE 0U
#define configUSE_TRACE_FACILITY 0
#define configGENERATE_RUN_TIME_STATS 0

// ---- hooks ----

#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configUSE_MALLOC_FAILED_HOOK 0
#define configUSE_DAEMON_TASK_STARTUP_HOOK 0

// Method 2 also checks a pattern at the end of the stack, catching overflows
// that have already returned by the time of the context switch
#define configCHECK_FOR_STACK_OVERFLOW 2

// ---- interrupt priorities ----
//
// The G474 implements 4 priority bits, so priorities run 0 (highest) to 15.
// The kernel runs at the lowest. Interrupts at numerically 5 or above (lower
// urgency) may call FreeRTOS "FromISR" functions; anything above that is
// never delayed by the kernel but must not touch it. Getting this wrong is
// the most common FreeRTOS bug on Cortex-M, and configASSERT catches it.
#define configPRIO_BITS 4U
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 15U
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5U
#define configKERNEL_INTERRUPT_PRIORITY                                                  \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8U - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY                                             \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8U - configPRIO_BITS))

// ---- handlers ----
//
// The port's handlers take the names used in the vector table in startup.c,
// so they replace the weak defaults there. The port checks at start-up that
// the vector table really points at them.
#define vPortSVCHandler SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

// ---- assertions ----
//
// A failed assertion is a bug. Stop with interrupts disabled: the watchdog is
// no longer fed, so the part resets, and on a trial image that reset counts
// as a failed attempt.
void app_assert_failed(const char *file, int line);
#define configASSERT(x)                                                                  \
    do {                                                                                 \
        if ((x) == 0) {                                                                  \
            app_assert_failed(__FILE__, __LINE__);                                       \
        }                                                                                \
    } while (0)

// ---- API functions included ----

#define INCLUDE_vTaskDelay 1
#define INCLUDE_xTaskDelayUntil 1
#define INCLUDE_vTaskDelete 0
#define INCLUDE_vTaskSuspend 0
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1

#endif // FREERTOS_CONFIG_H
