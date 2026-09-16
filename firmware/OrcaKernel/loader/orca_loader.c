#include "orca_loader.h"
#include "orca_elf.h"
#include "orca_arena.h"
#include "orca_fileio.h"
#include "orca_memmap.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/*
 * Небольшая таблица символов ядра, которые разрешено использовать
 * модулям/приложениям напрямую (в основном — минимальный libc, который
 * может понадобиться сгенерированному компилятором коду). Всё остальное
 * взаимодействие с системой идёт только через orca_api_t.
 */
typedef struct {
    const char* name;
    void*       addr;
} kernel_symbol_t;

/*
 * Хелперы libgcc. Настоящие прототипы у них нестандартные (__aeabi_uldivmod
 * возвращает 128 бит в r0-r3), но здесь нужен только адрес, поэтому
 * объявляем минимально: вызывать их из ядра мы не собираемся.
 * Само упоминание в таблице заставляет линкер втянуть их из libgcc — иначе
 * ядро, которое само 64-битным делением не пользуется, их бы не содержало.
 */
extern void __aeabi_uldivmod(void);
extern void __aeabi_ldivmod(void);
extern void __aeabi_lmul(void);
extern void __aeabi_llsl(void);
extern void __aeabi_llsr(void);
extern void __aeabi_lasr(void);
extern void __aeabi_uidiv(void);
extern void __aeabi_idiv(void);
extern void __aeabi_uidivmod(void);
extern void __aeabi_idivmod(void);
extern void __aeabi_ddiv(void);
extern void __aeabi_dmul(void);
extern void __aeabi_dadd(void);
extern void __aeabi_dsub(void);
extern void __aeabi_d2lz(void);
extern void __aeabi_d2ulz(void);
extern void __aeabi_l2d(void);
extern void __aeabi_ul2d(void);
extern void __aeabi_f2lz(void);
extern void __aeabi_f2ulz(void);
extern void __aeabi_l2f(void);
extern void __aeabi_ul2f(void);

static const kernel_symbol_t s_kernel_symbols[] = {
    { "memcpy",   (void*)memcpy   },
    { "memset",   (void*)memset   },
    { "memmove",  (void*)memmove  },
    { "memcmp",   (void*)memcmp   },
    { "memchr",   (void*)memchr   },
    { "strlen",   (void*)strlen   },
    { "strnlen",  (void*)strnlen  },
    { "strcmp",   (void*)strcmp   },
    { "strncmp",  (void*)strncmp  },
    { "strcpy",   (void*)strcpy   },
    { "strncpy",  (void*)strncpy  },
    { "strcat",   (void*)strcat   },
    { "strchr",   (void*)strchr   },
    { "strrchr",  (void*)strrchr  },
    { "strstr",   (void*)strstr   },
    { "strtol",   (void*)strtol   },
    { "strtoul",  (void*)strtoul  },
    { "snprintf", (void*)snprintf },
    /*
     * Компилятор сам подставляет вызовы __aeabi_* для 64-битного и
     * плавающего деления — без них модуль с одним `%` по uint64 не грузится.
     * Раньше комментарий это обещал, а в таблице стоял один atoi.
     */
    { "atoi",     (void*)atoi     },
    { "__aeabi_uldivmod", (void*)__aeabi_uldivmod },
    { "__aeabi_ldivmod",  (void*)__aeabi_ldivmod  },
    { "__aeabi_lmul",     (void*)__aeabi_lmul     },
    { "__aeabi_llsl",     (void*)__aeabi_llsl     },
    { "__aeabi_llsr",     (void*)__aeabi_llsr     },
    { "__aeabi_lasr",     (void*)__aeabi_lasr     },
    { "__aeabi_uidiv",    (void*)__aeabi_uidiv    },
    { "__aeabi_idiv",     (void*)__aeabi_idiv     },
    { "__aeabi_uidivmod", (void*)__aeabi_uidivmod },
    { "__aeabi_idivmod",  (void*)__aeabi_idivmod  },
    { "__aeabi_ddiv",     (void*)__aeabi_ddiv     },
    { "__aeabi_dmul",     (void*)__aeabi_dmul     },
    { "__aeabi_dadd",     (void*)__aeabi_dadd     },
    { "__aeabi_dsub",     (void*)__aeabi_dsub     },
    { "__aeabi_d2lz",     (void*)__aeabi_d2lz     },
    { "__aeabi_d2ulz",    (void*)__aeabi_d2ulz    },
    { "__aeabi_l2d",      (void*)__aeabi_l2d      },
    { "__aeabi_ul2d",     (void*)__aeabi_ul2d     },
    { "__aeabi_f2lz",     (void*)__aeabi_f2lz     },
    { "__aeabi_f2ulz",    (void*)__aeabi_f2ulz    },
    { "__aeabi_l2f",      (void*)__aeabi_l2f      },
    { "__aeabi_ul2f",     (void*)__aeabi_ul2f     },
};
#define KERNEL_SYMBOL_COUNT (sizeof(s_kernel_symbols) / sizeof(s_kernel_symbols[0]))

#define MAX_SECTIONS 32

typedef struct {
    Elf32_Shdr shdr;
    uint8_t*   load_addr; /* NULL, если секция не грузится в память (не SHF_ALLOC) */
} section_info_t;

static void* resolve_kernel_symbol(const char* name)
{
    for (uint32_t i = 0; i < KERNEL_SYMBOL_COUNT; i++) {
        if (strcmp(s_kernel_symbols[i].name, name) == 0) {
            return s_kernel_symbols[i].addr;
        }
    }
    return NULL;
}

/*
 * R_ARM_THM_CALL (BL) и R_ARM_THM_JUMP24 (B.W) — Thumb-2 ветвления с
 * 25-битным знаковым смещением, размазанным по двум полусловам:
 *
 *   первое:  11110 S imm10
 *   второе:  11 J1 1 J2 imm11        (BL)
 *            10 J1 1 J2 imm11        (B.W)
 *
 *   I1 = ~(J1 ^ S), I2 = ~(J2 ^ S)
 *   offset = SignExtend(S:I1:I2:imm10:imm11:'0')
 *
 * Без этого модуль нельзя собрать обычным способом: любой внешний
 * вызов из модуля даёт именно такую релокацию, и загрузчик раньше отвечал
 * ORCA_LOAD_ERR_UNSUPPORTED_RELOC. Обход через -mlong-calls больше не нужен.
 *
 * Смещение в инструкции считается от P+4 (конвейерный сдвиг PC), но отдельно
 * его вычитать не надо: GAS кладёт в неразрешённую инструкцию (f7ff fffe)
 * адденд -4, то есть сдвиг уже внутри адденда. Проверено сверкой формулы
 * с 1721 реальной инструкцией bl/b.w из orca_kernel.elf: байт-в-байт.
 *
 * use_implicit различает REL и RELA: в REL адденд лежит в самой инструкции,
 * в RELA — в r_addend, и биты иммедиата читать нельзя (остальные типы
 * релокаций ниже делают то же различение через orig).
 */
static bool patch_thumb_branch(uint16_t* p, uint32_t sym_value, uint32_t place,
                               int32_t extra_addend, bool use_implicit)
{
    uint32_t hi = p[0];
    uint32_t lo = p[1];

    uint32_t s     = (hi >> 10) & 0x1u;
    uint32_t imm10 = hi & 0x3FFu;
    uint32_t j1    = (lo >> 13) & 0x1u;
    uint32_t j2    = (lo >> 11) & 0x1u;
    uint32_t imm11 = lo & 0x7FFu;

    uint32_t i1 = 1u - (j1 ^ s);
    uint32_t i2 = 1u - (j2 ^ s);

    int32_t addend = (int32_t)((s << 24) | (i1 << 23) | (i2 << 22) |
                               (imm10 << 12) | (imm11 << 1));
    /* Знаковое расширение с 25 бит. */
    if (addend & 0x01000000) {
        addend -= 0x02000000;
    }
    if (!use_implicit) {
        addend = 0;
    }
    addend += extra_addend;

    /* Тумб-бит адреса функции не участвует в арифметике смещения. */
    int32_t offset = (int32_t)(sym_value & ~ELF_THUMB_BIT) + addend - (int32_t)place;

    if (offset < -16777216 || offset > 16777214 || (offset & 1)) {
        return false;
    }

    uint32_t v = (uint32_t)offset;
    s     = (v >> 24) & 0x1u;
    i1    = (v >> 23) & 0x1u;
    i2    = (v >> 22) & 0x1u;
    imm10 = (v >> 12) & 0x3FFu;
    imm11 = (v >> 1) & 0x7FFu;
    j1    = 1u - (i1 ^ s);
    j2    = 1u - (i2 ^ s);

    p[0] = (uint16_t)((hi & 0xF800u) | (s << 10) | imm10);
    p[1] = (uint16_t)((lo & 0xD000u) | (j1 << 13) | (j2 << 11) | imm11);
    return true;
}

static bool read_at(orca_file_t* f, uint32_t offset, void* buf, uint32_t len)
{
    if (!orca_fileio_seek(f, offset)) {
        return false;
    }
    return orca_fileio_read(f, buf, len) == (int32_t)len;
}

static orca_load_status_t loader_load_locked(const char* path,
                                             const orca_api_t* api,
                                             orca_loaded_module_t* out)
{
    (void)api; /* модуль получает api не через relocations, а как аргумент orca_app_main */

    memset(out, 0, sizeof(*out));

    orca_file_t* f = orca_fileio_open(path, false);
    if (f == NULL) {
        return ORCA_LOAD_ERR_IO;
    }

    Elf32_Ehdr ehdr;
    if (!read_at(f, 0, &ehdr, sizeof(ehdr))) {
        orca_fileio_close(f);
        return ORCA_LOAD_ERR_IO;
    }

    if (ehdr.e_ident[0] != ELFMAG0 || ehdr.e_ident[1] != ELFMAG1 ||
        ehdr.e_ident[2] != ELFMAG2 || ehdr.e_ident[3] != ELFMAG3 ||
        ehdr.e_type != ET_REL || ehdr.e_machine != EM_ARM) {
        orca_fileio_close(f);
        return ORCA_LOAD_ERR_BAD_ELF;
    }

    if (ehdr.e_shnum == 0 || ehdr.e_shnum > MAX_SECTIONS) {
        orca_fileio_close(f);
        return ORCA_LOAD_ERR_BAD_ELF;
    }

    static section_info_t sections[MAX_SECTIONS];
    memset(sections, 0, sizeof(sections));

    for (uint32_t i = 0; i < ehdr.e_shnum; i++) {
        uint32_t off = ehdr.e_shoff + i * sizeof(Elf32_Shdr);
        if (!read_at(f, off, &sections[i].shdr, sizeof(Elf32_Shdr))) {
            orca_fileio_close(f);
            return ORCA_LOAD_ERR_IO;
        }
    }

    /* 1. Считаем суммарный размер всех ALLOC-секций и выделяем единый блок в арене. */
    uint32_t total_size = 0;
    uint32_t max_align = 4;
    for (uint32_t i = 0; i < ehdr.e_shnum; i++) {
        Elf32_Shdr* s = &sections[i].shdr;
        if (!(s->sh_flags & SHF_ALLOC)) {
            continue;
        }

        uint32_t align = s->sh_addralign ? s->sh_addralign : 4;
        /*
         * sh_addralign приходит из файла и используется как маска
         * ~(align - 1). Не степень двойки превращала маску в мусор и
         * уводила cursor назад по образу — секции наложились бы друг на
         * друга. Заодно ограничиваем сверху, чтобы выравнивание само по
         * себе не съело весь лимит образа.
         */
        if ((align & (align - 1u)) != 0u || align > 4096u) {
            orca_fileio_close(f);
            return ORCA_LOAD_ERR_BAD_ELF;
        }

        /*
         * Проверяем лимит на каждом шаге, а не в конце: сумма sh_size из
         * битого файла легко заворачивалась за 2^32, итог проходил проверку
         * "<= ORCA_APP_MAX_IMAGE_SIZE", и дальше секции писались за пределы
         * выделенного блока.
         */
        total_size = (uint32_t)((total_size + align - 1u) & ~(align - 1u));
        if (s->sh_size > ORCA_APP_MAX_IMAGE_SIZE ||
            total_size > ORCA_APP_MAX_IMAGE_SIZE - s->sh_size) {
            orca_fileio_close(f);
            return ORCA_LOAD_ERR_BAD_ELF;
        }
        total_size += s->sh_size;

        if (align > max_align) {
            max_align = align;
        }
    }

    if (total_size == 0) {
        orca_fileio_close(f);
        return ORCA_LOAD_ERR_BAD_ELF;
    }

    uint8_t* image = (uint8_t*)orca_arena_alloc(total_size, max_align);
    if (image == NULL) {
        orca_fileio_close(f);
        return ORCA_LOAD_ERR_NO_MEMORY;
    }
    memset(image, 0, total_size);

    /* 2. Раскладываем ALLOC-секции по image и грузим содержимое PROGBITS. */
    uint32_t cursor = 0;
    for (uint32_t i = 0; i < ehdr.e_shnum; i++) {
        Elf32_Shdr* s = &sections[i].shdr;
        if (!(s->sh_flags & SHF_ALLOC)) {
            continue;
        }
        uint32_t align = s->sh_addralign ? s->sh_addralign : 4;
        cursor = (cursor + align - 1) & ~(align - 1);
        sections[i].load_addr = image + cursor;

        if (s->sh_type == SHT_PROGBITS && s->sh_size > 0) {
            if (!read_at(f, s->sh_offset, sections[i].load_addr, s->sh_size)) {
                orca_arena_free(image);
                orca_fileio_close(f);
                return ORCA_LOAD_ERR_IO;
            }
        }
        /* SHT_NOBITS (.bss) уже занулена memset выше */
        cursor += s->sh_size;
    }

    /* 3. Находим .symtab/.strtab. */
    int32_t symtab_idx = -1, strtab_idx = -1;
    for (uint32_t i = 0; i < ehdr.e_shnum; i++) {
        if (sections[i].shdr.sh_type == SHT_SYMTAB) {
            symtab_idx = (int32_t)i;
            strtab_idx = (int32_t)sections[i].shdr.sh_link;
        }
    }
    /*
     * strtab_idx пришёл из sh_link, то есть из файла: без проверки границы
     * sections[strtab_idx] читался бы за пределами массива.
     */
    if (symtab_idx < 0 || strtab_idx < 0 || strtab_idx >= (int32_t)ehdr.e_shnum ||
        sections[strtab_idx].shdr.sh_type != SHT_STRTAB) {
        orca_arena_free(image);
        orca_fileio_close(f);
        return ORCA_LOAD_ERR_BAD_ELF;
    }

    uint32_t sym_count = sections[symtab_idx].shdr.sh_size / sizeof(Elf32_Sym);
    static Elf32_Sym symtab_buf[256];
    if (sym_count > 256) {
        orca_arena_free(image);
        orca_fileio_close(f);
        return ORCA_LOAD_ERR_BAD_ELF;
    }
    if (!read_at(f, sections[symtab_idx].shdr.sh_offset, symtab_buf,
                 sym_count * sizeof(Elf32_Sym))) {
        orca_arena_free(image);
        orca_fileio_close(f);
        return ORCA_LOAD_ERR_IO;
    }

    static char strtab_buf[2048];
    uint32_t strtab_size = sections[strtab_idx].shdr.sh_size;
    if (strtab_size == 0 || strtab_size > sizeof(strtab_buf)) {
        orca_arena_free(image);
        orca_fileio_close(f);
        return ORCA_LOAD_ERR_BAD_ELF;
    }
    if (!read_at(f, sections[strtab_idx].shdr.sh_offset, strtab_buf, strtab_size)) {
        orca_arena_free(image);
        orca_fileio_close(f);
        return ORCA_LOAD_ERR_IO;
    }
    /*
     * Модуль приходит с SD, то есть из недоверенного источника. У валидного
     * .strtab последний байт нулевой, у битого может не быть — тогда strcmp
     * по имени символа уходит за массив. Ставим терминатор сами.
     */
    strtab_buf[strtab_size - 1] = '\0';

    /* Вычисляем финальный адрес каждого символа. */
    static uint32_t sym_addr[256];
    for (uint32_t i = 0; i < sym_count; i++) {
        Elf32_Sym* sym = &symtab_buf[i];
        if (sym->st_shndx == 0 /* SHN_UNDEF */) {
            /* st_name — индекс в .strtab, из файла и потому недоверенный. */
            if (sym->st_name >= strtab_size) {
                orca_arena_free(image);
                orca_fileio_close(f);
                return ORCA_LOAD_ERR_BAD_ELF;
            }
            const char* name = &strtab_buf[sym->st_name];
            void* resolved = resolve_kernel_symbol(name);
            if (resolved == NULL) {
                orca_arena_free(image);
                orca_fileio_close(f);
                return ORCA_LOAD_ERR_UNDEFINED_SYMBOL;
            }
            sym_addr[i] = (uint32_t)(uintptr_t)resolved;
        } else if (sym->st_shndx < ehdr.e_shnum && sections[sym->st_shndx].load_addr != NULL) {
            sym_addr[i] = (uint32_t)(uintptr_t)sections[sym->st_shndx].load_addr + sym->st_value;
        } else {
            sym_addr[i] = sym->st_value;
        }
    }

    /* 4. Применяем релокации для каждой секции REL/RELA. */
    for (uint32_t i = 0; i < ehdr.e_shnum; i++) {
        Elf32_Shdr* s = &sections[i].shdr;
        if (s->sh_type != SHT_REL && s->sh_type != SHT_RELA) {
            continue;
        }
        uint32_t target_idx = s->sh_info;
        if (target_idx >= ehdr.e_shnum || sections[target_idx].load_addr == NULL) {
            continue;
        }
        uint8_t* target_base = sections[target_idx].load_addr;
        uint32_t entsize = (s->sh_type == SHT_RELA) ? sizeof(Elf32_Rela) : sizeof(Elf32_Rel);
        uint32_t count = s->sh_size / entsize;

        for (uint32_t r = 0; r < count; r++) {
            uint32_t off = s->sh_offset + r * entsize;
            uint32_t r_offset, r_info;
            int32_t  r_addend = 0;

            if (s->sh_type == SHT_RELA) {
                Elf32_Rela rela;
                if (!read_at(f, off, &rela, sizeof(rela))) {
                    orca_arena_free(image);
                    orca_fileio_close(f);
                    return ORCA_LOAD_ERR_IO;
                }
                r_offset = rela.r_offset;
                r_info   = rela.r_info;
                r_addend = rela.r_addend;
            } else {
                Elf32_Rel rel;
                if (!read_at(f, off, &rel, sizeof(rel))) {
                    orca_arena_free(image);
                    orca_fileio_close(f);
                    return ORCA_LOAD_ERR_IO;
                }
                r_offset = rel.r_offset;
                r_info   = rel.r_info;
            }

            uint32_t sym_idx = ELF32_R_SYM(r_info);
            uint32_t type    = ELF32_R_TYPE(r_info);
            if (sym_idx >= sym_count) {
                orca_arena_free(image);
                orca_fileio_close(f);
                return ORCA_LOAD_ERR_BAD_ELF;
            }

            /*
             * r_offset приходит из файла и указывает, куда писать. Без этой
             * проверки битый (или намеренно собранный) .orca патчит любую
             * память ядра мимо арены — молча и до первого HardFault.
             */
            uint32_t patch_size = 4u;
            if (r_offset > sections[target_idx].shdr.sh_size ||
                sections[target_idx].shdr.sh_size - r_offset < patch_size) {
                orca_arena_free(image);
                orca_fileio_close(f);
                return ORCA_LOAD_ERR_BAD_ELF;
            }

            uint32_t sym_value = sym_addr[sym_idx];
            uint32_t* patch_addr = (uint32_t*)(void*)(target_base + r_offset);

            switch (type) {
                case R_ARM_NONE:
                    break;

                case R_ARM_ABS32:
                case R_ARM_TARGET1: {
                    uint32_t orig = (s->sh_type == SHT_RELA) ? 0 : *patch_addr;
                    *patch_addr = sym_value + (uint32_t)r_addend + orig;
                    break;
                }
                case R_ARM_REL32:
                case R_ARM_PREL31: {
                    uint32_t orig = (s->sh_type == SHT_RELA) ? 0 : *patch_addr;
                    uint32_t rel  = sym_value + (uint32_t)r_addend + orig -
                                    (uint32_t)(uintptr_t)patch_addr;
                    if (type == R_ARM_PREL31) {
                        /* 31-битное смещение, старший бит — флаг, его не трогаем */
                        *patch_addr = (*patch_addr & 0x80000000u) | (rel & 0x7FFFFFFFu);
                    } else {
                        *patch_addr = rel;
                    }
                    break;
                }
                case R_ARM_THM_CALL:
                case R_ARM_THM_JUMP24: {
                    if (!patch_thumb_branch((uint16_t*)(void*)patch_addr,
                                            sym_value,
                                            (uint32_t)(uintptr_t)patch_addr,
                                            r_addend,
                                            s->sh_type != SHT_RELA)) {
                        orca_arena_free(image);
                        orca_fileio_close(f);
                        return ORCA_LOAD_ERR_UNSUPPORTED_RELOC;
                    }
                    break;
                }
                default:
                    orca_arena_free(image);
                    orca_fileio_close(f);
                    return ORCA_LOAD_ERR_UNSUPPORTED_RELOC;
            }
        }
    }

    /* 5. Находим точки входа среди определённых символов. */
    void* main_addr = NULL;
    void* stop_addr = NULL;
    void* cmd_addr  = NULL;
    for (uint32_t i = 0; i < sym_count; i++) {
        Elf32_Sym* sym = &symtab_buf[i];
        if (sym->st_shndx == 0 || sym->st_name >= strtab_size) {
            continue;
        }
        const char* name = &strtab_buf[sym->st_name];
        if (strcmp(name, ORCA_APP_MAIN_SYMBOL) == 0) {
            main_addr = (void*)(uintptr_t)sym_addr[i];
        } else if (strcmp(name, ORCA_APP_STOP_SYMBOL) == 0) {
            stop_addr = (void*)(uintptr_t)sym_addr[i];
        } else if (strcmp(name, ORCA_CMD_MAIN_SYMBOL) == 0) {
            cmd_addr = (void*)(uintptr_t)sym_addr[i];
        }
    }

    orca_fileio_close(f);

    /*
     * Команда (orca_cmd_main) и долгоживущий модуль (orca_app_main) —
     * разные контракты, но образ один. Достаточно любой из точек входа.
     */
    if (main_addr == NULL && cmd_addr == NULL) {
        orca_arena_free(image);
        return ORCA_LOAD_ERR_NO_ENTRY;
    }

    /*
     * Код только что записан обычными store'ами и лежит в D-cache.
     * Перед первым вызовом main_fn его надо синхронизировать с памятью
     * и сбросить I-cache, иначе M7 исполнит устаревшие инструкции.
     */
    orca_loader_cache_sync(image, total_size);

    out->image_base = image;
    out->image_size = total_size;
    out->main_fn = (orca_app_main_fn)(uintptr_t)main_addr;
    out->stop_fn = (orca_app_stop_fn)(uintptr_t)stop_addr;
    out->cmd_fn  = (orca_cmd_main_fn)(uintptr_t)cmd_addr;

    const char* base_name = path;
    for (const char* p = path; *p != '\0'; p++) {
        if (*p == '/') {
            base_name = p + 1;
        }
    }
    strncpy(out->name, base_name, sizeof(out->name) - 1);

    return ORCA_LOAD_OK;
}

/*
 * Разбор ELF держит таблицы секций и символов в статических буферах: под
 * 32 секции, 256 символов и 2 КБ строк на стеке задачи места нет. Значит,
 * два одновременных разбора невозможны — а они реальны: команда с SD
 * грузится в задаче Link, а `app start` может прийти из другого контекста,
 * и второй загрузчик затирал sections/symtab первому прямо под руками.
 * Поэтому вход в разбор сериализован мьютексом.
 */
static SemaphoreHandle_t s_load_lock;

void orca_loader_init(void)
{
    if (s_load_lock == NULL) {
        s_load_lock = xSemaphoreCreateMutex();
    }
}

orca_load_status_t orca_loader_load(const char* path,
                                    const orca_api_t* api,
                                    orca_loaded_module_t* out)
{
    if (path == NULL || out == NULL) {
        return ORCA_LOAD_ERR_BAD_ELF;
    }
    if (s_load_lock == NULL) {
        return ORCA_LOAD_ERR_BUSY;
    }

    /*
     * Ждём недолго и с конечным таймаутом: загрузка идёт в задаче shell'а,
     * и лучше честно сказать "занято", чем подвесить консоль.
     */
    if (xSemaphoreTake(s_load_lock, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return ORCA_LOAD_ERR_BUSY;
    }

    orca_load_status_t status = loader_load_locked(path, api, out);

    xSemaphoreGive(s_load_lock);
    return status;
}

void orca_loader_unload(orca_loaded_module_t* mod)
{
    if (mod == NULL || mod->image_base == NULL) {
        return;
    }
    orca_arena_free(mod->image_base);
    memset(mod, 0, sizeof(*mod));
}
