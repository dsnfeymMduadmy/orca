#ifndef ORCA_ARENA_H
#define ORCA_ARENA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Простой allocator (first-fit free-list) над регионом SDRAM,
 * выделенным под код/данные/стеки загружаемых модулей и приложений
 * (ORCA_APP_ARENA_BASE / ORCA_APP_ARENA_SIZE, см. orca_memmap.h).
 *
 * Арена существует только если SDRAM поднялась (board_sdram_ready()).
 * Иначе orca_arena_ready() == false, а alloc возвращает NULL.
 */

void  orca_arena_init(void);
bool  orca_arena_ready(void);
void* orca_arena_alloc(uint32_t size, uint32_t align);
void  orca_arena_free(void* ptr);
uint32_t orca_arena_free_bytes(void);
uint32_t orca_arena_largest_free_block(void);

#endif /* ORCA_ARENA_H */
