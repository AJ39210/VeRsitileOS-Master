CC ?= cc
BUILD_DIR := .build
VCC := $(BUILD_DIR)/vcc

.PHONY: all test clean

all: $(VCC)

$(BUILD_DIR):
	mkdir -p $@

$(VCC): vcc.c | $(BUILD_DIR)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $<

test: $(VCC)
	$(VCC) -S tests/basic.c > $(BUILD_DIR)/basic.asm
	$(VCC) tests/basic.c -o $(BUILD_DIR)/basic
	python3 tests/prepare_host_elf.py $(BUILD_DIR)/basic
	$(BUILD_DIR)/basic

clean:
	rm -rf $(BUILD_DIR)
