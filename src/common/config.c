#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <errno.h>

#include "log.h"


int load_config(const char *path, struct config *cfg)
{
    FILE *fp = fopen(path, "r");
    if (!fp) {
        log_error("无法打开配置文件 %s", path);
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        char *p = strchr(line, '=');
        if (!p) continue;
        *p = '\0';
        char *key = line;
        char *val = p + 1;

        /* 去掉首尾空格 */
        while (*key == ' ' || *key == '\t') key++;
        while (*val == ' ' || *val == '\t') val++;
        char *end = val + strlen(val) - 1;
        while (end > val && (*end == '\n' || *end == '\r' || *end == ' ')) *end-- = '\0';

        if (strcmp(key, "tracker_host") == 0 || strcmp(key, "relay_host") == 0) {
            strncpy(cfg->host, val, sizeof(cfg->host) - 1);
        } else if (strcmp(key, "tracker_port") == 0 || strcmp(key, "relay_port") == 0) {
            strncpy(cfg->port, val, sizeof(cfg->port) - 1);
        }
    }
    fclose(fp);
    return 0;
}

int resolve_host(const char *host, const char *port, struct addrinfo **res)
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    /* 正常解析 */
    int r = getaddrinfo(host, port, &hints, res);
    if (r != 0) {
        log_error("getaddrinfo(%s, %s) 失败：%s", host, port, gai_strerror(r));
        return -1;
    }
    log_info("正常解析 %s:%s 成功", host, port);

    /* 实验 5：AI_NUMERICHOST 再解析一次，应失败 */
    struct addrinfo hints2 = hints;
    hints2.ai_flags = AI_NUMERICHOST;
    struct addrinfo *res2 = NULL;
    int r2 = getaddrinfo(host, port, &hints2, &res2);
    if (r2 != 0) {
        log_info("AI_NUMERICHOST 解析 %s 失败（预期）：%s", host, gai_strerror(r2));
    } else {
        log_error("AI_NUMERICHOST 解析 %s 竟然成功，与课件不符", host);
        freeaddrinfo(res2);
    }

    return 0;
}

void print_peer(int sockfd)
{
    struct sockaddr_in peer;
    socklen_t len = sizeof(peer);
    if (getpeername(sockfd, (struct sockaddr *)&peer, &len) < 0) {
        log_error("getpeername 失败：%s", strerror(errno));
        return;
    }

    char host[256], serv[32];
    if (getnameinfo((struct sockaddr *)&peer, len,
                    host, sizeof(host), serv, sizeof(serv), 0) != 0) {
        log_error("getnameinfo 失败");
        return;
    }
    log_info("对端地址：%s:%s", host, serv);
}

/* ---------- 实验 4：套接字选项记录 ---------- */

void print_sock_options(int fd)
{
    int type = 0;
    socklen_t len = sizeof(type);
    if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0) {
        log_info("SO_TYPE = %d (%s)", type,
                 type == SOCK_STREAM ? "TCP" : (type == SOCK_DGRAM ? "UDP" : "其他"));
    } else {
        log_error("getsockopt SO_TYPE 失败：%s", strerror(errno));
    }

    int rcvbuf = 0;
    len = sizeof(rcvbuf);
    if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &len) == 0) {
        log_info("SO_RCVBUF = %d 字节", rcvbuf);
    } else {
        log_error("getsockopt SO_RCVBUF 失败：%s", strerror(errno));
    }

    int sndbuf = 0;
    len = sizeof(sndbuf);
    if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, &len) == 0) {
        log_info("SO_SNDBUF = %d 字节", sndbuf);
    } else {
        log_error("getsockopt SO_SNDBUF 失败：%s", strerror(errno));
    }
}

/* ---------- 实验 4：SO_REUSEADDR 对比 ---------- */

void test_reuseaddr(const char *host, const char *port, int enable)
{
    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    if (getaddrinfo(host, port, &hints, &res) != 0) {
        log_error("test_reuseaddr: getaddrinfo 失败");
        return;
    }

    /* 第一次 bind */
    int fd1 = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd1 < 0) { log_error("socket1 失败"); freeaddrinfo(res); return; }

    if (enable) {
        int opt = 1;
        setsockopt(fd1, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    if (bind(fd1, res->ai_addr, res->ai_addrlen) < 0) {
        log_error("第一次 bind 失败：%s", strerror(errno));
        close(fd1);
        freeaddrinfo(res);
        return;
    }
    log_info("第一次 bind 成功（SO_REUSEADDR=%d）", enable);

    /* 第二次 bind，不关 fd1 */
    int fd2 = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd2 < 0) { log_error("socket2 失败"); close(fd1); freeaddrinfo(res); return; }

    if (enable) {
        int opt = 1;
        setsockopt(fd2, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    }

    if (bind(fd2, res->ai_addr, res->ai_addrlen) < 0) {
        log_info("第二次 bind 失败（预期，SO_REUSEADDR=%d）：%s",
                 enable, strerror(errno));
    } else {
        log_info("第二次 bind 成功（SO_REUSEADDR=%d）", enable);
    }

    close(fd1);
    close(fd2);
    freeaddrinfo(res);
}