#ifndef ORCA_ELF_H
#define ORCA_ELF_H

#include <stdint.h>

/*
 * Минимальный набор структур ELF32, необходимый для загрузки
 * relocatable-объектов (ET_REL) модулей/приложений Orca.
 * Не претендует на полноту libelf — только то, что нужно loader'у.
 */

#define EI_NIDENT 16

typedef struct {
    uint8_t  e_ident[EI_NIDENT];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint32_t e_entry;
    uint32_t e_phoff;
    uint32_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    uint32_t sh_name;
    uint32_t sh_type;
    uint32_t sh_flags;
    uint32_t sh_addr;
    uint32_t sh_offset;
    uint32_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint32_t sh_addralign;
    uint32_t sh_entsize;
} Elf32_Shdr;

typedef struct {
    uint32_t st_name;
    uint32_t st_value;
    uint32_t st_size;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
} Elf32_Sym;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
} Elf32_Rel;

typedef struct {
    uint32_t r_offset;
    uint32_t r_info;
    int32_t  r_addend;
} Elf32_Rela;

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ET_REL  1
#define EM_ARM  40

#define SHT_NULL     0
#define SHT_PROGBITS 1
#define SHT_SYMTAB   2
#define SHT_STRTAB   3
#define SHT_RELA     4
#define SHT_NOBITS   8
#define SHT_REL      9

#define SHF_ALLOC     0x2
#define SHF_EXECINSTR 0x4
#define SHF_WRITE     0x1

#define STN_UNDEF 0

#define ELF32_ST_TYPE(info) ((info) & 0xf)
#define ELF32_ST_BIND(info) ((info) >> 4)
#define STB_GLOBAL 1
#define STT_FUNC   2
#define STT_OBJECT 1

#define ELF32_R_SYM(info)  ((info) >> 8)
#define ELF32_R_TYPE(info) ((info) & 0xff)

/* Типы релокаций ARM, которые умеет применять orca_loader */
#define R_ARM_NONE       0
#define R_ARM_ABS32      2
#define R_ARM_REL32      3
#define R_ARM_THM_CALL   10
#define R_ARM_PREL31     42
#define R_ARM_THM_JUMP24 30
#define R_ARM_TARGET1    38

/*
 * Бит 0 адреса функции в Thumb-режиме — признак Thumb, а не часть адреса.
 * st_value для STT_FUNC приходит из ассемблера уже с этим битом, поэтому
 * при вычислении смещения ветвления его надо снимать, а при записи
 * указателя на функцию — оставлять.
 */
#define ELF_THUMB_BIT 1u

#endif /* ORCA_ELF_H */
