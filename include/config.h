#ifndef CONFIG_H
#define CONFIG_H

#include <netdb.h>

/* 配置结构体 */
struct config {
    char host[256];   /* 主机名，比如 localhost */
    char port[16];    /* 端口，比如 37401 */
};

/* 读配置文件 */
int load_config(const char *path, struct config *cfg);

/* 主机名解析（实验 5） */
int resolve_host(const char *host, const char *port, struct addrinfo **res);

/* 打印已连接对端地址 */
void print_peer(int sockfd);

/* 实验 4：打印套接字选项 */
void print_sock_options(int fd);

/* 实验 4：SO_REUSEADDR 对比 */
void test_reuseaddr(const char *host, const char *port, int enable);

#endif /* CONFIG_H */