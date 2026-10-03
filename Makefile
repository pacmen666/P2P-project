# CS374 第1周 Makefile
# 王：房间服务（tracker）、中继（relay）、公共模块、测试客户端

CC      = gcc
CFLAGS  = -Wall -g -Iinclude

all: room_server relay_server test_client

room_server: src/tracker/room_server.c src/common/log.c src/common/config.c include/common.h include/log.h
	$(CC) $(CFLAGS) -o room_server $^

relay_server: src/relay/relay_server.c src/common/log.c include/common.h include/log.h
	$(CC) $(CFLAGS) -o relay_server $^

test_client: tests/test_client.c src/common/log.c include/common.h include/log.h
	$(CC) $(CFLAGS) -o test_client $^

clean:
	rm -f room_server relay_server test_client