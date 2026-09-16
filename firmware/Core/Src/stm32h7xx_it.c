#include "main.h"
#include "stm32h7xx_it.h"
#include "orca_panic.h"
#include "FreeRTOS.h"
#include "task.h"

extern void xPortSysTickHandler(void);

/*
 * Обработчики отказов сделаны naked: нужен нетронутый указатель на фрейм
 * исключения, чтобы достать из него PC и LR — адрес инструкции, которая
 * упала. Компилятор в обычной функции успевает подвинуть стек и сохранить
 * регистры, и фрейм уже не найти.
 *
 * Бит 2 в LR говорит, на каком стеке лежит фрейм: 0 — MSP (упали в
 * прерывании), 1 — PSP (упали в задаче).
 */
#define FAULT_ENTRY(handler, code)                  \
    __attribute__((naked)) void handler(void)       \
    {                                               \
        __asm volatile (                            \
            "tst   lr, #4              \n"          \
            "ite   eq                  \n"          \
            "mrseq r1, msp             \n"          \
            "mrsne r1, psp             \n"          \
            "movs  r0, #%c0            \n"          \
            "movs  r2, #0              \n"          \
            "b     orca_panic          \n"          \
            :: "i" (code)                           \
        );                                          \
    }

FAULT_ENTRY(NMI_Handler,        ORCA_PANIC_NMI)
FAULT_ENTRY(HardFault_Handler,  ORCA_PANIC_HARDFAULT)
FAULT_ENTRY(MemManage_Handler,  ORCA_PANIC_MEMMANAGE)
FAULT_ENTRY(BusFault_Handler,   ORCA_PANIC_BUSFAULT)
FAULT_ENTRY(UsageFault_Handler, ORCA_PANIC_USAGEFAULT)

void DebugMon_Handler(void)
{
}

void SysTick_Handler(void)
{
    HAL_IncTick();
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xPortSysTickHandler();
    }
}
