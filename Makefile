# Fragment Archive — 第1周构建
# Linux / POSIX 实验机。需要 pthread。

CC      = gcc
CFLAGS  = -Wall -g -Iinclude
LDFLAGS = -pthread

COMMON_SRC = src/common/log.c src/common/fa_stream.c

.PHONY: all clean dirs

all: dirs room_server relay_server test_client client

dirs:
	mkdir -p logs

room_server: src/tracker/room_server.c src/common/log.c include/common.h include/log.h
	$(CC) $(CFLAGS) -o room_server src/tracker/room_server.c src/common/log.c

relay_server: src/relay/relay_server.c src/common/log.c include/common.h include/log.h
	$(CC) $(CFLAGS) -o relay_server src/relay/relay_server.c src/common/log.c

test_client: tests/test_client.c src/common/log.c include/common.h include/log.h
	$(CC) $(CFLAGS) -o test_client tests/test_client.c src/common/log.c

# 刘：终端案情板 + 进房 + 探测/通告
client: src/ui/client_main.c src/ui/ui_state.c src/ui/ui_term.c \
	src/punch/udp_sock.c $(COMMON_SRC) \
	include/common.h include/log.h include/ui_state.h include/punch.h include/fa_stream.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o client \
		src/ui/client_main.c src/ui/ui_state.c src/ui/ui_term.c \
		src/punch/udp_sock.c $(COMMON_SRC)

clean:
	rm -f room_server relay_server test_client client
