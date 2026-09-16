# Settings module Makefile for tools/module_sdk/template
MODULE_NAME   := settings
SRC           := settings_module.c
OBJ           := build/$(MODULE_NAME).o
BUILD_DIR     := build/$(MODULE_NAME)

.PHONY: all clean check-symbols

all: $(BUILD_DIR)/$(MODULE_NAME).orca

$(BUILD_DIR):
    mkdir -p $(BUILD_DIR)

$(BUILD_DIR)/%.o: %.c | $(BUILD_DIR)
    $(CC) $(CFLAGS) -c $< -o $@

$(BUILD_DIR)/$(MODULE_NAME).orca: $(OBJ)
    $(LD) $(LDFLAGS) -o $@ $(OBJS)

clean:
    rm -rf $(BUILD_DIR)