cmake_minimum_required(VERSION 3.20)

project(orca_kernel C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(
    USE_HAL_DRIVER
    STM32H743xx
    ARM_MATH_CM7
)

add_compile_options(
    -Wall
    -Wextra
    -Wno-unused-parameter
    $<$<CONFIG:Debug>:-Og>
    $<$<CONFIG:Debug>:-g3>
    $<$<CONFIG:Release>:-O2>
)

include(FetchContent)

FetchContent_Declare(
    stm32h7xx_hal
    GIT_REPOSITORY https://github.com/STMicroelectronics/stm32h7xx_hal_driver.git
    GIT_TAG v1.11.2
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    cmsis_device_h7
    GIT_REPOSITORY https://github.com/STMicroelectronics/cmsis_device_h7.git
    GIT_TAG v1.10.4
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    cmsis_core
    GIT_REPOSITORY https://github.com/ARM-software/CMSIS_5.git
    GIT_TAG 5.9.0
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    freertos_kernel
    GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
    GIT_TAG V11.1.0
    GIT_SHALLOW TRUE
)

FetchContent_Declare(
    fatfs
    URL http://elm-chan.org/fsw/ff/arc/ff15.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_Declare(
    lvgl
    GIT_REPOSITORY https://github.com/lvgl/lvgl.git
    GIT_TAG v9.5.0
    GIT_SHALLOW TRUE
)

#
# USB Device middleware — тот же код, что CubeMX кладёт в Middlewares/ST.
# Над ним работает orca_usb_msc: три LUN'а (FLASH ядра, QSPI, SD-карта).
# BOT/SCSI берём целиком из апстрима: это не тот код, который имеет смысл
# писать самому — хосты придирчивы к мелочам протокола.
#
FetchContent_Declare(
    stm32_usb_device
    GIT_REPOSITORY https://github.com/STMicroelectronics/stm32_mw_usb_device.git
    GIT_TAG v2.11.6
    GIT_SHALLOW TRUE
)

# CMSIS-заголовки нужны freertos_config ниже, поэтому их достаём первыми.
FetchContent_MakeAvailable(stm32h7xx_hal cmsis_device_h7 cmsis_core fatfs stm32_usb_device)

#
# FreeRTOS-Kernel V11 требует INTERFACE-библиотеку freertos_config с путём к
# FreeRTOSConfig.h — она должна существовать до MakeAvailable(freertos_kernel).
#
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc
    ${cmsis_device_h7_SOURCE_DIR}/Include
    ${cmsis_core_SOURCE_DIR}/CMSIS/Core/Include
)
target_compile_definitions(freertos_config INTERFACE STM32H743xx)

set(FREERTOS_PORT GCC_ARM_CM7 CACHE STRING "" FORCE)
set(FREERTOS_HEAP 4 CACHE STRING "" FORCE)

FetchContent_MakeAvailable(freertos_kernel)

#
# LVGL. Тянется после FreeRTOS: при LV_USE_OS = LV_OS_FREERTOS его src/osal
# подключает FreeRTOS.h/task.h/semphr.h/atomic.h, то есть цель lvgl обязана
# видеть заголовки ядра — отсюда link на freertos_kernel.
#
# lv_conf.h лежит в Core/Inc и передаётся через LV_BUILD_CONF_PATH: складывать
# его рядом с исходниками LVGL (как предлагает апстрим) нельзя — дерево _deps
# перезаписывается при каждом обновлении тега.
#
# Имена переменных взяты из env_support/cmake/os_desktop.cmake версии 9.5.
# В v8 они назывались LV_CONF_PATH / LV_CONF_BUILD_DISABLE_*, и с теми именами
# конфигурация падает: LVGL ищет lv_conf.h в корне проекта и не находит.
#
set(LV_BUILD_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc/lv_conf.h CACHE PATH "" FORCE)
set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(lvgl)

target_link_libraries(lvgl PUBLIC freertos_kernel)

#
# Чужой код собираем без наших -Wall -Wextra: add_compile_options выше
# действует и на подкаталоги, а исправлять предупреждения апстрима мы всё
# равно не будем — они только маскируют предупреждения в своём коде.
#
target_compile_options(lvgl PRIVATE -w)

set(HAL_SOURCES
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_cortex.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_rcc.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_rcc_ex.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_pwr.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_pwr_ex.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_gpio.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_dma.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_dma_ex.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_mdma.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_rtc.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_rtc_ex.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_uart.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_uart_ex.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_sdram.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_sd.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_sd_ex.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_qspi.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_pcd.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_hal_pcd_ex.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_ll_usb.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_ll_fmc.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_ll_sdmmc.c
    ${stm32h7xx_hal_SOURCE_DIR}/Src/stm32h7xx_ll_delayblock.c
)

set(FATFS_SOURCES
    ${fatfs_SOURCE_DIR}/source/ff.c
    ${fatfs_SOURCE_DIR}/source/ffsystem.c
    ${fatfs_SOURCE_DIR}/source/ffunicode.c
)

set(USBD_SOURCES
    ${stm32_usb_device_SOURCE_DIR}/Core/Src/usbd_core.c
    ${stm32_usb_device_SOURCE_DIR}/Core/Src/usbd_ctlreq.c
    ${stm32_usb_device_SOURCE_DIR}/Core/Src/usbd_ioreq.c
    ${stm32_usb_device_SOURCE_DIR}/Class/MSC/Src/usbd_msc.c
    ${stm32_usb_device_SOURCE_DIR}/Class/MSC/Src/usbd_msc_bot.c
    ${stm32_usb_device_SOURCE_DIR}/Class/MSC/Src/usbd_msc_scsi.c
    ${stm32_usb_device_SOURCE_DIR}/Class/MSC/Src/usbd_msc_data.c
)

#
# ff.h подключает конфиг как #include "ffconf.h", а такой include ищется
# в первую очередь рядом с самим ff.h. Никакой -I на Core/Inc это не
# перебивает: собирался бы штатный ffconf из архива (FF_USE_LFN 0,
# FF_VOLUMES 1, exFAT off), а наш файл молча не участвовал бы в сборке.
# Поэтому кладём свой конфиг прямо в дерево FatFs.
#
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/Core/Inc/ffconf.h
    ${fatfs_SOURCE_DIR}/source/ffconf.h
    COPYONLY
)

file(GLOB_RECURSE CORE_SOURCES "Core/Src/*.c")
file(GLOB_RECURSE KERNEL_SOURCES "OrcaKernel/*/*.c")

add_executable(${PROJECT_NAME}
    ${CORE_SOURCES}
    ${KERNEL_SOURCES}
    ${HAL_SOURCES}
    ${FATFS_SOURCES}
    ${USBD_SOURCES}
    Drivers/system_stm32h7xx.c
    Drivers/startup_stm32h743xx.s
)

target_include_directories(${PROJECT_NAME} PRIVATE
    Core/Inc
    OrcaKernel/api
    OrcaKernel/fs
    OrcaKernel/gui
    OrcaKernel/hal
    OrcaKernel/link
    OrcaKernel/loader
    OrcaKernel/services
    OrcaKernel/shell
    ${stm32h7xx_hal_SOURCE_DIR}/Inc
    ${stm32h7xx_hal_SOURCE_DIR}/Inc/Legacy
    ${cmsis_device_h7_SOURCE_DIR}/Include
    ${cmsis_core_SOURCE_DIR}/CMSIS/Core/Include
    ${freertos_kernel_SOURCE_DIR}/include
    ${freertos_kernel_SOURCE_DIR}/portable/GCC/ARM_CM7/r0p1
    ${fatfs_SOURCE_DIR}/source
    ${stm32_usb_device_SOURCE_DIR}/Core/Inc
    ${stm32_usb_device_SOURCE_DIR}/Class/MSC/Inc
    Drivers
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    freertos_kernel
    lvgl
    m
)

target_link_options(${PROJECT_NAME} PRIVATE
    -T${CMAKE_SOURCE_DIR}/Linker/STM32H743IITx_FLASH.ld
    -Wl,-Map=${PROJECT_NAME}.map
)

set(HEX_FILE ${PROJECT_NAME}.hex)
set(BIN_FILE ${PROJECT_NAME}.bin)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${PROJECT_NAME}> ${HEX_FILE}
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}> ${BIN_FILE}
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}>
    COMMENT "Building ${HEX_FILE} and ${BIN_FILE}"
)

#
# Прошивка по DFU. Плата должна быть в режиме bootloader: BOOT0 в 1 при
# сбросе, USB подключён напрямую к MCU (не через CH340).
# Проверить, что устройство видно: dfu-util -l  -> ожидается 0483:df11.
#
add_custom_target(flash-dfu
    COMMAND dfu-util -a 0 -d 0483:df11 -s 0x08000000:leave -D ${BIN_FILE}
    DEPENDS ${PROJECT_NAME}
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Flashing ${BIN_FILE} via DFU to 0x08000000"
)
