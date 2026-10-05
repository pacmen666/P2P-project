# Invoke from repository root: make -f tests/week1.mk test
# Build products stay outside tracked source directories.
CC ?= cc
OUT ?= build/week1
CPPFLAGS += -Iinclude -Isrc/common
CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic -Werror
COMMON = src/common/fa_stream.c
.PHONY: all test clean
all: $(OUT)/week1_peer.out $(OUT)/test_week1_stream.out
$(OUT):
	mkdir -p $(OUT)
$(OUT)/week1_peer.out: src/transfer/week1_peer.c $(COMMON) src/common/fa_stream.h include/common.h | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON) src/transfer/week1_peer.c $(LDFLAGS) -pthread -lcrypto -o $@
$(OUT)/test_week1_stream.out: tests/test_week1_stream.c $(COMMON) src/common/fa_stream.h include/common.h | $(OUT)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON) tests/test_week1_stream.c $(LDFLAGS) -o $@
test: all
	$(OUT)/test_week1_stream.out
	WEEK1_EXE=$(abspath $(OUT)/week1_peer.out) python3 tests/test_week1_integration.py
clean:
	rm -f $(OUT)/week1_peer.out $(OUT)/test_week1_stream.out
