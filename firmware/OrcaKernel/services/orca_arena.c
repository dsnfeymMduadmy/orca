#include "orca_arena.h"
#include "orca_memmap.h"
#include "board.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>
#include <string.h>

typedef struct block {
    uint32_t     size;   /* размер полезной области, без заголовка */
    struct block* next;
    uint8_t      free;
} block_t;

static block_t* s_free_list = NULL;
static uint8_t  s_initialized = 0;

#define ALIGN_UP(v, a) (((v) + ((a) - 1)) & ~((uintptr_t)(a) - 1))
#define BLOCK_HDR_SIZE (sizeof(block_t))

/* Заголовок кладём только по адресам, годным для указателя внутри block_t. */
#define BLOCK_ALIGN    8u

/*
 * Арену дёргают из двух разных контекстов: загрузчик работает в задаче
 * shell/Link, а orca_loader_unload() вызывается ещё и из app_task_entry
 * при штатном выходе модуля. Без взаимного исключения два прохода по
 * free-list (split в alloc и merge в free) рвали список: следующий alloc
 * уходил по указателю в уже слитый блок.
 *
 * Мьютекс не годится: orca_arena_init() зовётся из orca_appmgr_init() до
 * vTaskStartScheduler(), а первый alloc может случиться там же. Поэтому
 * критическая секция — операции короткие (проход по десятку блоков).
 */
static void arena_lock(void)
{
    taskENTER_CRITICAL();
}

static void arena_unlock(void)
{
    taskEXIT_CRITICAL();
}

bool orca_arena_ready(void)
{
    return s_initialized != 0;
}

void orca_arena_init(void)
{
    /*
     * Арена — это окно во внешнюю SDRAM. Если FMC не поднялся, запись
     * заголовка по 0xC0600000 — это bus fault прямо в стартовой последовательности,
     * без единого сообщения в консоль. Лучше остаться без арены: shell
     * и Orca Link продолжат работать, а alloc будет честно возвращать NULL.
     */
    if (!board_sdram_ready()) {
        s_free_list = NULL;
        s_initialized = 0;
        return;
    }

    block_t* first = (block_t*)(uintptr_t)ORCA_APP_ARENA_BASE;
    first->size = ORCA_APP_ARENA_SIZE - BLOCK_HDR_SIZE;
    first->next = NULL;
    first->free = 1;
    s_free_list = first;
    s_initialized = 1;
}

void* orca_arena_alloc(uint32_t size, uint32_t align)
{
    if (!s_initialized) {
        orca_arena_init();
        if (!s_initialized) {
            return NULL;
        }
    }
    if (size == 0) {
        return NULL;
    }
    if (align == 0) {
        align = 4;
    }

    void* result = NULL;
    arena_lock();

    block_t* cur = s_free_list;
    while (cur != NULL) {
        if (cur->free) {
            uintptr_t data_addr = (uintptr_t)cur + BLOCK_HDR_SIZE;
            uintptr_t block_end = data_addr + cur->size;
            uintptr_t aligned   = ALIGN_UP(data_addr, align);
            uint32_t  padding   = (uint32_t)(aligned - data_addr);

            /*
             * size + padding не складываем: size приходит из заголовков ELF
             * и при битом файле легко даёт переполнение и "подходящий" блок.
             */
            if (cur->size >= padding && (cur->size - padding) >= size) {
                /*
                 * Хвост отрезаем по выровненному адресу, а не по aligned+size:
                 * при нечётном size заголовок нового блока ложился со сдвигом,
                 * и запись split->next была невыровненным доступом к указателю.
                 */
                uintptr_t split_addr = ALIGN_UP(aligned + size, BLOCK_ALIGN);
                if (block_end >= split_addr + BLOCK_HDR_SIZE + 32u) {
                    block_t* split = (block_t*)split_addr;
                    split->size = (uint32_t)(block_end - split_addr - BLOCK_HDR_SIZE);
                    split->free = 1;
                    split->next = cur->next;
                    cur->next = split;
                    cur->size = (uint32_t)(split_addr - data_addr);
                }
                cur->free = 0;
                result = (void*)aligned;
                break;
            }
        }
        cur = cur->next;
    }

    arena_unlock();
    return result;
}

void orca_arena_free(void* ptr)
{
    if (ptr == NULL) {
        return;
    }

    arena_lock();

    /*
     * Диагностика важнее самой операции: указатель не из арены и повторный
     * free — это ошибки вызывающего, а раньше функция на них просто выходила.
     * Снаружи это выглядело как «арена течёт»: памяти нет, а кто её не отдал,
     * непонятно. Печатать под critical нельзя (printf берёт мьютекс UART),
     * поэтому только запоминаем факт, а говорим после разблокировки.
     */
    bool found       = false;
    bool double_free = false;

    block_t* cur = s_free_list;
    while (cur != NULL) {
        uintptr_t data_addr = (uintptr_t)cur + BLOCK_HDR_SIZE;
        if (data_addr <= (uintptr_t)ptr &&
            (uintptr_t)ptr < data_addr + cur->size) {
            found       = true;
            double_free = (cur->free != 0);
            cur->free = 1;
            break;
        }
        cur = cur->next;
    }

    /* слияние соседних свободных блоков (простой linear merge проходом по списку) */
    cur = s_free_list;
    while (cur != NULL && cur->next != NULL) {
        uintptr_t end_of_cur = (uintptr_t)cur + BLOCK_HDR_SIZE + cur->size;
        if (cur->free && cur->next->free && end_of_cur == (uintptr_t)cur->next) {
            cur->size += BLOCK_HDR_SIZE + cur->next->size;
            cur->next = cur->next->next;
        } else {
            cur = cur->next;
        }
    }

    arena_unlock();

    if (!found) {
        printf("[WARN] arena: free(0x%08lX) — адрес не из арены, игнорирую\r\n",
               (unsigned long)(uintptr_t)ptr);
    } else if (double_free) {
        printf("[WARN] arena: free(0x%08lX) — блок уже был свободен\r\n",
               (unsigned long)(uintptr_t)ptr);
    }
}

uint32_t orca_arena_free_bytes(void)
{
    uint32_t total = 0;
    arena_lock();
    for (block_t* cur = s_free_list; cur != NULL; cur = cur->next) {
        if (cur->free) {
            total += cur->size;
        }
    }
    arena_unlock();
    return total;
}

uint32_t orca_arena_largest_free_block(void)
{
    uint32_t largest = 0;
    arena_lock();
    for (block_t* cur = s_free_list; cur != NULL; cur = cur->next) {
        if (cur->free && cur->size > largest) {
            largest = cur->size;
        }
    }
    arena_unlock();
    return largest;
}
