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
	rm -f room_server relay_server test_client client tests/probe_client_min

# 重新编译
re: clean all

.PHONY: all clean re probe_client_min