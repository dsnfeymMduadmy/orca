# Orca OS — план работ

## Milestone 0 — Bring-up
- [ ] Clock config (480 МГц, PLL, настройка FMC/SDMMC1/QSPI таймингов)
- [ ] Startup/linker под internal FLASH (2MB) + DTCM/AXI SRAM карта
- [ ] Инициализация SDRAM через FMC, memtest на старте (debug-режим)
- [ ] Инициализация QSPI FLASH (memory-mapped режим для чтения ресурсов)
- [ ] UART консоль через CH340 (printf redirect, простой shell: ls/run/ps/kill/free)
- [ ] FreeRTOS: тики, idle-task, базовые системные таски (heartbeat, watchdog feeder)

## Milestone 1 — Хранилище
- [ ] FatFs + SDMMC1 диск-драйвер (`diskio_sdmmc.c`)
- [ ] FatFs + QSPI диск-драйвер (для `/res` на внутренней флешке, если тоже FAT) либо
      простой read-only blob-фс для QSPI-ресурсов
- [ ] I2C EEPROM драйвер + простая key-value обёртка для настроек
- [ ] `orca_vfs` — единая точка монтирования (`SD:`, `QSPI:`, `EEPROM:`)

## Milestone 2 — Дисплей и ввод
- [ ] LTDC драйвер для LCD1 (40-pin RGB), double buffer в SDRAM
- [ ] SPI-драйвер для LCD2 (20-pin) + I2C touch controller
- [ ] Базовый GUI-движок: framebuffer, примитивы (rect/line/text), простые виджеты (список, кнопка)
- [ ] Системный launcher (главный экран со списком apps/modules с SD)

## Milestone 3 — Загрузчик модулей (ключевая фича)
- [x] `orca_elf.h` — минимальные ELF32 структуры
- [x] `orca_loader` — парсинг ELF, релокации ABS32/REL32/TARGET1, резолв внешних
      символов (мини-libc), поиск `orca_app_main`/`orca_app_stop`
- [x] Поддержка `R_ARM_THM_CALL`/`R_ARM_THM_JUMP24` (побитовая перекодировка BL/BLX) —
      `patch_thumb_branch()` в `orca_loader.c`. Обход через `-mlong-calls` больше не нужен
      и из `tools/module_sdk/template/Makefile` убран
- [x] `orca_api_t` v1 — зафиксировать набор функций (log/task/gui/storage/gpio/uart/spi/i2c/system/ipc)
- [ ] `AppManager` — сканирование `/modules`, `/apps`, парсинг манифестов, lifecycle (start/stop/list)
- [x] Allocator арены в SDRAM (first-fit free-list) для кода/данных/стека модулей —
      реализация есть, нужна интеграция с реальным FreeRTOS-стеком задачи модуля
- [x] `orca_api_table.c` — реализация функций таблицы поверх HAL/FreeRTOS.
      Не реализованы и честно отдают отказ: группы `spi`, `i2c`, `ipc`, большая часть
      `gui`, `uart` (ждёт драйвер в `bsp_uart.c`) — см. WARN-019 в warnings.txt
- [ ] Изоляция ошибок: перехват HardFault/MemManage от модуля → убить таск модуля, не ронять систему
- [ ] Watchdog per-task (таймаут на "зависший" модуль)
- [ ] Graceful stop: `orca_appmgr_stop` уведомляет задачу (`ORCA_STOP_NOTIFY_BIT`), ждёт
      `ORCA_APPMGR_STOP_GRACE_MS` и только потом снимает её; право на выгрузку забирает
      `claim_teardown()`. Осталось: ожидание через event group вместо фиксированной
      паузы, чтобы модуль гарантированно успел освободить GPIO/UART/SPI

## Milestone 4 — SDK для сторонних модулей
- [x] `tools/module_sdk` — Makefile/CMake шаблон, `module.ld`, пример `esp_link`
      (`tools/module_sdk/examples/esp_link/main.c`)
- [ ] `tools/elf2orca` — упаковка/валидация манифеста, контрольные суммы
- [ ] Документация API (см. `docs/module_api.md`) — держать в синхроне с `orca_api.h`

## Milestone 5 — Безопасность и устойчивость
- [ ] Контрольная сумма/подпись `.orca` файлов (защита от повреждённых/подменённых модулей)
- [ ] `permissions` из манифеста → runtime-проверки доступа к API-группам
- [ ] MPU-регионы: код/данные модуля без доступа к памяти ядра и других модулей
- [ ] Лимиты по памяти и CPU-квоты на модуль (защита от "прожорливого" модуля)

## Milestone 6 — UX
- [ ] Меню настроек (Wi-Fi/BLE креды в EEPROM, яркость, время)
- [ ] Менеджер модулей в GUI (список, установка с SD, удаление, логи модуля)
- [ ] Экран "О системе" (версия ядра, свободная память SDRAM/heap, версия SD layout)

## Backlog / идеи
- [ ] OTA обновление ядра через QSPI (двойной образ + fallback)
- [ ] IPC между модулями (например GUI app запрашивает данные у "esp" модуля)
- [ ] Поддержка второго дисплея как отдельного "режима" (компактные часы vs основной UI)
