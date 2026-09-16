#!/usr/bin/env python3
"""
Упаковщик и валидатор модулей Orca (.orca).

Зачем он нужен. Образ .orca — это обычный relocatable ELF32 (ET_REL), то есть
"упаковка" сводится к переименованию. Ценность инструмента не в этом, а в
проверках: у загрузчика на устройстве (firmware/OrcaKernel/loader/orca_loader.c)
жёсткие статические лимиты — 32 секции, 256 символов, 2 КБ .strtab, семь типов
релокаций и закрытый список символов ядра. Нарушение любого из них выясняется
на плате, где вся диагностика — один код ошибки в консоли и никакого указания
на то, какой символ или какая секция виноваты.

Поэтому здесь воспроизведены ровно те же проверки, но с внятными сообщениями.
Лимиты и списки не продублированы константами, а вычитываются из исходников
ядра (см. KernelSpec): дублирующая таблица символов разошлась бы с
orca_loader.c на первой же правке, и инструмент начал бы врать.

Вторая половина работы — манифест. Ядро разбирает его своим минимальным
сканером (orca_manifest.c) с буферами фиксированного размера, и слишком
длинное имя там молча усекается. Лучше сказать об этом на ПК.

CRC32 — тот же полином, что у zlib (0xEDB88320), это и есть zlib.crc32.

Использование:
    # проверить образ и манифест, записать crc32 в манифест
    elf2orca.py pack build/my_module.orca --manifest module.json

    # собрать каталог модуля целиком: образ + манифест рядом
    elf2orca.py pack build/my_module.elf -o sdcard_root/modules/my_module/

    # проверить то, что уже лежит на карте (crc32 сверяется, не переписывается)
    elf2orca.py check sdcard_root/modules/esp_link/module.json

Код возврата: 0 — годно, 1 — есть ошибки, 2 — ошибка использования.
"""

import argparse
import json
import os
import re
import shutil
import struct
import sys
import zlib

# ------------------------------------------------------------------ #
# Лимиты ядра                                                        #
# ------------------------------------------------------------------ #

# Значения по умолчанию на случай, если исходники ядра рядом не найдены
# (инструмент скопировали отдельно). Держатся синхронными с orca_loader.c.
DEFAULTS = {
    "max_sections": 32,      # MAX_SECTIONS, orca_loader.c
    "max_symbols": 256,      # размер symtab_buf[], orca_loader.c
    "max_strtab": 2048,      # размер strtab_buf[], orca_loader.c
    "max_align": 4096,       # проверка sh_addralign, orca_loader.c
    "max_image": 512 * 1024,  # ORCA_APP_MAX_IMAGE_SIZE, orca_memmap.h
    "default_stack": 16 * 1024,  # ORCA_APP_DEFAULT_STACK, orca_memmap.h
    "api_version": 3,        # ORCA_API_VERSION, orca_api.h
    "min_stack_words": 128,  # configMINIMAL_STACK_SIZE, FreeRTOSConfig.h
    "manifest_max": 1024,    # ORCA_MANIFEST_FILE_MAX, orca_manifest.h
}

# Релокации, которые orca_loader умеет применять. Всё остальное на плате
# даёт ORCA_LOAD_ERR_UNSUPPORTED_RELOC без указания типа.
RELOC_NAMES = {
    0: "R_ARM_NONE",
    2: "R_ARM_ABS32",
    3: "R_ARM_REL32",
    10: "R_ARM_THM_CALL",
    30: "R_ARM_THM_JUMP24",
    38: "R_ARM_TARGET1",
    42: "R_ARM_PREL31",
}

# Типы, которые встречаются в реальных поломках. Голый номер релокации
# ничего не говорит, а причина у каждого из них одна и та же и известна.
RELOC_HINTS = {
    47: "R_ARM_THM_MOVW_ABS_NC: адрес собран парой movw/movt вместо литерала. "
        "Уберите -mslow-flash-data / -mpure-code — загрузчик умеет чинить "
        "только литеральные пулы (R_ARM_ABS32)",
    48: "R_ARM_THM_MOVT_ABS: то же, что и MOVW выше",
    40: "R_ARM_MOVW_ABS_NC (ARM-режим): модуль собран не в Thumb, "
        "добавьте -mthumb",
    41: "R_ARM_MOVT_ABS (ARM-режим): модуль собран не в Thumb, добавьте -mthumb",
    28: "R_ARM_CALL (ARM-режим): модуль собран не в Thumb, добавьте -mthumb",
    29: "R_ARM_JUMP24 (ARM-режим): модуль собран не в Thumb, добавьте -mthumb",
    25: "R_ARM_GOT_BREL: модуль собран с -fPIC, а GOT загрузчик не строит — "
        "уберите -fPIC",
    26: "R_ARM_PLT32: вызов через PLT; уберите -fPIC/-fPIE",
    43: "R_ARM_TLS_*: thread-local storage в модулях не поддерживается",
}

# Поля манифеста: имя -> размер буфера в ядре (orca_manifest.h).
# Ядро усекает до size-1 символов молча, поэтому проверяем здесь.
STRING_FIELDS = {
    "name": 32,
    "version": 16,
    "entry": 64,
    "author": 32,
    "description": 96,
    "icon": 32,
}

MODULE_TYPES = ("app", "module", "command")

# Точки входа (orca_api.h). Достаточно любой из main/cmd.
SYM_APP_MAIN = "orca_app_main"
SYM_APP_STOP = "orca_app_stop"
SYM_CMD_MAIN = "orca_cmd_main"

ET_REL = 1
EM_ARM = 40
SHT_PROGBITS = 1
SHT_SYMTAB = 2
SHT_STRTAB = 3
SHT_RELA = 4
SHT_NOBITS = 8
SHT_REL = 9
SHF_ALLOC = 0x2
SHF_EXECINSTR = 0x4


class KernelSpec:
    """
    Лимиты и списки, вычитанные из исходников ядра.

    Разбор регулярками, а не полноценным парсером C: нужны девять чисел и два
    списка строк, и все они записаны в предсказуемой форме. Если что-то не
    нашлось — берётся значение из DEFAULTS, а соответствующая проверка либо
    работает на умолчании, либо (для списка символов) пропускается с
    предупреждением: молча разрешить всё хуже, чем сказать, что не проверили.
    """

    def __init__(self, root=None):
        self.root = root or find_repo_root()
        self.limits = dict(DEFAULTS)
        self.kernel_symbols = None
        self.perm_names = None
        self.source_seen = False
        if self.root:
            self._load()

    def _read(self, rel):
        path = os.path.join(self.root, rel)
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                return f.read()
        except OSError:
            return None

    @staticmethod
    def _define(text, name):
        """Значение #define как целое; понимает 0x, суффикс u и умножение."""
        m = re.search(r"#define\s+%s\s+(.+)" % re.escape(name), text)
        if not m:
            return None
        expr = m.group(1).split("/*")[0].split("//")[0].strip()
        # Убираем суффиксы u/U/l/L у чисел и приводимые типы вида ((uint16_t)128).
        expr = re.sub(r"\(\s*(?:uint|int)\d+_t\s*\)", "", expr)
        expr = re.sub(r"\(\s*size_t\s*\)", "", expr)
        expr = re.sub(r"\b(0x[0-9a-fA-F]+|\d+)[uUlL]+\b", r"\1", expr)
        if not re.fullmatch(r"[0-9xa-fA-F\s()*+\-]+", expr):
            return None
        try:
            return int(eval(expr, {"__builtins__": {}}, {}))  # noqa: S307
        except Exception:
            return None

    def _load(self):
        loader = self._read("firmware/OrcaKernel/loader/orca_loader.c")
        if loader:
            self.source_seen = True
            v = self._define(loader, "MAX_SECTIONS")
            if v:
                self.limits["max_sections"] = v
            # symtab_buf[256] / strtab_buf[2048] — размеры записаны литералами.
            m = re.search(r"Elf32_Sym\s+symtab_buf\[(\d+)\]", loader)
            if m:
                self.limits["max_symbols"] = int(m.group(1))
            m = re.search(r"char\s+strtab_buf\[(\d+)\]", loader)
            if m:
                self.limits["max_strtab"] = int(m.group(1))
            m = re.search(r"align\s*>\s*(\d+)u", loader)
            if m:
                self.limits["max_align"] = int(m.group(1))
            syms = re.findall(r'\{\s*"([^"]+)"\s*,\s*\(void\*\)', loader)
            if syms:
                self.kernel_symbols = set(syms)

        memmap = self._read("firmware/OrcaKernel/api/orca_memmap.h")
        if memmap:
            for key, macro in (("max_image", "ORCA_APP_MAX_IMAGE_SIZE"),
                               ("default_stack", "ORCA_APP_DEFAULT_STACK")):
                v = self._define(memmap, macro)
                if v:
                    self.limits[key] = v

        api = self._read("firmware/OrcaKernel/api/orca_api.h")
        if api:
            v = self._define(api, "ORCA_API_VERSION")
            if v:
                self.limits["api_version"] = v

        man_h = self._read("firmware/OrcaKernel/services/orca_manifest.h")
        if man_h:
            v = self._define(man_h, "ORCA_MANIFEST_FILE_MAX")
            if v:
                self.limits["manifest_max"] = v

        man_c = self._read("firmware/OrcaKernel/services/orca_manifest.c")
        if man_c:
            block = re.search(r"s_perms\[\]\s*=\s*\{(.*?)\};", man_c, re.S)
            if block:
                names = re.findall(r'\{\s*"([^"]+)"', block.group(1))
                if names:
                    self.perm_names = names

        cfg = self._read("firmware/Core/Inc/FreeRTOSConfig.h")
        if cfg:
            v = self._define(cfg, "configMINIMAL_STACK_SIZE")
            if v:
                self.limits["min_stack_words"] = v


def find_repo_root():
    """
    Корень репозитория — ближайший вверх каталог, где есть firmware/OrcaKernel.
    Сначала от расположения скрипта, потом от текущего каталога: инструмент
    может лежать и вне дерева (скопировали в ~/bin), а запускаться из него.
    """
    for start in (os.path.dirname(os.path.abspath(__file__)), os.getcwd()):
        path = start
        while True:
            if os.path.isdir(os.path.join(path, "firmware", "OrcaKernel")):
                return path
            parent = os.path.dirname(path)
            if parent == path:
                break
            path = parent
    return None


# ------------------------------------------------------------------ #
# Отчёт                                                              #
# ------------------------------------------------------------------ #

class Report:
    """
    Накопитель замечаний. Проверки не прерываются на первой ошибке: у модуля,
    собранного не тем компилятором, их обычно десяток, и показывать их по
    одной за сборку — издевательство.
    """

    def __init__(self, quiet=False):
        self.errors = []
        self.warnings = []
        self.notes = []
        self.quiet = quiet

    def error(self, msg):
        self.errors.append(msg)

    def warn(self, msg):
        self.warnings.append(msg)

    def note(self, msg):
        self.notes.append(msg)

    @property
    def ok(self):
        return not self.errors

    def dump(self, stream=sys.stderr):
        if not self.quiet:
            for m in self.notes:
                print("  %s" % m, file=stream)
        for m in self.warnings:
            print("warning: %s" % m, file=stream)
        for m in self.errors:
            print("error: %s" % m, file=stream)


# ------------------------------------------------------------------ #
# Разбор ELF                                                         #
# ------------------------------------------------------------------ #

class Section:
    __slots__ = ("index", "name", "_name_off", "type", "flags", "addr",
                 "offset", "size", "link", "info", "align", "entsize")

    def __init__(self, index, fields):
        (name, self.type, self.flags, self.addr, self.offset, self.size,
         self.link, self.info, self.align, self.entsize) = fields
        self.index = index
        self._name_off = name
        self.name = ""


class Symbol:
    __slots__ = ("index", "name", "value", "size", "info", "other", "shndx")

    def __init__(self, index, fields, name):
        (_, self.value, self.size, self.info, self.other, self.shndx) = fields
        self.index = index
        self.name = name


class ElfImage:
    """Ровно тот разбор, который делает orca_loader — не больше."""

    def __init__(self, data, path="<memory>"):
        self.data = data
        self.path = path
        self.sections = []
        self.symbols = []
        self.strtab = b""
        self.valid_header = False
        self._parse()

    def _parse(self):
        if len(self.data) < 52:
            return
        hdr = struct.unpack_from("<16sHHIIIIIHHHHHH", self.data, 0)
        (self.e_ident, self.e_type, self.e_machine, self.e_version,
         self.e_entry, self.e_phoff, self.e_shoff, self.e_flags,
         self.e_ehsize, self.e_phentsize, self.e_phnum, self.e_shentsize,
         self.e_shnum, self.e_shstrndx) = hdr
        if self.e_ident[:4] != b"\x7fELF":
            return
        self.valid_header = True

        end = self.e_shoff + self.e_shnum * 40
        if self.e_shoff == 0 or end > len(self.data):
            return

        for i in range(self.e_shnum):
            fields = struct.unpack_from("<10I", self.data, self.e_shoff + i * 40)
            self.sections.append(Section(i, fields))

        # Имена секций — только для сообщений, поэтому ошибки здесь не фатальны.
        if self.e_shstrndx < len(self.sections):
            shstr = self._blob(self.sections[self.e_shstrndx])
            for s in self.sections:
                s.name = cstr(shstr, s._name_off)

        symtab = next((s for s in self.sections if s.type == SHT_SYMTAB), None)
        if symtab is None:
            return
        self.symtab = symtab
        if symtab.link >= len(self.sections):
            return
        strtab_sec = self.sections[symtab.link]
        if strtab_sec.type != SHT_STRTAB:
            return
        self.strtab = self._blob(strtab_sec)
        self.strtab_section = strtab_sec

        blob = self._blob(symtab)
        for i in range(len(blob) // 16):
            fields = struct.unpack_from("<IIIBBH", blob, i * 16)
            self.symbols.append(Symbol(i, fields, cstr(self.strtab, fields[0])))

    def _blob(self, sec):
        if sec.type == SHT_NOBITS:
            return b""
        start, end = sec.offset, sec.offset + sec.size
        if start > len(self.data) or end > len(self.data):
            return b""
        return self.data[start:end]

    def alloc_sections(self):
        return [s for s in self.sections if s.flags & SHF_ALLOC]

    def relocations(self):
        """(секция-владелец, тип, r_offset, индекс символа) по всем REL/RELA."""
        for s in self.sections:
            if s.type not in (SHT_REL, SHT_RELA):
                continue
            entsize = 12 if s.type == SHT_RELA else 8
            blob = self._blob(s)
            for r in range(len(blob) // entsize):
                r_offset, r_info = struct.unpack_from("<II", blob, r * entsize)
                yield s, r_info & 0xFF, r_offset, r_info >> 8


def cstr(blob, offset):
    if offset >= len(blob):
        return ""
    end = blob.find(b"\0", offset)
    if end < 0:
        end = len(blob)
    return blob[offset:end].decode("utf-8", "replace")


# ------------------------------------------------------------------ #
# Проверка образа                                                    #
# ------------------------------------------------------------------ #

def check_elf(elf, spec, rep):
    """
    Повторяет путь loader_load_locked(): каждая проверка здесь соответствует
    одному `return ORCA_LOAD_ERR_*` в orca_loader.c.
    """
    lim = spec.limits

    if not elf.valid_header:
        rep.error("%s: не ELF (нет сигнатуры \\x7fELF)" % elf.path)
        return None

    if elf.e_type != ET_REL:
        rep.error("e_type=%d, нужен ET_REL(1): модуль должен линковаться с -r, "
                  "иначе загрузчику нечего релоцировать" % elf.e_type)
    if elf.e_machine != EM_ARM:
        rep.error("e_machine=%d, нужен EM_ARM(40)" % elf.e_machine)

    if elf.e_shnum == 0:
        rep.error("нет таблицы секций")
        return None
    if elf.e_shnum > lim["max_sections"]:
        rep.error("секций %d, загрузчик берёт не больше %d "
                  "(MAX_SECTIONS); попробуйте -ffunction-sections без -r "
                  "или объедините секции в module.ld"
                  % (elf.e_shnum, lim["max_sections"]))

    # Раскладка ALLOC-секций — арифметика один в один с загрузчиком.
    total = 0
    max_align = 4
    for s in elf.alloc_sections():
        align = s.align or 4
        if align & (align - 1):
            rep.error("секция '%s': sh_addralign=%d не степень двойки"
                      % (s.name, align))
            continue
        if align > lim["max_align"]:
            rep.error("секция '%s': выравнивание %d больше предела %d"
                      % (s.name, align, lim["max_align"]))
            continue
        total = (total + align - 1) & ~(align - 1)
        total += s.size
        max_align = max(max_align, align)

    if total == 0:
        rep.error("нет ни одной загружаемой секции (SHF_ALLOC): образ пуст")
    elif total > lim["max_image"]:
        rep.error("образ в памяти %d Б, предел ORCA_APP_MAX_IMAGE_SIZE = %d Б"
                  % (total, lim["max_image"]))

    if not elf.symbols:
        rep.error("нет .symtab/.strtab — загрузчик не сможет найти точку входа")
        return None

    if len(elf.symbols) > lim["max_symbols"]:
        rep.error("символов %d, загрузчик берёт не больше %d; "
                  "снимите отладочные символы (-g0) или уберите static-имена"
                  % (len(elf.symbols), lim["max_symbols"]))

    strtab_size = len(elf.strtab)
    if strtab_size == 0:
        rep.error(".strtab пуст")
    elif strtab_size > lim["max_strtab"]:
        rep.error(".strtab %d Б, предел %d Б; длинные имена символов "
                  "съедают его быстрее всего" % (strtab_size, lim["max_strtab"]))

    # Неразрешённые внешние символы: на плате это ORCA_LOAD_ERR_UNDEFINED_SYMBOL
    # без имени виновника, что делает ошибку почти неотлаживаемой.
    undefined = [s.name for s in elf.symbols if s.shndx == 0 and s.name]
    if spec.kernel_symbols is None:
        if undefined:
            rep.warn("список символов ядра не найден (нет orca_loader.c рядом) — "
                     "%d внешних символов не проверены" % len(undefined))
    else:
        missing = sorted({n for n in undefined if n not in spec.kernel_symbols})
        for name in missing:
            rep.error("внешний символ '%s' не экспортируется ядром; всё, кроме "
                      "минимального libc, доступно только через orca_api_t" % name)

    # Типы релокаций.
    bad = {}
    for sec, rtype, _off, _sym in elf.relocations():
        if rtype not in RELOC_NAMES:
            bad.setdefault(rtype, set()).add(sec.name)
    for rtype, where in sorted(bad.items()):
        hint = RELOC_HINTS.get(rtype)
        rep.error("релокация типа %d в %s не поддерживается загрузчиком%s"
                  % (rtype, ", ".join(sorted(where)),
                     "; " + hint if hint else ""))

    # r_offset вне целевой секции: загрузчик это ловит, но молча.
    for sec, _rtype, r_offset, _sym in elf.relocations():
        if sec.info >= len(elf.sections):
            continue
        target = elf.sections[sec.info]
        if not (target.flags & SHF_ALLOC):
            continue
        if r_offset + 4 > target.size:
            rep.error("релокация в '%s' указывает за пределы секции "
                      "(offset=%d, size=%d) — образ повреждён"
                      % (target.name, r_offset, target.size))
            break

    # Точки входа.
    defined = {s.name for s in elf.symbols if s.shndx != 0 and s.name}
    has_main = SYM_APP_MAIN in defined
    has_cmd = SYM_CMD_MAIN in defined
    if not has_main and not has_cmd:
        rep.error("нет ни %s, ни %s: загрузчик вернёт ORCA_LOAD_ERR_NO_ENTRY"
                  % (SYM_APP_MAIN, SYM_CMD_MAIN))
    if has_main and has_cmd:
        rep.warn("определены и %s, и %s — ядро запустит модуль как задачу, "
                 "а команда останется недостижимой" % (SYM_APP_MAIN, SYM_CMD_MAIN))
    if SYM_APP_STOP in defined and not has_main:
        rep.warn("%s без %s: останавливать нечего" % (SYM_APP_STOP, SYM_APP_MAIN))

    kind = "command" if (has_cmd and not has_main) else "module"
    text = sum(s.size for s in elf.alloc_sections() if s.flags & SHF_EXECINSTR)
    bss = sum(s.size for s in elf.alloc_sections() if s.type == SHT_NOBITS)
    rep.note("образ: %d Б в памяти (код %d, bss %d), символов %d, "
             ".strtab %d Б, выравнивание %d"
             % (total, text, bss, len(elf.symbols), strtab_size, max_align))
    return {"image_size": total, "kind": kind, "entries": sorted(
        n for n in (SYM_APP_MAIN, SYM_APP_STOP, SYM_CMD_MAIN) if n in defined)}


# ------------------------------------------------------------------ #
# Проверка манифеста                                                 #
# ------------------------------------------------------------------ #

def check_manifest(man, raw_len, spec, rep, elf_info=None):
    lim = spec.limits

    if raw_len > lim["manifest_max"]:
        rep.error("манифест %d Б, ядро читает не больше %d Б "
                  "(ORCA_MANIFEST_FILE_MAX) и откажется его разбирать"
                  % (raw_len, lim["manifest_max"]))

    if not isinstance(man, dict):
        rep.error("манифест должен быть JSON-объектом")
        return

    for key in man:
        # Ядро читает ключ в char key[24]; более длинный обрежется и
        # превратится в неизвестный, то есть поле молча пропадёт.
        if len(key.encode("utf-8")) >= 24:
            rep.error("ключ '%s' длиннее 23 байт — ядро его усечёт и "
                      "проигнорирует поле" % key)

    if "name" not in man:
        rep.error("нет обязательного поля 'name'")
    for field, size in STRING_FIELDS.items():
        if field not in man:
            continue
        value = man[field]
        if not isinstance(value, str):
            rep.error("поле '%s' должно быть строкой" % field)
            continue
        n = len(value.encode("utf-8"))
        if n >= size:
            rep.error("поле '%s': %d байт, буфер ядра %d — будет усечено "
                      "(кириллица занимает 2 байта на символ)"
                      % (field, n, size))

    mtype = man.get("type")
    if mtype is None:
        rep.warn("нет поля 'type' — запись попадёт в каталог с типом по "
                 "имени родительского каталога (/apps или /modules)")
    elif mtype not in MODULE_TYPES:
        rep.error("type='%s'; допустимы %s" % (mtype, ", ".join(MODULE_TYPES)))
    elif elf_info:
        # Тип из манифеста и реальная точка входа должны совпадать, иначе
        # модуль загрузится и ничего не сделает.
        if mtype == "command" and elf_info["kind"] != "command":
            rep.error("type='command', но в образе нет %s" % SYM_CMD_MAIN)
        if mtype in ("app", "module") and elf_info["kind"] == "command":
            rep.error("type='%s', но в образе только %s — ядро запустит "
                      "задачу, которой некуда войти" % (mtype, SYM_CMD_MAIN))

    api_version = man.get("api_version")
    if api_version is None:
        rep.warn("нет 'api_version' — ядро пропустит модуль без проверки "
                 "совместимости")
    elif not isinstance(api_version, int) or isinstance(api_version, bool):
        rep.error("'api_version' должно быть целым числом")
    elif api_version < 1:
        rep.error("api_version=%d: версии нумеруются с 1" % api_version)
    elif api_version > lim["api_version"]:
        rep.error("api_version=%d новее ядра (%d) — orca_appmgr_start "
                  "откажется запускать" % (api_version, lim["api_version"]))

    stack = man.get("stack")
    if stack is not None:
        if not isinstance(stack, int) or isinstance(stack, bool):
            rep.error("'stack' должно быть числом байт")
        else:
            min_bytes = lim["min_stack_words"] * 4
            if stack < min_bytes:
                rep.error("stack=%d Б меньше минимума FreeRTOS (%d Б); ядро "
                          "поднимет значение и предупредит в логе"
                          % (stack, min_bytes))
            elif stack > 256 * 1024:
                rep.warn("stack=%d Б — это %.0f КБ из кучи ядра (128 КБ), "
                         "xTaskCreate почти наверняка не пройдёт"
                         % (stack, stack / 1024))

    perms = man.get("permissions")
    if perms is None:
        rep.warn("нет 'permissions' — модуль получит ВСЕ права "
                 "(обратная совместимость); перечислите нужные группы явно")
    elif not isinstance(perms, list):
        rep.error("'permissions' должно быть массивом строк")
    else:
        known = spec.perm_names
        for p in perms:
            if not isinstance(p, str):
                rep.error("'permissions': элементы должны быть строками")
                continue
            if len(p.encode("utf-8")) >= 16:
                rep.error("право '%s' длиннее 15 байт — ядро усечёт имя "
                          "и право не будет выдано" % p)
            if known is not None and p not in known:
                rep.error("неизвестное право '%s'; ядро молча его "
                          "проигнорирует. Доступны: %s" % (p, ", ".join(known)))
        if known is not None:
            extra = _permissions_unused(perms, elf_info)
            for msg in extra:
                rep.warn(msg)

    entry = man.get("entry")
    if entry is not None and isinstance(entry, str) and not entry.endswith(".orca"):
        rep.warn("entry='%s' без расширения .orca — appmgr ищет образ "
                 "именно по этому имени" % entry)


def _permissions_unused(perms, elf_info):
    """
    Права, которые почти наверняка лишние. Проверка грубая и намеренно
    консервативная: модуль вызывает API через таблицу, статически это не
    видно, поэтому говорим только о заведомо бессмысленных сочетаниях.
    """
    msgs = []
    if elf_info and elf_info["kind"] == "command" and "gui" in perms:
        msgs.append("право 'gui' у команды shell'а: команда выполняется "
                    "синхронно и рисовать ей негде")
    if len(set(perms)) != len(perms):
        msgs.append("в 'permissions' есть повторы")
    return msgs


# ------------------------------------------------------------------ #
# Команды                                                            #
# ------------------------------------------------------------------ #

def load_manifest(path, rep):
    try:
        with open(path, "rb") as f:
            raw = f.read()
    except OSError as e:
        rep.error("манифест %s: %s" % (path, e.strerror))
        return None, 0
    try:
        man = json.loads(raw.decode("utf-8"))
    except UnicodeDecodeError:
        rep.error("%s: манифест не в UTF-8" % path)
        return None, len(raw)
    except json.JSONDecodeError as e:
        rep.error("%s: не разбирается как JSON — %s (строка %d)"
                  % (path, e.msg, e.lineno))
        return None, len(raw)
    return man, len(raw)


def write_manifest(path, man):
    """
    Пишем с отступом в 2 пробела и ensure_ascii=False: манифест правят руками,
    и \\u0443\\u0432 вместо кириллицы делают это невозможным. Ядро \\u-escape
    всё равно не распаковывает (см. js_string), так что ASCII-форма ещё и
    ломала бы описание на устройстве.
    """
    text = json.dumps(man, indent=2, ensure_ascii=False) + "\n"
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)
    return len(text.encode("utf-8"))


def crc32_file(path):
    crc = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(65536)
            if not chunk:
                break
            crc = zlib.crc32(chunk, crc)
    return crc & 0xFFFFFFFF


def manifest_beside(image_path):
    """app.json, затем module.json рядом с образом — порядок как в appmgr."""
    base = os.path.dirname(os.path.abspath(image_path))
    for name in ("app.json", "module.json"):
        candidate = os.path.join(base, name)
        if os.path.isfile(candidate):
            return candidate
    return None


def cmd_pack(args):
    rep = Report(quiet=args.quiet)
    spec = KernelSpec(args.kernel_root)

    if not os.path.isfile(args.image):
        print("error: %s: файла нет" % args.image, file=sys.stderr)
        return 1

    with open(args.image, "rb") as f:
        data = f.read()
    elf = ElfImage(data, args.image)
    elf_info = check_elf(elf, spec, rep)

    # Куда кладём результат. Каталог -> <out>/<name>.orca, иначе путь как есть.
    manifest_path = args.manifest or manifest_beside(args.image)
    man, raw_len = (None, 0)
    if manifest_path:
        man, raw_len = load_manifest(manifest_path, rep)

    out_path = args.out
    if out_path and os.path.isdir(out_path):
        stem = None
        if isinstance(man, dict) and isinstance(man.get("entry"), str):
            stem = man["entry"]
        elif isinstance(man, dict) and isinstance(man.get("name"), str):
            stem = man["name"] + ".orca"
        else:
            stem = os.path.splitext(os.path.basename(args.image))[0] + ".orca"
        out_path = os.path.join(out_path, stem)

    if out_path and os.path.abspath(out_path) != os.path.abspath(args.image):
        if not rep.ok and not args.force:
            # Копировать заведомо негодный образ на карту — значит отложить
            # ту же ошибку до устройства.
            rep.dump()
            print("образ не скопирован: сначала исправьте ошибки "
                  "(или --force)", file=sys.stderr)
            return 1
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        shutil.copyfile(args.image, out_path)
        rep.note("образ записан: %s" % out_path)
    else:
        out_path = args.image

    crc = crc32_file(out_path)
    rep.note("crc32 = %08X" % crc)

    if isinstance(man, dict):
        check_manifest(man, raw_len, spec, rep, elf_info)

        if args.entry_check and isinstance(man.get("entry"), str):
            expected = os.path.join(os.path.dirname(os.path.abspath(manifest_path)),
                                    man["entry"])
            if os.path.abspath(out_path) != os.path.abspath(expected):
                rep.warn("entry='%s' указывает на %s, а образ лежит в %s — "
                         "appmgr его не найдёт"
                         % (man["entry"], expected, out_path))

        if not args.no_crc:
            man["crc32"] = "%08x" % crc
            dest = args.manifest_out or manifest_path
            if rep.ok or args.force:
                new_len = write_manifest(dest, man)
                rep.note("crc32 записан в %s" % dest)
                if new_len > spec.limits["manifest_max"]:
                    rep.error("после добавления crc32 манифест вырос до %d Б, "
                              "предел %d" % (new_len, spec.limits["manifest_max"]))
    elif manifest_path is None:
        rep.warn("манифест не найден рядом с образом: модуль запустится с "
                 "полными правами и без проверки crc32")

    rep.dump()
    if rep.ok:
        if not args.quiet:
            print("ok: %s" % out_path, file=sys.stderr)
        return 0
    return 1


def cmd_check(args):
    rep = Report(quiet=args.quiet)
    spec = KernelSpec(args.kernel_root)

    target = args.target
    manifest_path = None
    image_path = None

    if os.path.isdir(target):
        for name in ("app.json", "module.json"):
            candidate = os.path.join(target, name)
            if os.path.isfile(candidate):
                manifest_path = candidate
                break
        if manifest_path is None:
            orcas = [f for f in sorted(os.listdir(target)) if f.endswith(".orca")]
            if not orcas:
                print("error: в %s нет ни манифеста, ни .orca" % target,
                      file=sys.stderr)
                return 1
            image_path = os.path.join(target, orcas[0])
    elif target.endswith(".json"):
        manifest_path = target
    else:
        image_path = target
        manifest_path = manifest_beside(target)

    man, raw_len = (None, 0)
    if manifest_path:
        man, raw_len = load_manifest(manifest_path, rep)

    if image_path is None and isinstance(man, dict):
        base = os.path.dirname(os.path.abspath(manifest_path))
        entry = man.get("entry")
        candidates = []
        if isinstance(entry, str):
            candidates.append(entry)
        if isinstance(man.get("name"), str):
            candidates.append(man["name"] + ".orca")
        candidates += ["app.orca", "module.orca"]
        seen = set()
        candidates = [c for c in candidates
                      if not (c in seen or seen.add(c))]
        for c in candidates:
            if os.path.isfile(os.path.join(base, c)):
                image_path = os.path.join(base, c)
                break
        if image_path is None:
            rep.error("образ не найден: пробовали %s" % ", ".join(candidates))

    elf_info = None
    if image_path and os.path.isfile(image_path):
        with open(image_path, "rb") as f:
            elf = ElfImage(f.read(), image_path)
        elf_info = check_elf(elf, spec, rep)

    if isinstance(man, dict):
        check_manifest(man, raw_len, spec, rep, elf_info)

        # Ровно та же сверка, что делает orca_appmgr_start перед загрузкой.
        declared = man.get("crc32")
        if declared is None:
            rep.warn("в манифесте нет 'crc32' — ядро загрузит образ без "
                     "проверки целостности")
        elif image_path and os.path.isfile(image_path):
            actual = crc32_file(image_path)
            want = declared if isinstance(declared, int) else \
                int(str(declared), 16)
            if want != actual:
                rep.error("crc32 не совпадает: в манифесте %08X, у файла %08X "
                          "— образ пересобран без elf2orca или повреждён"
                          % (want, actual))
            else:
                rep.note("crc32 %08X совпадает" % actual)

    rep.dump()
    if rep.ok:
        if not args.quiet:
            print("ok: %s" % (manifest_path or image_path), file=sys.stderr)
        return 0
    return 1


def cmd_info(args):
    """Что увидит загрузчик: секции, символы, релокации. Для отладки."""
    spec = KernelSpec(args.kernel_root)
    with open(args.image, "rb") as f:
        elf = ElfImage(f.read(), args.image)
    if not elf.valid_header:
        print("error: не ELF", file=sys.stderr)
        return 1

    print("%s: ET_%s, machine=%d, секций %d"
          % (args.image, "REL" if elf.e_type == ET_REL else str(elf.e_type),
             elf.e_machine, elf.e_shnum))
    print("\nсекции (* — грузится в арену):")
    for s in elf.sections:
        mark = "*" if s.flags & SHF_ALLOC else " "
        print("  %s %-20s type=%-2d size=%-8d align=%d"
              % (mark, s.name or "?", s.type, s.size, s.align))

    undefined = sorted({s.name for s in elf.symbols if s.shndx == 0 and s.name})
    if undefined:
        print("\nвнешние символы:")
        for n in undefined:
            known = spec.kernel_symbols is None or n in spec.kernel_symbols
            print("  %s %s" % ("ok " if known else "НЕТ", n))

    counts = {}
    for _sec, rtype, _off, _sym in elf.relocations():
        counts[rtype] = counts.get(rtype, 0) + 1
    if counts:
        print("\nрелокации:")
        for rtype, n in sorted(counts.items()):
            name = RELOC_NAMES.get(rtype, "неподдерживаемая")
            print("  %-18s (%2d) x%d" % (name, rtype, n))

    entries = [n for n in (SYM_APP_MAIN, SYM_APP_STOP, SYM_CMD_MAIN)
               if any(s.name == n and s.shndx != 0 for s in elf.symbols)]
    print("\nточки входа: %s" % (", ".join(entries) or "нет"))
    print("crc32: %08X" % crc32_file(args.image))
    return 0


def main(argv=None):
    p = argparse.ArgumentParser(
        prog="elf2orca",
        description="упаковка и проверка модулей Orca (.orca)")
    p.add_argument("--kernel-root", metavar="DIR",
                   help="корень репозитория (по умолчанию ищется вверх от "
                        "скрипта); из него берутся лимиты загрузчика")
    p.add_argument("-q", "--quiet", action="store_true",
                   help="только ошибки и предупреждения")
    sub = p.add_subparsers(dest="cmd")

    sp = sub.add_parser("pack", help="проверить образ и записать crc32 в манифест")
    sp.add_argument("image", help="собранный relocatable ELF (.elf или .orca)")
    sp.add_argument("-o", "--out", metavar="PATH",
                    help="куда положить .orca (каталог или файл)")
    sp.add_argument("-m", "--manifest", metavar="FILE",
                    help="манифест (по умолчанию app.json/module.json рядом)")
    sp.add_argument("--manifest-out", metavar="FILE",
                    help="куда записать манифест с crc32")
    sp.add_argument("--no-crc", action="store_true",
                    help="не трогать манифест, только проверить")
    sp.add_argument("--entry-check", action="store_true", default=True,
                    help="сверить поле entry с фактическим именем образа")
    sp.add_argument("--force", action="store_true",
                    help="писать результат, несмотря на ошибки")
    sp.set_defaults(func=cmd_pack)

    sc = sub.add_parser("check", help="проверить то, что уже лежит на карте")
    sc.add_argument("target", help="манифест, .orca или каталог модуля")
    sc.set_defaults(func=cmd_check)

    si = sub.add_parser("info", help="показать образ глазами загрузчика")
    si.add_argument("image")
    si.set_defaults(func=cmd_info)

    args = p.parse_args(argv)
    if not getattr(args, "func", None):
        p.print_help()
        return 2
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
