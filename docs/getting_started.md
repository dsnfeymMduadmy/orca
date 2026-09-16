# Быстрый старт

## Зависимости

- `arm-none-eabi-gcc` (toolchain для Cortex-M7)
- `cmake` >= 3.20 и `ninja`
- `git` + сеть при первом конфигурировании: HAL, CMSIS, FreeRTOS и FatFs
  тянет `FetchContent` (версии зафиксированы в `firmware/CMakeLists.txt`)
- `dfu-util` для прошивки по USB либо ST-Link/J-Link + `openocd` для SWD

> Вендорный код в репозитории не лежит, кроме двух файлов, которые правятся
> под плату: `firmware/Drivers/startup_stm32h743xx.s` и
> `firmware/Drivers/system_stm32h7xx.c`. Всё остальное появляется в
> `firmware/build/_deps/` после первого `cmake -B build`.

## Сборка ядра (прошивка)

```sh
cd firmware
cmake -B build -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake
cmake --build build -j
```

Результат: `build/orca_kernel.elf` / `.bin`.

Прошивка по DFU (BOOT0 в 1 при сбросе, USB прямо в MCU, не через CH340):

```sh
cmake --build build --target flash-dfu
```

## Сборка модуля/приложения

Шаблон под свой модуль:

```sh
cd tools/module_sdk/template
make MODULE_NAME=my_module
```

Готовый пример — UART-мост к ESP:

```sh
cd tools/module_sdk/examples/esp_link
make                 # -> build/esp_link.orca
make check-symbols   # неразрешённые символы: все должны резолвиться ядром
```

`check-symbols` стоит прогонять до копирования на карту: символ, которого нет
в таблице `orca_loader.c`, виден только на часах и только как отказ запуска.

Копируем на SD:

```
/modules/esp_link/module.json
/modules/esp_link/esp_link.orca
```

## Разметка SD-карты

Отформатировать как FAT32, скопировать содержимое `sdcard_root/` как есть — это
референсный layout, который ожидает `AppManager`.

## Отладка

- Serial-консоль: CH340 (USB-UART), **921600** 8N1 — системный shell
  (`ls`, `run`, `ps`, `kill`, `free`). Скорость задана в
  `firmware/Core/Src/bsp_uart.c`: на 115200 трансляция framebuffer
  упирается в порт.
- Просмотр экрана с PC: `python3 tools/fbstream/orca_view.py`.
- SWD: 4-pin 2.0mm разъём, стандартная распиновка (SWDIO/SWCLK/GND/VCC).
