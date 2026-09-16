#!/usr/bin/env bash
# Трансляция интерфейса устройства (GUI на LVGL) на ПК — одной командой.
#
#   ./orca_gui.sh                    порт найдётся сам, лог в logs/
#   ./orca_gui.sh /dev/ttyUSB1       порт указан явно
#   ./orca_gui.sh 192.168.4.1:3333   через ESP32
#   ./orca_gui.sh --scale 1          кадр 1:1 вместо 1/2 (медленнее)
#   ORCA_FPS=5 ORCA_SCALE=4 ./orca_gui.sh
#
# Скрипт делает то, что иначе приходится держать в голове:
#   1. находит порт платы (/dev/ttyUSB*, /dev/ttyACM*);
#   2. просит транслировать именно GUI, а не тестовую картинку fbtest
#      (--src gui, на устройстве это команда `fb src gui`);
#   3. складывает всё, что сказала прошивка, в logs/gui-ДАТА-ВРЕМЯ.log и
#      обновляет симлинк logs/gui-latest.log для `tail -f`.
#
# Лог начинается с загрузки: при подключении по UART клиент дёргает NRST
# (Transport в orca_view.py), поэтому в файл попадает всё с первой строки
# "=== Orca OS starting ===" — включая [WARN]/[ERROR] и жалобы самого LVGL,
# если GUI не поднялся. Не сбрасывать плату: ./orca_gui.sh --no-reset.
#
# Прореживание по умолчанию 1/2 (240x160): полный кадр 480x320 RGB565 — это
# 300 КБ, на 921600 бод они уходят больше трёх секунд. 1/2 даёт ~1 кадр в
# секунду, чего хватает, чтобы видеть интерфейс и попадать по кнопкам.
#
# Переменные окружения: ORCA_URI, ORCA_STREAM (gui|term|console), ORCA_FMT,
# ORCA_SCALE, ORCA_FPS, ORCA_LOG_DIR, PYTHON. Аргументы скрипта уходят в
# orca_view.py как есть и перебивают значения по умолчанию — argparse берёт
# последнее вхождение флага.

set -euo pipefail

here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
view="$here/orca_view.py"

usage() {
    # Справка — это шапка самого файла: два места с описанием флагов
    # разъезжаются на второй же правке.
    awk 'NR>1 && /^#/ { sub(/^# ?/, ""); print; next } NR>1 { exit }' "$0"
}

if [ "${1-}" = "-h" ] || [ "${1-}" = "--help" ]; then
    usage
    exit 0
fi

if [ ! -f "$view" ]; then
    echo "[!] рядом нет orca_view.py ($view)" >&2
    exit 1
fi

# ── адрес устройства ─────────────────────────────────────────────────
uri="${ORCA_URI-}"
if [ $# -gt 0 ]; then
    case "$1" in
        /*|*:[0-9]*) uri="$1"; shift ;;
    esac
fi

if [ -z "$uri" ]; then
    shopt -s nullglob
    ports=(/dev/ttyUSB* /dev/ttyACM*)
    shopt -u nullglob

    if [ ${#ports[@]} -eq 0 ]; then
        echo "[!] порт платы не найден (/dev/ttyUSB*, /dev/ttyACM*)" >&2
        echo "    подключи плату или укажи адрес: $0 /dev/ttyUSB0 | host:port" >&2
        exit 1
    fi
    uri="${ports[0]}"
    if [ ${#ports[@]} -gt 1 ]; then
        # Молча взять первый из нескольких — верный способ пять минут
        # смотреть на пустое окно, подключившись к чужому переходнику.
        echo "[i] портов несколько (${ports[*]}), беру $uri" >&2
    fi
fi

if [ -e "$uri" ] && [ ! -w "$uri" ]; then
    echo "[!] нет прав на $uri. Добавь себя в группу порта и перелогинься:" >&2
    echo "    sudo usermod -aG uucp \"$USER\"    # в Debian/Ubuntu: dialout" >&2
fi

# ── python и зависимости ─────────────────────────────────────────────
py="${PYTHON:-python3}"
if ! command -v "$py" >/dev/null 2>&1; then
    echo "[!] нет $py" >&2
    exit 1
fi

case "$uri" in
    /*)
        if ! "$py" -c 'import serial' >/dev/null 2>&1; then
            echo "[!] для UART нужен pyserial:" >&2
            echo "    pacman -S python-pyserial | pip install pyserial" >&2
            exit 1
        fi
        ;;
esac

# ── лог ──────────────────────────────────────────────────────────────
log_dir="${ORCA_LOG_DIR:-$here/logs}"
mkdir -p "$log_dir"
log="$log_dir/gui-$(date +%Y%m%d-%H%M%S).log"
# Симлинк относительный: каталог с логами можно перенести целиком.
ln -sfn "$(basename "$log")" "$log_dir/gui-latest.log"

echo "[OK] устройство: $uri"
echo "[OK] лог:        $log"
echo "[i]  живой лог:  tail -f $log_dir/gui-latest.log"

exec "$py" "$view" "$uri" \
    --stream "${ORCA_STREAM:-gui}" \
    --src gui \
    --fmt "${ORCA_FMT:-rgb565}" \
    --scale "${ORCA_SCALE:-2}" \
    --fps "${ORCA_FPS:-10}" \
    --log "$log" \
    "$@"
