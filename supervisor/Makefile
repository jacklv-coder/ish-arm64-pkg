# Build the i386 musl-static PID1 supervisor with zig.
#
# Why zig: it ships a built-in cross toolchain for i386-linux-musl with
# no extra setup (no Docker, no musl-cross gcc compile). Install once:
#
#     brew install zig
#
# Override CC/CFLAGS to use a different toolchain if you have one.

# Use `make CC="..."` to override; default is zig cross.
ZIG     ?= zig
# Default guest is arm64 (matches OpenMinis/ish-arm64 fork; native on
# Apple Silicon). Override TARGET_TRIPLE for other configs.
TARGET_TRIPLE ?= aarch64-linux-musl
CC      = $(ZIG) cc -target $(TARGET_TRIPLE)
CFLAGS  = -static -Os -fno-stack-protector -fno-pic \
          -ffunction-sections -fdata-sections \
          -Wall -Wextra -Wno-unused-parameter \
          -I../protocol -I../include
LDFLAGS = -static -Wl,--gc-sections

OUT_DIR ?= ../../build/supervisor
TARGET  := $(OUT_DIR)/ishsv

SRCS := ishsv.c

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SRCS) ../protocol/proto.h
	@mkdir -p $(OUT_DIR)
	$(CC) $(CFLAGS) $(SRCS) -o $@ $(LDFLAGS)
	@echo "Built $@:"
	@file $@ 2>/dev/null || true
	@ls -lh $@

clean:
	rm -rf $(OUT_DIR)
