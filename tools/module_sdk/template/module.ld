/*
 * Линкер-скрипт для relocatable-модулей Orca.
 * Модуль собирается как ET_REL (частичная линковка, `ld -r`), поэтому
 * этот скрипт лишь задаёт порядок и выравнивание секций — реальные
 * адреса назначает orca_loader в runtime (все секции ALLOC).
 */
SECTIONS
{
    .text : {
        *(.text*)
        *(.rodata*)
    }

    .data : {
        *(.data*)
    }

    .bss (NOLOAD) : {
        *(.bss*)
        *(COMMON)
    }
}
