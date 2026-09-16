#include "stm32h7xx_hal.h"
#include <stdint.h>

/*
 * После записи кода модуля в SDRAM его нужно вытолкнуть из D-cache в память
 * и выкинуть возможные устаревшие строки из I-cache: иначе Cortex-M7 может
 * начать исполнять мусор, который лежал по этим адресам раньше.
 * Адрес/размер выравниваются на строку кеша (32 байта).
 */
void orca_loader_cache_sync(void* addr, uint32_t size)
{
    if (addr == NULL || size == 0) {
        return;
    }

    uint32_t start = (uint32_t)(uintptr_t)addr;
    uint32_t end   = start + size;

    uint32_t aligned_start = start & ~(uint32_t)0x1Fu;
    uint32_t aligned_size  = ((end + 0x1Fu) & ~(uint32_t)0x1Fu) - aligned_start;

    SCB_CleanDCache_by_Addr((uint32_t*)(uintptr_t)aligned_start, (int32_t)aligned_size);
    SCB_InvalidateICache_by_Addr((void*)(uintptr_t)aligned_start, (int32_t)aligned_size);
    __DSB();
    __ISB();
}
