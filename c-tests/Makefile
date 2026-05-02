# Quick host-only Makefile for sanity checks (no iSH link, no Meson).
#
# Targets:
#   make proto    — compile + run protocol unit tests
#   make ishembed — compile-test ishembed.c (no link)
#   make ishsv    — compile-test supervisor on host (does NOT run; needs i386-linux-musl-gcc to actually run)
#   make all      — all of the above
#
# This Makefile is for convenience. The canonical build is via Meson
# (../meson.build) or ../scripts/build-ios.sh.

CC      ?= cc
CFLAGS  ?= -Wall -Wextra -Wno-unused-parameter -O2
INCLUDES = -I../include -I../protocol -I../ffi

OUT = ../build/host-checks
$(shell mkdir -p $(OUT))

.PHONY: all proto ishembed ishsv ish_ffi clean
all: proto ishembed ishsv ish_ffi

ish_ffi: $(OUT)/ish_ffi.o
$(OUT)/ish_ffi.o: ../ffi/ish_ffi.c ../ffi/ish_ffi.h
	$(CC) -c $(CFLAGS) $(INCLUDES) -DGUEST_ARM64=1 \
		-I../third_party/ish -I../third_party/ish/vdso/arm64 \
		$< -o $@

proto: $(OUT)/proto_test
	$(OUT)/proto_test

$(OUT)/proto_test: proto_test.c ../protocol/proto.h
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@

ishembed: $(OUT)/ishembed.o
$(OUT)/ishembed.o: ../host/ishembed.c ../include/ishembed.h ../protocol/proto.h ../ffi/ish_ffi.h
	$(CC) -c $(CFLAGS) $(INCLUDES) $< -o $@

ishsv: $(OUT)/ishsv.o
$(OUT)/ishsv.o: ../supervisor/ishsv.c ../protocol/proto.h
	$(CC) -c $(CFLAGS) $(INCLUDES) $< -o $@

clean:
	rm -rf $(OUT)
