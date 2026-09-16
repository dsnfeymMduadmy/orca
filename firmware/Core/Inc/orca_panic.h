#ifndef ORCA_PANIC_H
#define ORCA_PANIC_H

#include <stdint.h>

/*
 * Единая точка "всё умерло". До неё каждый отказ был отдельным while(1):
 * плата просто замирала, светодиод гас, UART молчал — по внешнему виду
 * невозможно отличить HardFault от переполнения стека или от того, что
 * задача просто не получает такты.
 *
 * Теперь любой отказ печатает причину в USART1 напрямую (без FreeRTOS и
 * без прерываний — работает, когда планировщик уже мёртв) и дальше мигает
 * красным LED0 своим кодом, чтобы диагноз читался вообще без ПК.
 */
typedef enum {
    ORCA_PANIC_NMI        = 1,
    ORCA_PANIC_HARDFAULT  = 2,
    ORCA_PANIC_MEMMANAGE  = 3,
    ORCA_PANIC_BUSFAULT   = 4,
    ORCA_PANIC_USAGEFAULT = 5,
    ORCA_PANIC_STACK_OVF  = 6,
    ORCA_PANIC_MALLOC     = 7,
    ORCA_PANIC_ASSERT     = 8,
    ORCA_PANIC_ERROR_HDLR = 9
} orca_panic_code_t;

/*
 * frame — указатель на аппаратно уложенный фрейм исключения
 * (r0 r1 r2 r3 r12 lr pc xpsr) или NULL, если причина не аппаратная.
 * name — имя задачи/контекста, может быть NULL.
 */
void orca_panic(uint32_t code, const uint32_t* frame, const char* name)
    __attribute__((noreturn));

#endif /* ORCA_PANIC_H */
