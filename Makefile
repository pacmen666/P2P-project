# CS374 第2周 Makefile
# 王：房间服务（tracker）、中继（relay）、公共模块、测试客户端

CC      = gcc
CFLAGS  = -Wall -g -Iinclude
LDFLAGS ?=
# Homebrew OpenSSL is keg-only; Linux uses the system headers/library.
ifeq ($(shell uname -s),Darwin)
OPENSSL_PREFIX ?= $(shell brew --prefix openssl@3 2>/dev/null)
CPPFLAGS += -I$(OPENSSL_PREFIX)/include
LDFLAGS += -L$(OPENSSL_PREFIX)/lib
FAULT_FLAGS = -dynamiclib
else
FAULT_FLAGS = -shared -fPIC -ldl
endif

# 默认目标：编译所有
all: room_server relay_server test_client client

CLIENT_OBJS = src/ui/client_main.c src/punch/udp_sock.c src/ui/ui_state.c src/ui/ui_term.c \
              src/common/fa_stream.c src/common/log.c
CLIENT_HDRS = include/common.h include/fa_stream.h include/punch.h include/log.h include/ui_state.h

client: $(CLIENT_OBJS) $(CLIENT_HDRS)
	$(CC) $(CPPFLAGS) $(CFLAGS) -pthread $(CLIENT_OBJS) -o $@

# 房间服务
room_server: src/tracker/room_server.c src/common/log.c src/common/config.c include/common.h include/log.h include/config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o room_server $(filter %.c,$^)

# 中继服务
relay_server: src/relay/relay_server.c src/common/fa_stream.c src/common/log.c src/common/config.c include/common.h include/fa_stream.h include/log.h include/config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o relay_server $(filter %.c,$^)

# 测试客户端
test_client: tests/test_client.c src/common/log.c src/common/config.c include/common.h include/log.h include/config.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o test_client $(filter %.c,$^)

# 清理
clean:
	rm -f room_server relay_server test_client client client_transfer relay_transfer tests/udp_fault.so tests/test_transfer_stream

# 重新编译
re: clean all

# ---- 分块传输（彭）+ 正式中继 relay_server（王）；见 docs/transfer_closeout.md。
# client_transfer 是陈的实验 8/9 统计口径采集入口（TRANSFER_STATS 行含 total_up/elapsed/hash_reject）。 ----
TRANSFER_OBJS = src/transfer/client_main.c src/transfer/transfer.c src/generate/generate.c \
                src/punch/udp_sock.c src/ui/ui_state.c src/ui/ui_term.c \
                src/common/fa_stream.c src/common/log.c
TRANSFER_HDRS = include/common.h include/fa_stream.h include/punch.h include/log.h include/ui_state.h \
                src/transfer/transfer.h src/generate/generate.h

client_transfer: $(TRANSFER_OBJS) $(TRANSFER_HDRS) Makefile
	$(CC) $(CPPFLAGS) $(CFLAGS) -DFA_TRANSFER_CLIENT -Isrc/generate -Isrc/transfer -pthread $(TRANSFER_OBJS) $(LDFLAGS) -lcrypto -o $@

# relay_forward.c is retained as historical source; the only active relay is relay_server.
transfer: room_server client_transfer relay_server

tests/udp_fault.so: tests/udp_fault.c
	$(CC) -Wall -Wextra $(FAULT_FLAGS) -o $@ $<

tests/test_transfer_stream: tests/test_transfer_stream.c src/common/fa_stream.c include/fa_stream.h
	$(CC) $(CPPFLAGS) $(CFLAGS) tests/test_transfer_stream.c src/common/fa_stream.c -o $@

test-transfer: transfer tests/udp_fault.so tests/test_transfer_stream
	./tests/test_transfer_stream
	python3 -u tests/test_transfer.py

.PHONY: all clean re transfer test-transfer