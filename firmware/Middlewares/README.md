# Middlewares

Сторонние компоненты сюда **не коммитятся** — качаются/распаковываются отдельно,
чтобы не тащить в репозиторий чужой код и не ломать обновления.

Ожидаемая раскладка:

```
Middlewares/
  FreeRTOS/              — FreeRTOS Kernel v10.x (порт ARM_CM7/r0p1)
    include/
    portable/GCC/ARM_CM7/r0p1/
    portable/MemMang/heap_4.c
  FatFs/                 — ChaN FatFs R0.15+
    source/              (ff.c, ff.h, ffunicode.c, diskio.h)
    ffconf.h             — конфиг лежит в firmware/Core/Inc/ffconf.h
  STM32H7xx_HAL_Driver/  — ST HAL/LL драйверы для STM32H7
  CMSIS/
    Device/ST/STM32H7xx/ — startup_stm32h743xx.s, system_stm32h7xx.c, CMSIS device headers
    Include/             — core_cm7.h и т.п.
```

## Откуда брать

| Компонент | Источник |
|-----------|----------|
| FreeRTOS Kernel | github.com/FreeRTOS/FreeRTOS-Kernel (tag V10.6.x) |
| FatFs | elm-chan.org/fsw/ff |
| STM32H7 HAL + CMSIS | github.com/STMicroelectronics/STM32CubeH7 (`Drivers/`) |

Самый простой путь — сгенерировать пустой проект в STM32CubeMX под
STM32H743IIT6 (FreeRTOS + FatFs + SDMMC1 + FMC + QSPI + USART1) и скопировать
оттуда `Drivers/` и `Middlewares/`, а `Core/` взять из этого репозитория.

## Важные требования к конфигурации

- **FreeRTOS**: `configUSE_NEWLIB_REENTRANT`, куча `heap_4`, порт `ARM_CM7/r0p1`
  (для H7 обязательно r0p1 — там воркэраунд эрраты кеша).
- **FloatABI**: `-mfpu=fpv5-d16 -mfloat-abi=hard`. Должно совпадать с флагами
  сборки модулей в `tools/module_sdk/template/Makefile`, иначе загруженный
  модуль поедет по FPU-конвенции.
- **FatFs**: `FF_MAX_SS = 512`, `FF_USE_LFN = 1` (нужны длинные имена для
  `/modules/<name>/module.json`), `FF_VOLUMES >= 2` (SD + QSPI).
- **DMA-буферы SDMMC** должны лежать в некешируемой области либо требуют
  `SCB_CleanInvalidateDCache` вокруг обменов — см. MPU-конфиг в
  `firmware/Core/Src/board.c`.

## .gitignore

Каталог намеренно пуст в репозитории (кроме этого README) — добавьте
`firmware/Middlewares/*` в `.gitignore`, если не хотите случайно закоммитить
вендорные исходники.
