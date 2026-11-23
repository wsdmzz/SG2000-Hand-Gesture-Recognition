# collector — 8 寸屏本地采集器
#
# 主机侧（本 Makefile 默认）：
#   make test        编译 SDK 无关模块 + null 平台，跑冒烟测试
#                    （config/storage 95帧回归/state/ui/工具链对账/端到端）
#   make collector_null  只编 null 平台版（手动演练 UI/状态机用）
#
# 板侧（待 platform_cvitek.c 完成后启用，见 06_build/）：
#   用 riscv64-unknown-linux-musl 工具链 + sophpi cvi_mpi 头/库交叉编译，
#   核心六模块（config/storage/state/ui_model/main）与平台层解耦，无需改动。

CC      ?= gcc
CFLAGS  ?= -O2 -g
CFLAGS  += -std=gnu11 -Wall -Wextra -Iinclude
BUILD   := build

CORE_SRC := config.c storage.c state.c ui_model.c
CORE_OBJ := $(patsubst %.c,$(BUILD)/%.o,$(CORE_SRC))

.PHONY: all test clean
all: $(BUILD)/collector_null

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%.o: %.c include/collector.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/platform_null.o: tests/platform_null.c include/collector.h tests/test_jpeg.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/smoke_test.o: tests/smoke_test.c include/collector.h | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/collector_null: $(CORE_OBJ) $(BUILD)/main.o $(BUILD)/platform_null.o
	$(CC) $(CFLAGS) $^ -o $@

$(BUILD)/smoke_test: $(CORE_OBJ) $(BUILD)/smoke_test.o
	$(CC) $(CFLAGS) $^ -o $@

# 冒烟测试的 e2e 段用 system("./collector_null …")，须在 build/ 内运行
test: $(BUILD)/smoke_test $(BUILD)/collector_null
	cd $(BUILD) && ./smoke_test

clean:
	rm -rf $(BUILD)