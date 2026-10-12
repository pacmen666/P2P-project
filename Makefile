# CS374 第2周 Makefile
# 王：房间/中继；刘：终端客户端；公共模块与测试

CC      = gcc
CFLAGS  = -Wall -g -Iinclude
LDFLAGS =
PTHREAD = -pthread

# 默认目标：编译所有
all: room_server relay_server test_client client

# 房间服务
room_server: src/tracker/room_server.c src/common/log.c src/common/config.c include/common.h include/log.h include/config.h
	$(CC) $(CFLAGS) -o room_server $^

# 中继服务
relay_server: src/relay/relay_server.c src/common/log.c src/common/config.c include/common.h include/log.h include/config.h
	$(CC) $(CFLAGS) -o relay_server $^

# 测试客户端
test_client: tests/test_client.c src/common/log.c src/common/config.c include/common.h include/log.h include/config.h
	$(CC) $(CFLAGS) -o test_client $^

# 刘：案情板、房间连接、通告与打洞客户端
client: src/ui/client_main.c src/ui/ui_state.c src/ui/ui_term.c \
	src/punch/udp_sock.c src/common/log.c src/common/fa_stream.c \
	include/common.h include/log.h include/ui_state.h include/punch.h include/fa_stream.h
	$(CC) $(CFLAGS) $(PTHREAD) -o client \
		src/ui/client_main.c src/ui/ui_state.c src/ui/ui_term.c \
		src/punch/udp_sock.c src/common/log.c src/common/fa_stream.c

probe_client_min: tests/probe_client_min.c src/punch/udp_sock.c src/common/log.c \
	include/common.h include/punch.h include/log.h
	$(CC) $(CFLAGS) -o tests/probe_client_min \
		tests/probe_client_min.c src/punch/udp_sock.c src/common/log.c

# 清理
clean:
	rm -f room_server relay_server test_client client tests/probe_client_min client_transfer relay_transfer tests/udp_fault.so tests/test_transfer_stream

# 重新编译
re: clean all

.PHONY: all clean re probe_client_min
# 彭的独立联调入口：保留原 client / relay_server 默认行为。
TRANSFER_SRC = src/transfer/client_main.c src/transfer/transfer.c src/generate/generate.c
TRANSFER_SHARED = src/ui/ui_state.c src/ui/ui_term.c src/punch/udp_sock.c src/common/log.c src/common/fa_stream.c
client_transfer: $(TRANSFER_SRC) $(TRANSFER_SHARED) src/transfer/transfer.h include/common.h include/fa_stream.h
	$(CC) $(CFLAGS) -DFA_TRANSFER_CLIENT -Isrc/transfer -Isrc/generate $(PTHREAD) -o $@ $(TRANSFER_SRC) $(TRANSFER_SHARED) $(LDFLAGS) -lcrypto

relay_transfer: src/relay/relay_forward.c src/common/fa_stream.c include/common.h include/fa_stream.h
	$(CC) $(CFLAGS) -o $@ src/relay/relay_forward.c src/common/fa_stream.c $(LDFLAGS)

transfer: room_server client_transfer relay_transfer

tests/udp_fault.so: tests/udp_fault.c
	$(CC) -Wall -Wextra -shared -fPIC -o $@ $< -ldl

tests/test_transfer_stream: tests/test_transfer_stream.c src/common/fa_stream.c include/fa_stream.h
	$(CC) $(CFLAGS) -Wextra -Werror -o $@ tests/test_transfer_stream.c src/common/fa_stream.c

test-transfer: transfer tests/udp_fault.so tests/test_transfer_stream
	./tests/test_transfer_stream
	python3 -u tests/test_transfer.py
.PHONY: transfer test-transfer
