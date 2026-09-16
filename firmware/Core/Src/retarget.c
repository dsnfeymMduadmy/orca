#include "stm32h7xx_hal.h"
#include "orca_panic.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <sys/stat.h>
#include <errno.h>

extern UART_HandleTypeDef huart1;
extern int bsp_uart_console_write(const uint8_t* buf, uint32_t len);

/*
 * printf идёт тем же путём, что и весь остальной вывод в USART1: под
 * мьютексом TX. Прямой HAL_UART_Transmit вклинивался в середину бинарного
 * кадра Orca Link (например, лог модуля во время трансляции экрана) — на
 * ПК это выглядело как поток CRC-ошибок и рваная картинка.
 */
int _write(int file, char *ptr, int len)
{
    (void)file;
    if (len <= 0) {
        return 0;
    }
    int n = bsp_uart_console_write((const uint8_t*)ptr, (uint32_t)len);
    return (n < 0) ? -1 : n;
}

int _read(int file, char *ptr, int len)
{
    (void)file;
    (void)ptr;
    (void)len;
    return 0;
}

int _close(int file)
{
    (void)file;
    return -1;
}

int _lseek(int file, int ptr, int dir)
{
    (void)file;
    (void)ptr;
    (void)dir;
    return 0;
}

int _fstat(int file, struct stat *st)
{
    (void)file;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file)
{
    (void)file;
    return 1;
}

/*
 * Куча newlib. Ей пользуется не FreeRTOS (у той свой heap_4 статическим
 * массивом), а libc: snprintf с большим буфером, strdup и прочее.
 * Верхняя граница _heap_limit приходит из линкер-скрипта (конец RAM_D1).
 * Без неё _sbrk отдавал адреса за концом региона, и падало это не здесь,
 * а позже, на записи по мусорному указателю — искать такое часами.
 */
void *_sbrk(int incr)
{
    extern char _end;
    extern char _heap_limit;
    static char *heap_end = NULL;
    char *prev_heap_end;

    if (heap_end == NULL) {
        heap_end = &_end;
    }
    if (incr < 0) {
        errno = EINVAL;
        return (void *)-1;
    }
    /* Считаем в остатке, а не в сумме: heap_end + incr может переполниться. */
    if ((size_t)incr > (size_t)(&_heap_limit - heap_end)) {
        errno = ENOMEM;
        return (void *)-1;
    }
    prev_heap_end = heap_end;
    heap_end += incr;
    return (void *)prev_heap_end;
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    orca_panic(ORCA_PANIC_STACK_OVF, NULL, pcTaskName);
}

void vApplicationMallocFailedHook(void)
{
    orca_panic(ORCA_PANIC_MALLOC, NULL, NULL);
}

#if (configSUPPORT_STATIC_ALLOCATION == 1)
static StaticTask_t xIdleTaskTCB;
static StackType_t xIdleStack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *pulIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer = &xIdleTaskTCB;
    *ppxIdleTaskStackBuffer = &xIdleStack[0];
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

static StaticTask_t xTimerTaskTCB;
static StackType_t xTimerStack[configTIMER_TASK_STACK_DEPTH];

void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
                                    StackType_t **ppxTimerTaskStackBuffer,
                                    uint32_t *pulTimerTaskStackSize)
{
    *ppxTimerTaskTCBBuffer = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = &xTimerStack[0];
    *pulTimerTaskStackSize = configTIMER_TASK_STACK_DEPTH;
}
#endif

