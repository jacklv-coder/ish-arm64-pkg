# Build the AArch64 musl-static PID 1 supervisor with Zig.
#
# Why Zig: it ships a built-in cross toolchain for aarch64-linux-musl with
# no extra setup (no Docker, no musl-cross gcc compile). Install once:
#
#     brew install zig
#
# Override CC/CFLAGS to use a different toolchain if you have one.

# Use `make CC="..."` to override; default is zig cross.
ZIG     ?= zig
# The default target matches this package's arm64 guest ABI. Override
# TARGET_TRIPLE only for an explicitly different source-build configuration.
TARGET_TRIPLE ?= aarch64-linux-musl
CC      = $(ZIG) cc -target $(TARGET_TRIPLE)
CFLAGS  = -static -Os -fno-stack-protector -fno-pic -fno-pie \
          -mcpu=baseline \
          -ffunction-sections -fdata-sections \
          -Wall -Wextra -Wno-unused-parameter \
          -I../protocol -I../include
LDFLAGS = -static -no-pie -Wl,--gc-sections -Wl,--build-id=none

OUT_DIR ?= ../../build/supervisor
export OUT_DIR
TARGET  := $(OUT_DIR)/ishsv

SRCS := ishsv.c

.PHONY: all clean
all: $(TARGET)

$(TARGET): $(SRCS) ../protocol/proto.h
	@case "$$OUT_DIR" in \
		"") \
			echo "refusing to build with empty OUT_DIR" >&2; \
			exit 64 ;; \
		*[!/]* ) \
			;; \
		*) \
			echo "refusing to build with slash-only OUT_DIR: $$OUT_DIR" >&2; \
			exit 64 ;; \
	esac
	@mkdir -p -- "$$OUT_DIR"
	$(CC) $(CFLAGS) $(SRCS) -o "$@" $(LDFLAGS)
	@echo "Built $@:"
	@file "$@" 2>/dev/null || true
	@ls -lh "$@"

clean:
	@case "$$OUT_DIR" in \
		"") \
			echo "refusing to clean empty OUT_DIR" >&2; \
			exit 64 ;; \
		*[!/]* ) \
			;; \
		*) \
			echo "refusing to clean slash-only OUT_DIR: $$OUT_DIR" >&2; \
			exit 64 ;; \
	esac
	rm -f -- "$$OUT_DIR/ishsv"
	@rmdir -- "$$OUT_DIR" 2>/dev/null || true
