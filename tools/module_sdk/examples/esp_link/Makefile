# esp_link — пример модуля. Собирается тем же SDK, что и шаблон: подставляем
# свои пути и включаем шаблонный Makefile.
#
#   make                 -> build/esp_link.orca
#   make check-symbols   -> список неразрешённых символов
#   make clean

MODULE_NAME  := esp_link
SDK_DIR      := ../../template
ORCA_API_DIR := ../../../../firmware/OrcaKernel/api
LDSCRIPT     := $(SDK_DIR)/module.ld

include $(SDK_DIR)/Makefile
