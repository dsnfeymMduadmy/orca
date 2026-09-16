cd #!/usr/bin/env bash
source ~/esp/esp-idf/export.sh
#
# Сборка прошивки Orca ESP (ESP32-C3, Seeed XIAO) и прошивка по USB.
#
#   ./build-flash-esp.sh                 сборка + прошивка
#   ./build-flash-esp.sh -b              только сборка
#   ./build-flash-esp.sh -f              только прошивка (что лежит в build/)
#   ./build-flash-esp.sh -m              прошивка + сразу монитор
#
set -euo pipefail

ESP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/firmware/esp32"
BUILD_DIR="$ESP_DIR/build"
TARGET="esp32c3"

# Консоль и прошивка идут через нативный USB-C (USB_SERIAL_JTAG), поэтому
# порт — ttyACM*, а не ttyUSB*. esptool сам дёргает RTS/DTR, кнопки BOOT
# нажимать не нужно.
PORT="${ORCA_ESP_PORT:-/dev/ttyACM0}"

DO_BUILD=1
DO_FLASH=1
DO_MONITOR=0
DO_CLEAN=0

msg()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m warn:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '3,8p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
    cat <<'EOF'

Опции:
  -b, --build-only   собрать, не прошивать
  -f, --flash-only   прошить готовый build/ (без пересборки)
  -m, --monitor      после прошивки открыть монитор (idf.py monitor)
  -p PORT            порт прошивки (по умолчанию /dev/ttyACM0,
                     либо переменная окружения ORCA_ESP_PORT)
  -c, --clean        удалить build/ и sdkconfig перед сборкой
  -h, --help         эта справка

Требуется окружение ESP-IDF: source $IDF_PATH/export.sh (или . $IDF_PATH/export.sh
в текущем шелле). Цель — esp32c3, плата Seeed XIAO ESP32-C3.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--build-only) DO_FLASH=0 ;;
        -f|--flash-only) DO_BUILD=0 ;;
        -m|--monitor)    DO_MONITOR=1 ;;
        -p)              shift; [[ $# -gt 0 ]] || die "-p требует порт"; PORT="$1" ;;
        -c|--clean)      DO_CLEAN=1 ;;
        -h|--help)       usage; exit 0 ;;
        *)               die "неизвестная опция: $1 (см. --help)" ;;
    esac
    shift
done

command -v idf.py >/dev/null 2>&1 \
    || die "idf.py не найден в PATH — сначала source \$IDF_PATH/export.sh"

if (( DO_CLEAN )); then
    msg "удаляю $BUILD_DIR и sdkconfig"
    rm -rf "$BUILD_DIR" "$ESP_DIR/sdkconfig"
fi

if (( DO_BUILD )); then
    # set-target затирает sdkconfig, поэтому зовём его только когда конфига
    # ещё нет или цель в нём не совпадает.
    if [[ ! -f "$ESP_DIR/sdkconfig" ]] \
            || ! grep -q "^CONFIG_IDF_TARGET=\"$TARGET\"" "$ESP_DIR/sdkconfig"; then
        msg "конфигурирую цель $TARGET"
        (cd "$ESP_DIR" && idf.py set-target "$TARGET")
    fi

    msg "собираю"
    (cd "$ESP_DIR" && idf.py build)
fi

if (( DO_FLASH )); then
    [[ -d "$BUILD_DIR" ]] || die "нет $BUILD_DIR — сначала собери (без -f)"
    [[ -e "$PORT" ]] || warn "порт $PORT не найден: подключи плату по USB-C"

    msg "прошиваю в $PORT"
    (cd "$ESP_DIR" && idf.py -p "$PORT" flash)
    msg "прошито"
fi

if (( DO_MONITOR )); then
    msg "монитор $PORT (Ctrl-] — выход)"
    cd "$ESP_DIR" && exec idf.py -p "$PORT" monitor
fi
