#!/usr/bin/env bash
#
# Сборка ядра Orca и прошивка по DFU.
#
#   ./build-flash.sh                 сборка Debug + прошивка
#   ./build-flash.sh -b              только сборка
#   ./build-flash.sh -f              только прошивка (что лежит в build/)
#   ./build-flash.sh -r -c           Release с нуля + прошивка
#
set -euo pipefail

FIRMWARE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/firmware"
BUILD_DIR="$FIRMWARE_DIR/build"
TOOLCHAIN="$FIRMWARE_DIR/cmake/arm-none-eabi.cmake"
BIN_NAME="orca_kernel.bin"

DFU_VID_PID="0483:df11"
DFU_ALT=0
DFU_ADDR="0x08000000"

BUILD_TYPE="Debug"
DO_CONFIGURE=1
DO_BUILD=1
DO_FLASH=1
DO_CLEAN=0
JOBS="$(nproc 2>/dev/null || echo 4)"

msg()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m warn:\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

usage() {
    sed -n '3,9p' "${BASH_SOURCE[0]}" | sed 's/^# \?//'
    cat <<'EOF'

Опции:
  -b, --build-only   собрать, не прошивать
  -f, --flash-only   прошить готовый build/orca_kernel.bin
  -r, --release      CMAKE_BUILD_TYPE=Release (по умолчанию Debug)
  -c, --clean        удалить build/ перед сборкой (пере-выкачает _deps)
  -j N               параллельные джобы (по умолчанию nproc)
  -h, --help         эта справка

Прошивка требует плату в режиме bootloader: BOOT0 = 1 при сбросе, USB
подключён напрямую к MCU, а не через CH340.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        -b|--build-only) DO_FLASH=0 ;;
        -f|--flash-only) DO_CONFIGURE=0; DO_BUILD=0 ;;
        -r|--release)    BUILD_TYPE="Release" ;;
        -c|--clean)      DO_CLEAN=1 ;;
        -j)              shift; [[ $# -gt 0 ]] || die "-j требует число"; JOBS="$1" ;;
        -j*)             JOBS="${1#-j}" ;;
        -h|--help)       usage; exit 0 ;;
        *)               die "неизвестная опция: $1 (см. --help)" ;;
    esac
    shift
done

need() { command -v "$1" >/dev/null 2>&1 || die "не найден $1 — $2"; }

if (( DO_BUILD )); then
    need arm-none-eabi-gcc "toolchain для Cortex-M7"
    need cmake             "cmake >= 3.20"
    need ninja             "генератор Ninja"
fi
(( DO_FLASH )) && need dfu-util "утилита прошивки по USB DFU"

if (( DO_CLEAN )); then
    msg "удаляю $BUILD_DIR"
    rm -rf "$BUILD_DIR"
    DO_CONFIGURE=1
fi

# CMakeCache помнит toolchain и тип сборки: переконфигурируем, если тип сменился.
if (( DO_CONFIGURE )) && [[ -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cached="$(sed -n 's/^CMAKE_BUILD_TYPE:STRING=//p' "$BUILD_DIR/CMakeCache.txt")"
    [[ "$cached" == "$BUILD_TYPE" ]] || msg "тип сборки $cached -> $BUILD_TYPE"
fi

if (( DO_CONFIGURE )); then
    msg "конфигурирую ($BUILD_TYPE)"
    cmake -S "$FIRMWARE_DIR" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi

if (( DO_BUILD )); then
    msg "собираю -j$JOBS"
    cmake --build "$BUILD_DIR" -j "$JOBS"
fi

BIN="$BUILD_DIR/$BIN_NAME"

if (( DO_FLASH )); then
    [[ -f "$BIN" ]] || die "нет $BIN — сначала собери (без -f)"

    if ! dfu-util -l 2>/dev/null | grep -q "$DFU_VID_PID"; then
        warn "устройство $DFU_VID_PID не видно в dfu-util -l"
        warn "BOOT0 = 1 при сбросе, USB напрямую в MCU"
        read -r -p "нажми Enter когда плата в DFU (Ctrl-C — отмена) " _ \
            || die "ввод недоступен — подключи плату и запусти снова"
        dfu-util -l 2>/dev/null | grep -q "$DFU_VID_PID" \
            || die "устройство $DFU_VID_PID так и не появилось"
    fi

    msg "прошиваю $BIN_NAME ($(stat -c%s "$BIN") байт) в $DFU_ADDR"
    dfu-util -a "$DFU_ALT" -d "$DFU_VID_PID" -s "${DFU_ADDR}:leave" -D "$BIN"
    msg "готово, плата перезапущена (BOOT0 верни в 0)"
fi
