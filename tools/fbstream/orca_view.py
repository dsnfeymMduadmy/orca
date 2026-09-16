#!/usr/bin/env python3
"""Orca Link — клиент устройства на ПК.

    python3 orca_view.py /dev/ttyUSB0 --stream console
    python3 orca_view.py /dev/ttyUSB0 --stream gui
    python3 orca_view.py /dev/ttyUSB0 --stream gui --src gui --log
    python3 orca_view.py /dev/ttyUSB0 --stream term --fps 15
    python3 orca_view.py 192.168.4.1:3333 --stream gui   # через ESP32
    python3 orca_view.py /dev/ttyUSB0 --shot s.png       # один кадр в файл
    python3 orca_view.py /dev/ttyUSB0 --shell "free"     # одна команда

Режимы (--stream):
    console — консоль устройства: логи прошивки и вывод команд, ввод команд
              с клавиатуры. Кадры не запрашиваются вообще.
    gui     — окно 480x320 (альбомная ориентация, размер будущей панели):
              трансляция экрана, клик/перетаскивание мышью уходят на
              устройство кадрами ORCA_LINK_TOUCH. Снизу — лог и строка ввода.
    term    — картинка прямо в терминале полублоками '▀'. Нужен TrueColor.

Источник кадров (--src):
    gui     — интерфейс устройства на LVGL (штатный источник прошивки).
    test    — тестовая картинка fbtest: нужна, когда надо понять, кто сломан,
              GUI или сам тракт SDRAM -> Orca Link -> ПК.
    Без флага источник на устройстве не трогаем: прошивка сама выбирает test,
    если GUI не поднялся, и перебивать её выбор значит получить чёрный экран.

--log [ФАЙЛ] — копия всего, что сказало устройство (логи прошивки, ответы
    команд, ошибки), в файл с метками времени. Без имени файл называется
    orca-ГГГГММДД-ЧЧММСС.log. Пишется независимо от режима, поэтому лог
    остаётся полным, даже если окно gui закрылось с ошибкой.

Зависимости: pyserial (для UART), tkinter (для gui), Pillow (для --shot и
для быстрого масштабирования в gui — не обязательна).
"""

import argparse
import array
import os
import select
import struct
import sys
import time

# ─── Orca Link ───────────────────────────────────────────────────────

MAGIC0, MAGIC1 = 0xAA, 0x55
VERSION = 1
HEADER_SIZE = 8
CRC_SIZE = 2
MAX_PAYLOAD = 1024
MAX_FRAME = HEADER_SIZE + MAX_PAYLOAD + CRC_SIZE

T_PING, T_PONG, T_HELLO = 0x01, 0x02, 0x03
T_LOG = 0x10
T_FB_INFO, T_FB_CHUNK, T_FB_END, T_FB_REQ = 0x20, 0x21, 0x22, 0x23
T_TOUCH, T_BUTTON = 0x30, 0x31
T_SHELL, T_SHELL_RESP = 0x40, 0x41
T_ERROR = 0xF0

PIXFMT = {"rgb565": 0x01, "rgb888": 0x02, "gray8": 0x03, "rle": 0x10}
BPP = {0x01: 2, 0x02: 3, 0x03: 1}

# Логический размер экрана устройства (ORCA_FBTEST_WIDTH/HEIGHT).
# Окно gui-режима всегда такое, независимо от scale трансляции: координаты
# тача считаются в этой системе, а пришедший кадр растягивается под неё.
SCREEN_W, SCREEN_H = 480, 320

TOUCH_UP, TOUCH_DOWN, TOUCH_MOVE = 0, 1, 2

# Структуры (#pragma pack(1), little-endian) — держать в синхроне с orca_link.h
S_HELLO = struct.Struct("<BBHI24s16s")        # 48 B
S_LOG = struct.Struct("<BBH16s")              # 20 B
S_FB_INFO = struct.Struct("<IHHBBHI")         # 16 B
S_FB_CHUNK = struct.Struct("<IHH")            #  8 B
S_FB_REQ = struct.Struct("<BBBBB3s")          #  8 B
S_TOUCH = struct.Struct("<HHBB2s")            #  8 B
S_BUTTON = struct.Struct("<BB2s")             #  4 B
S_SHELL_RESP = struct.Struct("<iHH")          #  8 B
S_ERROR = struct.Struct("<HH")                #  4 B


def crc16(data: bytes) -> int:
    """CRC16-CCITT-FALSE, как в orca_link_crc16()."""
    crc = 0xFFFF
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def encode(ftype: int, seq: int, payload: bytes = b"") -> bytes:
    body = bytes([VERSION, ftype]) + struct.pack("<HH", seq, len(payload)) + payload
    return bytes([MAGIC0, MAGIC1]) + body + struct.pack("<H", crc16(body))


class Parser:
    """Потоковый разбор — зеркало orca_link_rx_byte().

    Байты, не попавшие ни в один кадр, не выбрасываются, а копятся в
    `text`: устройство печатает логи обычным printf'ом мимо протокола
    (см. retarget.c), и в console-режиме это ровно то, что нужно показать.
    Логика разделения та же, что на прошивке: 0xAA вне кадра — начало кадра,
    всё остальное — текст.
    """

    WAIT0, WAIT1, HEADER, PAYLOAD, CRC = range(5)

    def __init__(self):
        self.state = self.WAIT0
        self.buf = bytearray(MAX_FRAME)
        self.pos = 0
        self.plen = 0
        self.ok = self.crc_err = self.resync = 0
        self.text = bytearray()

    def feed(self, byte: int):
        st = self.state

        if st == self.WAIT0:
            if byte == MAGIC0:
                self.buf[0] = byte
                self.pos = 1
                self.state = self.WAIT1
            else:
                self.text.append(byte)

        elif st == self.WAIT1:
            if byte == MAGIC1:
                self.buf[1] = byte
                self.pos = 2
                self.state = self.HEADER
            elif byte == MAGIC0:
                # Два 0xAA подряд: первый был не заголовком, а текстом.
                self.text.append(MAGIC0)
                self.pos = 1
            else:
                self.resync += 1
                self.text.append(MAGIC0)
                self.text.append(byte)
                self.state = self.WAIT0

        elif st == self.HEADER:
            self.buf[self.pos] = byte
            self.pos += 1
            if self.pos == HEADER_SIZE:
                if self.buf[2] != VERSION:
                    self.resync += 1
                    self.state = self.WAIT0
                    return None
                self.plen = self.buf[6] | (self.buf[7] << 8)
                if self.plen > MAX_PAYLOAD:
                    self.resync += 1
                    self.state = self.WAIT0
                    return None
                self.state = self.PAYLOAD if self.plen else self.CRC

        elif st == self.PAYLOAD:
            self.buf[self.pos] = byte
            self.pos += 1
            if self.pos == HEADER_SIZE + self.plen:
                self.state = self.CRC

        elif st == self.CRC:
            self.buf[self.pos] = byte
            self.pos += 1
            if self.pos == HEADER_SIZE + self.plen + CRC_SIZE:
                self.state = self.WAIT0
                got = self.buf[self.pos - 2] | (self.buf[self.pos - 1] << 8)
                want = crc16(bytes(self.buf[2:HEADER_SIZE + self.plen]))
                if got != want:
                    self.crc_err += 1
                    return None
                self.ok += 1
                return (self.buf[3],
                        bytes(self.buf[HEADER_SIZE:HEADER_SIZE + self.plen]))

        return None

    def take_text(self) -> bytes:
        out = bytes(self.text)
        self.text.clear()
        return out


# ─── транспорт ───────────────────────────────────────────────────────

class Transport:
    """Единый интерфейс поверх pyserial и TCP-сокета."""

    def __init__(self, uri: str, reset: bool = True):
        self.seq = 0
        if ":" in uri and not uri.startswith("/"):
            import socket
            host, port = uri.rsplit(":", 1)
            self.sock = socket.create_connection((host, int(port)), timeout=5)
            self.sock.setblocking(False)
            self.fd = self.sock
            self.kind = "tcp"
            self.desc = f"tcp {host}:{port}"
        else:
            import serial
            dev, _, baud = uri.partition("@")
            baud = int(baud) if baud else 921600
            self.ser = serial.Serial()
            self.ser.port = dev
            self.ser.baudrate = baud
            self.ser.timeout = 0
            # DTR/RTS специально НЕ трогаем ни до, ни сразу после open():
            # pyserial поднимает обе линии, а на плате это RTS -> BOOT0 и
            # DTR -> NRST, то есть с поднятыми линиями плата стоит в reset.
            # Это ровно то, что нужно: пока она там стоит, ПК успевает открыть
            # файл лога, и загрузка не уходит в никуда. Отпускает плату start().
            self.ser.open()
            # Мусор прошлого сеанса выбрасываем здесь: плата в reset и ещё
            # ничего не напечатала, потерять boot-лог невозможно.
            self.ser.reset_input_buffer()
            self._reset = reset
            if not reset:
                # Держать плату в reset нельзя даже с --no-reset: линии поднял
                # драйвер, а не мы. Отпускаем сразу — плата всё равно
                # перезапустится, на CH340 этого не избежать.
                self._release_lines()
            self.fd = self.ser
            self.kind = "serial"
            self.desc = f"{dev} @ {baud}"

    def start(self):
        """Отпустить плату: сброс (если не запрещён) и ожидание Orca Link.

        Вынесено из конструктора, потому что сброс обязан произойти после
        того, как открыт файл лога, — иначе boot-лог прошивки уходит в
        никуда, а именно за ним лог обычно и включают.
        """
        if self.kind != "serial" or not self._reset:
            return
        self._reset = False
        self._release_boot0()

    def _release_lines(self):
        """Отпустить плату: BOOT0 = 0, и только потом фронт на NRST.

        Порядок важнее самих уровней: отпустить NRST, пока RTS (BOOT0) ещё
        высокий, значит увести плату в системный загрузчик USB DFU — консоль
        замолкает, и это выглядит как зависшая прошивка.
        """
        self.ser.rts = False          # BOOT0 = 0 -> грузимся из flash
        self.ser.dtr = False          # NRST отпущен -> плата стартует

    def _release_boot0(self):
        """Перезапустить плату схемой автозагрузки CH340 (RTS->BOOT0, DTR->NRST).

        Перезагружает именно переход «обе линии высоко -> обе низко» — тот же,
        что Linux устраивает при open(). Одиночный импульс на DTR схема не
        замечает (проверено на плате: 0 байт в ответ, прошивка продолжает
        работать с прошлого старта), поэтому сначала поднимаем обе линии и
        только потом отпускаем в безопасном порядке.
        """
        self.ser.rts = True           # BOOT0 высокий, но NRST уже активен
        self.ser.dtr = True
        time.sleep(0.05)
        self._release_lines()
        # Ждём, пока ядро поднимет службу Orca Link: до этого любой кадр,
        # который мы пошлём, просто некому принять. Прошивка за это время
        # успевает напечатать всю загрузку — она копится в буфере драйвера
        # и достаётся первым же poll(), то есть попадает и в лог.
        time.sleep(1.2)

    def fileno(self):
        return self.fd.fileno()

    def read(self, timeout=0.1) -> bytes:
        r, _, _ = select.select([self.fd], [], [], timeout)
        if not r:
            return b""
        if self.kind == "tcp":
            try:
                return self.sock.recv(4096)
            except BlockingIOError:
                return b""
        n = self.ser.in_waiting or 1
        return self.ser.read(n)

    def send(self, ftype: int, payload: bytes = b""):
        data = encode(ftype, self.seq & 0xFFFF, payload)
        self.seq += 1
        if self.kind == "tcp":
            self.sock.sendall(data)
        else:
            self.ser.write(data)
            self.ser.flush()

    def close(self):
        (self.sock if self.kind == "tcp" else self.ser).close()


# ─── разбор потока кадров ────────────────────────────────────────────

LOG_LEVEL = {0: "INFO", 1: "WARN", 2: "ERROR"}


class Client:
    """Транспорт + парсер + сборка многокадровых сообщений.

    Кадр экрана приходит как FB_INFO / N x FB_CHUNK / FB_END, а ответ
    shell'а — как цепочка SHELL_RESP с флагом more. Собирать это в каждом
    режиме заново смысла нет, поэтому склейка живёт здесь, а режимы
    получают готовые события через колбэки.
    """

    def __init__(self, tr: Transport, log_file=None):
        self.tr = tr
        self.parser = Parser()

        self.on_frame = None      # (fb, w, h, pixfmt)
        self.on_text = None       # str — сырой вывод прошивки мимо протокола
        self.on_shell = None      # (status, text)
        self.on_hello = None      # str
        self.on_error = None      # str

        self.log_file = log_file
        self._log_bol = True      # курсор лога стоит в начале строки

        self._fb = None
        self._fw = self._fh = self._fmt = 0
        self._off = 0
        self._shell = []
        self.frames = 0
        self.t0 = time.time()

    # -- лог ----------------------------------------------------------

    def mirror(self, s: str):
        """Копия сказанного устройством в файл, с метками времени.

        Живёт здесь, а не в режимах: текст приходит четырьмя путями (сырой
        printf прошивки, LOG, ответ shell'а, ERROR), и дублировать запись в
        каждом режиме значит гарантированно забыть один из них.

        Метку ставим только там, где строка действительно начинается:
        прошивка отдаёт строку кусками по мере готовности UART, и без
        _log_bol метка времени влезала бы в середину слова.
        """
        if not self.log_file or not s:
            return
        stamp = time.strftime("[%H:%M:%S] ")
        for part in s.replace("\r\n", "\n").replace("\r", "\n").splitlines(True):
            if self._log_bol:
                self.log_file.write(stamp)
            self.log_file.write(part)
            self._log_bol = part.endswith("\n")
        # Флаш на каждый кусок: лог читают, пока трансляция идёт, и он же
        # остаётся единственным следом, если процесс умер.
        self.log_file.flush()

    def _text(self, s: str):
        self.mirror(s)
        if self.on_text:
            self.on_text(s)

    # -- отправка -----------------------------------------------------

    def hello(self):
        self.tr.send(T_HELLO)

    def shell(self, cmd: str):
        self.tr.send(T_SHELL, cmd.encode())

    def set_source(self, src: str):
        """Источник кадров: 'gui' (LVGL) или 'test' (fbtest).

        Своего кадра протокола под это нет — переключает команда shell'а
        (cmd_fb в orca_shell.c), так что и здесь это обычная команда.
        Отправлять её надо до stream_on: смена источника на ходу оставит
        на ПК недособранный кадр от предыдущего.
        """
        self.shell(f"fb src {src}")

    def stream_on(self, pixfmt: int, scale: int, fps: int, display=0):
        self.tr.send(T_FB_REQ, S_FB_REQ.pack(1, display, pixfmt,
                                             max(scale, 1),
                                             max(min(fps, 60), 1), b"\0\0\0"))

    def stream_off(self):
        self.tr.send(T_FB_REQ, S_FB_REQ.pack(0, 0, 0, 1, 10, b"\0\0\0"))

    def touch(self, x: int, y: int, action: int, display=0):
        x = max(0, min(int(x), SCREEN_W - 1))
        y = max(0, min(int(y), SCREEN_H - 1))
        self.tr.send(T_TOUCH, S_TOUCH.pack(x, y, display, action, b"\0\0"))

    def button(self, button_id: int, pressed: bool):
        self.tr.send(T_BUTTON, S_BUTTON.pack(button_id, 1 if pressed else 0, b"\0\0"))

    # -- приём --------------------------------------------------------

    def poll(self, timeout=0.05) -> bool:
        """Читает всё доступное. False — соединение закрылось."""
        data = self.tr.read(timeout)
        if not data:
            return True

        for b in data:
            got = self.parser.feed(b)
            if got:
                self._dispatch(*got)

        raw = self.parser.take_text()
        if raw:
            self._text(raw.decode("utf-8", errors="replace"))
        return True

    def _dispatch(self, ftype: int, pl: bytes):
        if ftype == T_FB_INFO and len(pl) >= S_FB_INFO.size:
            _fid, w, h, fmt, _did, _cc, total = S_FB_INFO.unpack_from(pl)
            # Кадр может прийти рваным (устройство дропает чанки при занятом
            # UART) — недостающие байты остаются нулями, а не мусором
            # прошлого кадра.
            self._fb = bytearray(total)
            self._fw, self._fh, self._fmt = w, h, fmt
            self._off = 0

        elif ftype == T_FB_CHUNK and self._fb is not None:
            _fid, _idx, dlen = S_FB_CHUNK.unpack_from(pl)
            data = pl[S_FB_CHUNK.size:S_FB_CHUNK.size + dlen]
            if self._off + len(data) <= len(self._fb):
                self._fb[self._off:self._off + len(data)] = data
                self._off += len(data)

        elif ftype == T_FB_END and self._fb is not None:
            self.frames += 1
            if self.on_frame:
                self.on_frame(self._fb, self._fw, self._fh, self._fmt)
            self._fb = None

        elif ftype == T_SHELL_RESP and len(pl) >= S_SHELL_RESP.size:
            status, tlen, more = S_SHELL_RESP.unpack_from(pl)
            self._shell.append(pl[S_SHELL_RESP.size:S_SHELL_RESP.size + tlen])
            if not more:
                text = b"".join(self._shell).decode("utf-8", errors="replace")
                self._shell = []
                self.mirror(text if text.endswith("\n") or not text
                            else text + "\n")
                if self.on_shell:
                    self.on_shell(status, text)

        elif ftype == T_HELLO and len(pl) >= S_HELLO.size:
            api, link, disp, up, dev, ver = S_HELLO.unpack_from(pl)
            name = dev.split(b"\0")[0].decode(errors="replace")
            fwv = ver.split(b"\0")[0].decode(errors="replace")
            hello = (f"{name} fw={fwv} api=v{api} link=v{link} "
                     f"displays={disp} uptime={up}ms")
            # В лог тоже: без версии прошивки лог через неделю бесполезен.
            self.mirror(f"[hello] {hello}\n")
            if self.on_hello:
                self.on_hello(hello)

        elif ftype == T_LOG and len(pl) >= S_LOG.size:
            lvl, _rsv, mlen, tag = S_LOG.unpack_from(pl)
            msg = pl[S_LOG.size:S_LOG.size + mlen].decode(errors="replace")
            tag = tag.split(b"\0")[0].decode(errors="replace")
            self._text(f"[{LOG_LEVEL.get(lvl, '?')}][{tag}] {msg}\n")

        elif ftype == T_ERROR and len(pl) >= S_ERROR.size:
            code, tlen = S_ERROR.unpack_from(pl)
            msg = pl[S_ERROR.size:S_ERROR.size + tlen].decode(errors="replace")
            self.mirror(f"[error] {code}: {msg}\n")
            if self.on_error:
                self.on_error(f"ошибка {code}: {msg}")

        elif ftype == T_PONG:
            self._text("[pong]\n")

    def fps(self) -> float:
        return self.frames / max(time.time() - self.t0, 1e-6)


# ─── пиксели ─────────────────────────────────────────────────────────

_RGB565_LUT = None


def _rgb565_lut():
    """Таблица 65536 -> 3 байта RGB.

    Попиксельная арифметика в Python на 480x320 занимала ~0.4 с на кадр —
    трансляция шла медленнее, чем устройство её отдавало. Таблица строится
    один раз (~50 мс) и превращает распаковку в map + join на уровне C.
    """
    global _RGB565_LUT
    if _RGB565_LUT is None:
        lut = []
        for v in range(65536):
            r = (v >> 11) & 0x1F
            g = (v >> 5) & 0x3F
            b = v & 0x1F
            lut.append(bytes(((r << 3) | (r >> 2),
                              (g << 2) | (g >> 4),
                              (b << 3) | (b >> 2))))
        _RGB565_LUT = lut
    return _RGB565_LUT


def to_rgb888(fb: bytes, w: int, h: int, pixfmt: int) -> bytes:
    """Кадр устройства -> плотный RGB888 размера w*h*3."""
    need = w * h
    if pixfmt == 0x01:
        arr = array.array("H")
        usable = min(len(fb) // 2, need)
        arr.frombytes(bytes(fb[:usable * 2]))
        if sys.byteorder == "big":
            arr.byteswap()
        lut = _rgb565_lut()
        out = b"".join(map(lut.__getitem__, arr))
        return out + b"\0" * ((need - usable) * 3)
    if pixfmt == 0x02:
        out = bytes(fb[:need * 3])
        return out + b"\0" * (need * 3 - len(out))
    if pixfmt == 0x10:
        # RLE_RGB565: пары [count:u8][pixel:u16 LE], count 1..255.
        # Кодер — orca_fbstream.c (rle_walk), второй декодер живёт в
        # app/.../link/FrameAssembler.kt: расхождение проявится как сдвиг
        # картинки по диагонали, поэтому логика тут ровно та же.
        lut = _rgb565_lut()
        parts = []
        got = 0
        pos = 0
        end = len(fb) - 2
        while pos <= end and got < need:
            count = fb[pos]
            px = lut[fb[pos + 1] | (fb[pos + 2] << 8)]
            pos += 3
            # Прогон обрезаем по остатку кадра: битый count не должен
            # раздувать буфер и рвать растр.
            count = min(count, need - got)
            if count:
                parts.append(px * count)
                got += count
        # Недостающий хвост — нули, как и при потерянных чанках сырого кадра.
        return b"".join(parts) + b"\0" * ((need - got) * 3)
    out = bytearray()
    src = fb[:need]
    for v in src:
        out += bytes((v, v, v))
    return bytes(out) + b"\0" * ((need - len(src)) * 3)


def save_png(path, fb, w, h, pixfmt):
    try:
        from PIL import Image
    except ImportError:
        print("[!] нужен Pillow: pip install pillow", file=sys.stderr)
        return False
    Image.frombytes("RGB", (w, h), to_rgb888(fb, w, h, pixfmt)).save(path)
    return True


# ─── режим term: рендер в терминал ───────────────────────────────────

CSI = "\x1b["
RESET = CSI + "0m"
UPPER_HALF = "\u2580"


def term_size():
    try:
        c = os.get_terminal_size()
        return c.columns, c.lines
    except OSError:
        return 80, 24


def render_term(fb: bytes, w: int, h: int, pixfmt: int) -> str:
    tw, th = term_size()
    out_w = min(w, tw)
    out_h = min(h // 2, th - 2)
    if out_w <= 0 or out_h <= 0:
        return "терминал слишком мал"

    rgb = to_rgb888(fb, w, h, pixfmt)
    stride = w * 3
    xs = [min(int(x * w / out_w), w - 1) for x in range(out_w)]

    rows = []
    for ty in range(out_h):
        y_top = min(int((ty * 2) * h / (out_h * 2)), h - 1)
        y_bot = min(int((ty * 2 + 1) * h / (out_h * 2)), h - 1)
        base_t, base_b = y_top * stride, y_bot * stride
        parts = []
        last = None
        for sx in xs:
            ot, ob = base_t + sx * 3, base_b + sx * 3
            top = (rgb[ot], rgb[ot + 1], rgb[ot + 2])
            bot = (rgb[ob], rgb[ob + 1], rgb[ob + 2])
            if last != (top, bot):
                parts.append(f"{CSI}38;2;{top[0]};{top[1]};{top[2]}m"
                             f"{CSI}48;2;{bot[0]};{bot[1]};{bot[2]}m")
                last = (top, bot)
            parts.append(UPPER_HALF)
        parts.append(RESET)
        rows.append("".join(parts))
    return "\n".join(rows)


def run_term(client: Client, args) -> int:
    def on_frame(fb, w, h, fmt):
        sys.stdout.write(CSI + "H" + render_term(fb, w, h, fmt))
        sys.stdout.write(
            f"\n{CSI}K {w}x{h}  {client.frames} кадров  {client.fps():4.1f} fps  "
            f"crc_err={client.parser.crc_err} resync={client.parser.resync}  "
            f"Ctrl-C выход")
        sys.stdout.flush()

    client.on_frame = on_frame
    client.on_hello = lambda s: print(f"[OK] {s}", file=sys.stderr)
    client.on_error = lambda s: print(f"\n[!] устройство: {s}", file=sys.stderr)
    client.on_shell = lambda st, t: (sys.stdout.write(t), sys.stdout.flush())
    client.on_text = None   # логи затёрли бы картинку

    client.hello()
    if args.src:
        client.set_source(args.src)
    client.stream_on(PIXFMT[args.fmt], args.scale, args.fps)
    print(f"[OK] стрим: {args.fmt} scale={args.scale} fps={args.fps}"
          + (f" src={args.src}" if args.src else ""), file=sys.stderr)
    sys.stdout.write(CSI + "2J")

    while True:
        client.poll(0.05)


# ─── режим console ───────────────────────────────────────────────────

CONSOLE_HELP = """\
Консоль Orca. Команды уходят на устройство, вывод и логи приходят сюда.
  /quit        выйти            /ping    проверить связь
  /hello       версия прошивки  /reset   переоткрыть порт (сброс платы)
Всё остальное — команда устройства ('help' — её список).
"""


def run_console(client: Client, args) -> int:
    """Логи прошивки + интерактивный ввод команд.

    Читаем два источника разом через select: сокет/порт устройства и stdin.
    Отдельный поток на ввод не нужен, а построчного режима терминала
    достаточно — редактирование строки делает сам tty.
    """
    def out(s):
        sys.stdout.write(s)
        sys.stdout.flush()

    client.on_text = out
    client.on_shell = lambda st, t: out(t if t else "")
    client.on_hello = lambda s: out(f"[OK] {s}\n")
    client.on_error = lambda s: out(f"[!] устройство: {s}\n")
    # Кадры в консоли не нужны — и не просим их, чтобы не забивать линию.
    client.stream_off()
    client.hello()

    out(CONSOLE_HELP)
    out("orca> ")

    while True:
        r, _, _ = select.select([sys.stdin, client.tr.fd], [], [], 0.1)

        if client.tr.fd in r:
            client.poll(0)

        if sys.stdin in r:
            line = sys.stdin.readline()
            if not line:            # Ctrl-D
                return 0
            cmd = line.strip()

            if cmd in ("/quit", "/exit", "/q"):
                return 0
            if cmd == "/ping":
                client.tr.send(T_PING)
            elif cmd == "/hello":
                client.hello()
            elif cmd == "/reset":
                if client.tr.kind == "serial":
                    client.tr._release_boot0()
                    out("[OK] плата перезапущена\n")
                else:
                    out("[!] сброс доступен только по UART\n")
            elif cmd:
                client.shell(cmd)
            out("orca> ")


# ─── режим gui ───────────────────────────────────────────────────────

# Палитра. Держим в одном месте: цвет, вписанный в два десятка вызовов
# config(), потом невозможно поменять, не пропустив половину виджетов.
UI = {
    "bg":       "#16181d",   # фон окна
    "panel":    "#1e2128",   # карточки, тулбар
    "panel_hi": "#272b34",   # ховер, активная вкладка
    "line":     "#333947",   # разделители
    "fg":       "#e6e8ee",   # основной текст
    "fg_dim":   "#8b93a7",   # подписи
    "accent":   "#4c9aff",   # акцент, фокус
    "ok":       "#3ddc97",
    "warn":     "#ffb454",
    "err":      "#ff6b6b",
    "screen":   "#000000",
}

FONT_UI = ("DejaVu Sans", 10)
FONT_UI_SMALL = ("DejaVu Sans", 9)
FONT_MONO = ("DejaVu Sans Mono", 10)
FONT_MONO_SMALL = ("DejaVu Sans Mono", 9)


def run_gui(client: Client, args) -> int:
    """Окно с трансляцией экрана, консолью и управлением на лету.

    Картинка всегда рисуется в области с пропорциями SCREEN_W:SCREEN_H, даже
    если устройство шлёт кадр прореженным (scale>1) — растягиваем. Координаты
    мыши пересчитываются из размера области в систему экрана устройства,
    поэтому scale и размер окна можно крутить как угодно, не ломая попадание
    тапов.
    """
    try:
        import tkinter as tk
        from tkinter import ttk
    except ImportError:
        print("[!] нужен tkinter: sudo pacman -S tk / apt install python3-tk",
              file=sys.stderr)
        return 1

    try:
        from PIL import Image, ImageTk
        have_pil = True
    except ImportError:
        have_pil = False

    import base64

    root = tk.Tk()
    root.title(f"Orca — {client.tr.desc}")
    root.geometry("980x760")
    root.minsize(760, 560)
    root.configure(bg=UI["bg"])

    style = ttk.Style(root)
    # clam — единственная встроенная тема, которая позволяет перекрасить всё;
    # default и native игнорируют часть настроек фона.
    with_clam = "clam" in style.theme_names()
    if with_clam:
        style.theme_use("clam")

    style.configure("TFrame", background=UI["bg"])
    style.configure("Panel.TFrame", background=UI["panel"])
    style.configure("TLabel", background=UI["bg"], foreground=UI["fg"],
                    font=FONT_UI)
    style.configure("Panel.TLabel", background=UI["panel"], foreground=UI["fg"],
                    font=FONT_UI)
    style.configure("Dim.TLabel", background=UI["panel"],
                    foreground=UI["fg_dim"], font=FONT_UI_SMALL)
    style.configure("Value.TLabel", background=UI["panel"], foreground=UI["fg"],
                    font=FONT_MONO_SMALL)
    style.configure("Title.TLabel", background=UI["panel"],
                    foreground=UI["fg_dim"], font=(FONT_UI[0], 9, "bold"))
    style.configure("TButton", background=UI["panel_hi"], foreground=UI["fg"],
                    borderwidth=0, focusthickness=0, padding=(12, 6),
                    font=FONT_UI)
    style.map("TButton",
              background=[("active", UI["line"]), ("disabled", UI["panel"])],
              foreground=[("disabled", UI["fg_dim"])])
    style.configure("Accent.TButton", background=UI["accent"],
                    foreground="#0b1220")
    style.map("Accent.TButton", background=[("active", "#6fb0ff")])
    style.configure("TCombobox", fieldbackground=UI["panel_hi"],
                    background=UI["panel_hi"], foreground=UI["fg"],
                    arrowcolor=UI["fg_dim"], borderwidth=0, padding=4)
    style.map("TCombobox", fieldbackground=[("readonly", UI["panel_hi"])])
    style.configure("TCheckbutton", background=UI["panel"],
                    foreground=UI["fg_dim"], font=FONT_UI_SMALL)
    style.map("TCheckbutton", background=[("active", UI["panel"])])
    style.configure("TSeparator", background=UI["line"])
    style.configure("Vertical.TScrollbar", background=UI["panel_hi"],
                    troughcolor=UI["bg"], borderwidth=0, arrowcolor=UI["fg_dim"])

    root.option_add("*TCombobox*Listbox.background", UI["panel_hi"])
    root.option_add("*TCombobox*Listbox.foreground", UI["fg"])
    root.option_add("*TCombobox*Listbox.selectBackground", UI["accent"])
    root.option_add("*TCombobox*Listbox.font", FONT_UI_SMALL)

    state = {
        "streaming": True,
        "fmt": args.fmt,
        "scale": args.scale,
        "fps": args.fps,
        "autoscroll": True,
        "last_w": 0,
        "last_h": 0,
        "frame_times": [],     # окно для мгновенного fps
        "img": None,           # ссылка на PhotoImage, иначе GC съест картинку
    }

    # ── шапка ────────────────────────────────────────────────────────
    header = ttk.Frame(root, style="Panel.TFrame", padding=(14, 10))
    header.pack(fill="x")

    dot = tk.Canvas(header, width=12, height=12, bg=UI["panel"],
                    highlightthickness=0)
    dot.pack(side="left")
    dot_id = dot.create_oval(2, 2, 10, 10, fill=UI["fg_dim"], outline="")

    ttk.Label(header, text="Orca Link", style="Panel.TLabel",
              font=(FONT_UI[0], 11, "bold")).pack(side="left", padx=(8, 12))
    conn_label = ttk.Label(header, text=client.tr.desc, style="Dim.TLabel")
    conn_label.pack(side="left")

    fps_label = ttk.Label(header, text="—", style="Value.TLabel")
    fps_label.pack(side="right")

    # ── тело: экран слева, панель справа ─────────────────────────────
    body = ttk.Frame(root, padding=(12, 12, 12, 0))
    body.pack(fill="both", expand=True)
    body.columnconfigure(0, weight=1)
    body.columnconfigure(1, minsize=250)
    body.rowconfigure(0, weight=1)

    screen_wrap = tk.Frame(body, bg=UI["screen"], highlightthickness=1,
                           highlightbackground=UI["line"])
    screen_wrap.grid(row=0, column=0, sticky="nsew")

    canvas = tk.Canvas(screen_wrap, bg=UI["screen"], highlightthickness=0,
                       cursor="crosshair")
    canvas.pack(fill="both", expand=True)
    img_id = canvas.create_image(0, 0, anchor="nw")
    hint_id = canvas.create_text(10, 10, anchor="nw", text="ожидание кадра…",
                                 fill=UI["fg_dim"], font=FONT_UI_SMALL)

    side = ttk.Frame(body, style="Panel.TFrame", padding=12)
    side.grid(row=0, column=1, sticky="nsew", padx=(12, 0))

    ttk.Label(side, text="ТРАНСЛЯЦИЯ", style="Title.TLabel").pack(anchor="w")

    stream_btn = ttk.Button(side, text="Остановить", style="Accent.TButton")
    stream_btn.pack(fill="x", pady=(6, 10))

    def add_combo(title, values, initial, on_change):
        ttk.Label(side, text=title, style="Dim.TLabel").pack(anchor="w")
        var = tk.StringVar(value=str(initial))
        cb = ttk.Combobox(side, textvariable=var, values=values,
                          state="readonly", font=FONT_UI_SMALL, width=10)
        cb.pack(fill="x", pady=(2, 8))
        cb.bind("<<ComboboxSelected>>", lambda _e: on_change(var.get()))
        return var

    def set_fmt(v):
        state["fmt"] = v
        restart_stream()

    def set_scale(v):
        state["scale"] = int(v)
        restart_stream()

    def set_fps(v):
        state["fps"] = int(v)
        restart_stream()

    def set_src(v):
        """Переключение gui/test прямо из окна.

        Стрим перезапускаем: у источников разное разрешение (интерфейс 480x320,
        fbtest 240x160), и без нового FB_REQ первый кадр после переключения
        приезжает с прежним размером в заголовке.
        """
        client.set_source(v)
        log_write(f"[i] источник кадров: {v}\n", "dim")
        restart_stream()

    add_combo("источник кадров", ["gui", "test"], args.src or "gui", set_src)
    add_combo("формат пикселя", list(PIXFMT), args.fmt, set_fmt)
    add_combo("прореживание", ["1", "2", "4"], args.scale, set_scale)
    add_combo("кадров в секунду", ["1", "5", "10", "15", "30", "60"],
              args.fps, set_fps)

    ttk.Separator(side, orient="horizontal").pack(fill="x", pady=8)
    ttk.Label(side, text="УСТРОЙСТВО", style="Title.TLabel").pack(anchor="w")

    info_rows = {}
    for key, title in (("res", "разрешение"), ("frames", "кадров"),
                       ("crc", "ошибок CRC"), ("resync", "ресинков")):
        row = ttk.Frame(side, style="Panel.TFrame")
        row.pack(fill="x", pady=1)
        ttk.Label(row, text=title, style="Dim.TLabel").pack(side="left")
        lbl = ttk.Label(row, text="—", style="Value.TLabel")
        lbl.pack(side="right")
        info_rows[key] = lbl

    ttk.Separator(side, orient="horizontal").pack(fill="x", pady=8)
    ttk.Label(side, text="КНОПКИ ПЛАТЫ", style="Title.TLabel").pack(anchor="w")

    btn_row = ttk.Frame(side, style="Panel.TFrame")
    btn_row.pack(fill="x", pady=(6, 0))
    for i in range(3):
        b = ttk.Button(btn_row, text=f"BTN {i}", width=6)
        b.pack(side="left", expand=True, fill="x", padx=(0 if i == 0 else 4, 0))
        b.bind("<ButtonPress-1>", lambda _e, n=i: client.button(n, True))
        b.bind("<ButtonRelease-1>", lambda _e, n=i: client.button(n, False))

    ttk.Separator(side, orient="horizontal").pack(fill="x", pady=8)
    ttk.Label(side, text="СВЯЗЬ", style="Title.TLabel").pack(anchor="w")

    link_row = ttk.Frame(side, style="Panel.TFrame")
    link_row.pack(fill="x", pady=(6, 0))
    ttk.Button(link_row, text="PING", command=lambda: client.tr.send(T_PING)) \
        .pack(side="left", expand=True, fill="x")
    ttk.Button(link_row, text="HELLO", command=client.hello) \
        .pack(side="left", expand=True, fill="x", padx=(4, 0))

    if client.tr.kind == "serial":
        def do_reset():
            client.tr._release_boot0()
            log_write("[OK] плата перезапущена\n", "ok")
            client.hello()
            if state["streaming"]:
                restart_stream()

        ttk.Button(side, text="Перезапустить плату", command=do_reset) \
            .pack(fill="x", pady=(6, 0))

    # ── консоль ──────────────────────────────────────────────────────
    console = ttk.Frame(root, style="Panel.TFrame", padding=(12, 8))
    console.pack(fill="both", expand=True, padx=12, pady=12)

    ctop = ttk.Frame(console, style="Panel.TFrame")
    ctop.pack(fill="x")
    ttk.Label(ctop, text="КОНСОЛЬ", style="Title.TLabel").pack(side="left")

    auto_var = tk.BooleanVar(value=True)
    ttk.Checkbutton(ctop, text="автопрокрутка", variable=auto_var,
                    command=lambda: state.update(autoscroll=auto_var.get())) \
        .pack(side="right")
    ttk.Button(ctop, text="Очистить",
               command=lambda: (log.configure(state="normal"),
                                log.delete("1.0", "end"),
                                log.configure(state="disabled"))) \
        .pack(side="right", padx=(0, 8))

    log_wrap = ttk.Frame(console, style="Panel.TFrame")
    log_wrap.pack(fill="both", expand=True, pady=(6, 6))

    scroll = ttk.Scrollbar(log_wrap, orient="vertical")
    scroll.pack(side="right", fill="y")

    log = tk.Text(log_wrap, height=9, font=FONT_MONO_SMALL, bg="#12141a",
                  fg=UI["fg"], insertbackground=UI["fg"], wrap="word",
                  relief="flat", padx=8, pady=6, yscrollcommand=scroll.set,
                  state="disabled")
    log.pack(side="left", fill="both", expand=True)
    scroll.config(command=log.yview)

    log.tag_config("ok", foreground=UI["ok"])
    log.tag_config("warn", foreground=UI["warn"])
    log.tag_config("err", foreground=UI["err"])
    log.tag_config("cmd", foreground=UI["accent"])
    log.tag_config("dim", foreground=UI["fg_dim"])

    entry_row = ttk.Frame(console, style="Panel.TFrame")
    entry_row.pack(fill="x")

    ttk.Label(entry_row, text="orca>", style="Value.TLabel").pack(side="left",
                                                                  padx=(0, 6))
    entry = tk.Entry(entry_row, font=FONT_MONO, bg=UI["panel_hi"],
                     fg=UI["fg"], insertbackground=UI["accent"],
                     relief="flat", highlightthickness=1,
                     highlightbackground=UI["line"],
                     highlightcolor=UI["accent"])
    entry.pack(side="left", fill="x", expand=True, ipady=5)
    entry.focus_set()

    def log_write(s: str, tag: str = ""):
        if not s:
            return
        log.configure(state="normal")
        log.insert("end", s, tag or ())
        # Без верхней границы Text растёт неограниченно: за час трансляции
        # с болтливой прошивкой это сотни мегабайт в памяти процесса.
        if int(log.index("end-1c").split(".")[0]) > 2000:
            log.delete("1.0", "500.0")
        log.configure(state="disabled")
        if state["autoscroll"]:
            log.see("end")

    # ── трансляция ───────────────────────────────────────────────────

    def restart_stream():
        if state["streaming"]:
            client.stream_on(PIXFMT[state["fmt"]], state["scale"], state["fps"])

    def toggle_stream():
        state["streaming"] = not state["streaming"]
        if state["streaming"]:
            client.stream_on(PIXFMT[state["fmt"]], state["scale"], state["fps"])
            stream_btn.config(text="Остановить", style="Accent.TButton")
            canvas.itemconfig(hint_id, text="ожидание кадра…")
            log_write("[i] трансляция включена\n", "dim")
        else:
            client.stream_off()
            stream_btn.config(text="Включить", style="TButton")
            state["frame_times"].clear()
            fps_label.config(text="—")
            # show() гасит подсказку на первом кадре и обратно её не включает:
            # без этой строки после остановки на экране остаётся последний
            # кадр и ни одного признака, что трансляции уже нет.
            canvas.itemconfig(hint_id, text="трансляция выключена")
            log_write("[i] трансляция выключена\n", "dim")

    stream_btn.config(command=toggle_stream)

    # ── отрисовка кадра ──────────────────────────────────────────────

    def draw_area():
        """Область под картинку с пропорциями экрана устройства.

        Возвращает (x, y, w, h) в координатах canvas. Letterbox по центру:
        растягивать кадр на всю ширину нельзя — исказятся пропорции, и
        попадание тапов уедет вместе с картинкой.
        """
        cw = max(canvas.winfo_width(), 1)
        ch = max(canvas.winfo_height(), 1)
        scale = min(cw / SCREEN_W, ch / SCREEN_H)
        w = max(int(SCREEN_W * scale), 1)
        h = max(int(SCREEN_H * scale), 1)
        return (cw - w) // 2, (ch - h) // 2, w, h

    def show(fb, w, h, fmt):
        if w <= 0 or h <= 0:
            return
        state["last_w"], state["last_h"] = w, h
        ax, ay, aw, ah = draw_area()
        rgb = to_rgb888(fb, w, h, fmt)

        if have_pil:
            im = Image.frombytes("RGB", (w, h), rgb)
            if (w, h) != (aw, ah):
                im = im.resize((aw, ah), Image.NEAREST)
            photo = ImageTk.PhotoImage(im)
        else:
            # Без Pillow: PPM + base64 — единственный формат, который Tk
            # понимает из памяти. zoom() умеет только целое увеличение,
            # поэтому нецелый масштаб оставляем как есть.
            ppm = b"P6\n%d %d\n255\n" % (w, h) + rgb
            photo = tk.PhotoImage(data=base64.b64encode(ppm))
            kx, ky = aw // w, ah // h
            k = max(min(kx, ky), 1)
            if k > 1:
                photo = photo.zoom(k, k)
            ax, ay = (canvas.winfo_width() - w * k) // 2, \
                     (canvas.winfo_height() - h * k) // 2

        state["img"] = photo
        canvas.itemconfig(img_id, image=photo)
        canvas.coords(img_id, max(ax, 0), max(ay, 0))
        canvas.itemconfig(hint_id, text="")

        now = time.time()
        ft = state["frame_times"]
        ft.append(now)
        # Мгновенный fps по последней секунде: средний за всё время
        # (client.fps()) после паузы в трансляции показывает ерунду.
        while ft and now - ft[0] > 1.0:
            ft.pop(0)
        fps_label.config(text=f"{len(ft):>2} fps   {w}×{h}")

        info_rows["res"].config(text=f"{w}×{h}")
        info_rows["frames"].config(text=str(client.frames))
        info_rows["crc"].config(text=str(client.parser.crc_err))
        info_rows["resync"].config(text=str(client.parser.resync))
        if client.parser.crc_err:
            info_rows["crc"].config(foreground=UI["warn"])

    def on_resize(_e):
        # Кадр перерисуется сам на следующем FB_END; сдвигаем текущую
        # картинку, чтобы при растягивании окна она не липла к углу.
        ax, ay, _aw, _ah = draw_area()
        canvas.coords(img_id, max(ax, 0), max(ay, 0))

    canvas.bind("<Configure>", on_resize)

    # ── ввод ─────────────────────────────────────────────────────────
    #
    # Кнопка зажата -> down, движение с зажатой -> move, отпускание -> up.
    # Ровно те же три действия, что даст настоящая панель, поэтому модуль,
    # написанный под мышь, потом заработает с тачскрином без правок.

    def to_device(e):
        ax, ay, aw, ah = draw_area()
        if aw <= 0 or ah <= 0:
            return 0, 0
        x = (e.x - ax) / aw * SCREEN_W
        y = (e.y - ay) / ah * SCREEN_H
        return (max(0, min(int(x), SCREEN_W - 1)),
                max(0, min(int(y), SCREEN_H - 1)))

    last_move = {"t": 0.0}

    def on_press(e):
        x, y = to_device(e)
        client.touch(x, y, TOUCH_DOWN)

    def on_drag(e):
        # Мышь даёт ~120 событий в секунду, каждое — отдельный кадр в линию.
        # На UART 921600 это заметная доля полосы, которую отъедает у картинки.
        now = time.time()
        if now - last_move["t"] < 0.025:
            return
        last_move["t"] = now
        x, y = to_device(e)
        client.touch(x, y, TOUCH_MOVE)

    def on_release(e):
        x, y = to_device(e)
        client.touch(x, y, TOUCH_UP)

    canvas.bind("<Button-1>", on_press)
    canvas.bind("<B1-Motion>", on_drag)
    canvas.bind("<ButtonRelease-1>", on_release)

    history = []
    hist_pos = {"i": 0}

    def on_enter(_event):
        cmd = entry.get().strip()
        entry.delete(0, "end")
        if not cmd:
            return
        history.append(cmd)
        hist_pos["i"] = len(history)
        log_write(f"orca> {cmd}\n", "cmd")
        client.shell(cmd)

    def on_history(delta):
        if not history:
            return
        hist_pos["i"] = max(0, min(len(history), hist_pos["i"] + delta))
        entry.delete(0, "end")
        if hist_pos["i"] < len(history):
            entry.insert(0, history[hist_pos["i"]])

    entry.bind("<Return>", on_enter)
    entry.bind("<Up>", lambda _e: (on_history(-1), "break")[1])
    entry.bind("<Down>", lambda _e: (on_history(1), "break")[1])

    # Цифры 1..8 в любом месте окна — кнопки платы, кроме случая, когда
    # пользователь набирает команду.
    def key_down(e):
        if e.widget is not entry and e.char.isdigit() and e.char != "0":
            client.button(int(e.char) - 1, True)

    def key_up(e):
        if e.widget is not entry and e.char.isdigit() and e.char != "0":
            client.button(int(e.char) - 1, False)

    root.bind("<KeyPress>", key_down)
    root.bind("<KeyRelease>", key_up)

    # ── подключение колбэков ─────────────────────────────────────────

    def set_dot(color):
        dot.itemconfig(dot_id, fill=color)

    client.on_frame = show
    client.on_text = lambda s: log_write(s)
    client.on_shell = lambda st, t: log_write(
        (t if t.endswith("\n") or not t else t + "\n"),
        "err" if st != 0 else "")
    client.on_hello = lambda s: (log_write(f"[OK] {s}\n", "ok"),
                                 set_dot(UI["ok"]),
                                 conn_label.config(text=s))[0]
    client.on_error = lambda s: (log_write(f"[!] устройство: {s}\n", "err"),
                                 set_dot(UI["err"]))[0]

    client.hello()
    if args.src:
        client.set_source(args.src)
        log_write(f"[i] источник кадров: {args.src}\n", "dim")
    client.stream_on(PIXFMT[state["fmt"]], state["scale"], state["fps"])
    log_write(f"[OK] стрим: {state['fmt']} scale={state['scale']} "
              f"fps={state['fps']}\n", "ok")
    if client.log_file is not None:
        log_write(f"[OK] лог: {client.log_file.name}\n", "ok")
    log_write("[i] клик мышью = тап, цифры 1..8 = кнопки платы, "
              "стрелки вверх/вниз = история команд\n", "dim")

    alive = {"run": True}

    def pump():
        """Опрос линии из главного цикла Tk.

        Блокирующего select здесь быть не может — он бы заморозил
        отрисовку окна. Читаем без ожидания и возвращаем управление Tk.
        """
        if not alive["run"]:
            return
        try:
            client.poll(0)
        except Exception as exc:
            log_write(f"[!] {exc}\n", "err")
            set_dot(UI["err"])
        root.after(10, pump)

    def watchdog():
        """Индикатор связи гаснет, если кадров нет дольше двух секунд.

        Без этого зелёная точка после обрыва линии остаётся зелёной, и
        выглядит это так, будто всё в порядке.
        """
        if not alive["run"]:
            return
        ft = state["frame_times"]
        if state["streaming"]:
            if ft and time.time() - ft[-1] < 2.0:
                set_dot(UI["ok"])
            else:
                set_dot(UI["warn"])
                fps_label.config(text="нет кадров")
        root.after(1000, watchdog)

    def on_close():
        alive["run"] = False
        try:
            client.stream_off()
        except Exception:
            pass
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", on_close)
    root.after(10, pump)
    root.after(1000, watchdog)
    root.mainloop()
    return 0


# ─── одиночные операции ──────────────────────────────────────────────

def run_shot(client: Client, args) -> int:
    done = {"rc": 1}

    def on_frame(fb, w, h, fmt):
        if save_png(args.shot, fb, w, h, fmt):
            print(f"[OK] кадр {w}x{h} -> {args.shot}", file=sys.stderr)
            done["rc"] = 0
        done["stop"] = True

    client.on_frame = on_frame
    if args.src:
        client.set_source(args.src)
    client.stream_on(PIXFMT[args.fmt], args.scale, args.fps)

    deadline = time.time() + 10
    while "stop" not in done:
        if time.time() > deadline:
            print("[!] таймаут — кадр не пришёл", file=sys.stderr)
            return 1
        client.poll(0.05)
    return done["rc"]


def run_once(client: Client, args) -> int:
    done = {}

    def on_shell(status, text):
        sys.stdout.write(text)
        sys.stdout.flush()
        done["rc"] = status

    client.on_shell = on_shell
    client.on_error = lambda s: print(f"[!] устройство: {s}", file=sys.stderr)
    client.shell(args.shell)

    deadline = time.time() + 10
    while "rc" not in done:
        if time.time() > deadline:
            print("[!] таймаут — устройство молчит", file=sys.stderr)
            return 1
        client.poll(0.05)
    return done["rc"]


# ─── main ────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(
        description="Orca Link — консоль и экран устройства",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__)
    ap.add_argument("uri", help="/dev/ttyUSB0[@baud] или host:port")
    ap.add_argument("--stream", choices=("console", "gui", "term"),
                    default="term", help="режим работы (по умолчанию term)")
    ap.add_argument("--fps", type=int, default=10, help="кадров в секунду (1..60)")
    ap.add_argument("--scale", type=int, default=2,
                    help="прореживание на устройстве: 1=1:1, 2=1/2, 4=1/4")
    ap.add_argument("--fmt", choices=list(PIXFMT), default="rgb565")
    ap.add_argument("--src", choices=("gui", "test"),
                    help="источник кадров: gui (LVGL) или test (fbtest); "
                         "без флага выбор устройства не меняется")
    ap.add_argument("--log", metavar="ФАЙЛ", nargs="?", const="",
                    help="писать вывод устройства в файл (без имени — "
                         "orca-ГГГГММДД-ЧЧММСС.log)")
    ap.add_argument("--no-reset", action="store_true",
                    help="не перезапускать плату и не ждать её загрузку "
                         "(сам reset при открытии порта CH340 всё равно даёт, "
                         "но boot-лог в этом режиме в файл не попадёт)")
    ap.add_argument("--shot", metavar="PNG", help="сохранить один кадр и выйти")
    ap.add_argument("--shell", metavar="CMD", help="выполнить команду и выйти")
    args = ap.parse_args()

    tr = Transport(args.uri, reset=not args.no_reset)
    print(f"[OK] подключено: {tr.desc}", file=sys.stderr)

    # Лог открываем после транспорта: пустой файл от неудачного подключения
    # только мешает потом искать нужный.
    log_file = None
    if args.log is not None:
        path = args.log or time.strftime("orca-%Y%m%d-%H%M%S.log")
        # Дозапись, а не перезапись: имя лога можно задать своё и гонять
        # клиент несколько раз, не теряя предыдущий запуск.
        log_file = open(path, "a", encoding="utf-8", errors="replace")
        log_file.write(time.strftime("\n=== %Y-%m-%d %H:%M:%S ")
                       + f"{tr.desc} stream={args.stream}"
                       + (f" src={args.src}" if args.src else "") + " ===\n")
        log_file.flush()
        print(f"[OK] лог: {path}", file=sys.stderr)

    client = Client(tr, log_file)

    # Плату отпускаем последней: до этой строки лог уже открыт, поэтому в файл
    # попадает всё с первой строки "=== Orca OS starting ===".
    tr.start()

    try:
        if args.shell:
            return run_once(client, args)
        if args.shot:
            return run_shot(client, args)
        if args.stream == "console":
            return run_console(client, args)
        if args.stream == "gui":
            return run_gui(client, args)
        return run_term(client, args)
    except KeyboardInterrupt:
        return 0
    finally:
        if args.stream == "term" and not (args.shell or args.shot):
            sys.stdout.write(RESET + "\n")
        try:
            client.stream_off()
        except Exception:
            pass
        tr.close()
        if log_file is not None:
            log_file.close()


if __name__ == "__main__":
    sys.exit(main())
