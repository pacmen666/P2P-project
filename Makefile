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
	rm -f room_server relay_server test_client

# 重新编译
re: clean all

.PHONY: all clean re