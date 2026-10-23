# CS374 第2周 Makefile
# 王：房间服务（tracker）、中继（relay）、公共模块、测试客户端

CC      = gcc
CFLAGS  = -Wall -g -Iinclude
LDFLAGS =

# 默认目标：编译所有
all: room_server relay_server test_client

# 房间服务
room_server: src/tracker/room_server.c src/common/log.c src/common/config.c include/common.h include/log.h include/config.h
	$(CC) $(CFLAGS) -o room_server $^

# 中继服务
relay_server: src/relay/relay_server.c src/common/log.c src/common/config.c include/common.h include/log.h include/config.h
	$(CC) $(CFLAGS) -o relay_server $^

# 测试客户端
test_client: tests/test_client.c src/common/log.c src/common/config.c include/common.h include/log.h include/config.h
	$(CC) $(CFLAGS) -o test_client $^

# 清理
clean:
	rm -f room_server relay_server test_client client_transfer relay_transfer

# 重新编译
re: clean all

# ---- 第4周：分块传输（彭）+ 中继适配层（王）。
# client_transfer 是陈的实验 8/9 统计口径采集入口（TRANSFER_STATS 行含 total_up/elapsed/hash_reject）。 ----
TRANSFER_OBJS = src/transfer/client_main.c src/transfer/transfer.c src/generate/generate.c \
                src/punch/udp_sock.c src/ui/ui_state.c src/ui/ui_term.c \
                src/common/fa_stream.c src/common/log.c
TRANSFER_HDRS = include/common.h include/fa_stream.h include/punch.h include/log.h include/ui_state.h \
                src/transfer/transfer.h src/generate/generate.h

client_transfer: $(TRANSFER_OBJS) $(TRANSFER_HDRS)
	$(CC) $(CFLAGS) -Isrc/generate -Isrc/transfer -pthread $(TRANSFER_OBJS) -lcrypto -o $@

relay_transfer: src/relay/relay_forward.c src/common/fa_stream.c include/common.h include/fa_stream.h
	$(CC) $(CFLAGS) src/relay/relay_forward.c src/common/fa_stream.c -o $@

transfer: client_transfer relay_transfer

test-transfer: transfer
	python3 tests/test_transfer.py

.PHONY: all clean re transfer test-transfer