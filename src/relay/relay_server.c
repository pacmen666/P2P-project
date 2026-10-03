#include "common.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_RELAY_CLIENTS 16
#define BUF_SIZE          2048

struct relay_bind {
    uint8_t peer_id;
    int     fd;
    int     used;
};

static struct relay_bind g_bind_table[MAX_RELAY_CLIENTS];

/* ---------- 工具函数 ---------- */

static void send_all(int fd, const void *buf, int len)
{
    const char *p = (const char *)buf;
    int sent = 0;
    while (sent < len) {
        int n = send(fd, p + sent, len - sent, 0);
        if (n <= 0) break;
        sent += n;
    }
}

static void send_header_and_body(int fd, uint8_t cmd, uint16_t req_id,
                                 const void *body, uint16_t body_len)
{
    struct v1_header h;
    h.magic    = htons(V1_MAGIC);
    h.ver      = V1_VERSION;
    h.cmd      = cmd;
    h.req_id   = htons(req_id);
    h.body_len = htons(body_len);

    send_all(fd, &h, sizeof(h));
    if (body_len > 0 && body) {
        send_all(fd, body, body_len);
    }
}

static void send_error(int fd, uint16_t req_id, uint16_t err)
{
    uint16_t body = htons(err);
    send_header_and_body(fd, CMD_ERROR, req_id, &body, sizeof(body));
}

/* ---------- 处理 RELAY_BIND ---------- */

static void handle_relay_bind(int fd, uint16_t req_id,
                              const uint8_t *body, uint16_t body_len)
{
    if (body_len < 3) {
        log_error("RELAY_BIND 正文太短");
        send_error(fd, req_id, ERR_BODY_LEN);
        return;
    }

    uint8_t  peer_id = body[0];
    uint16_t room_id = (body[1] << 8) | body[2];

    log_info("收到 RELAY_BIND：peer_id=%u, room_id=%u", peer_id, room_id);

    if (peer_id < 1 || peer_id > 4) {
        log_error("RELAY_BIND 身份不符：peer_id=%u", peer_id);
        send_error(fd, req_id, ERR_RELAY_ID);
        close(fd);
        return;
    }

    for (int i = 0; i < MAX_RELAY_CLIENTS; i++) {
        if (g_bind_table[i].used && g_bind_table[i].peer_id == peer_id) {
            log_info("peer_id=%u 重新绑定，关闭旧连接 fd=%d",
                     peer_id, g_bind_table[i].fd);
            close(g_bind_table[i].fd);
            g_bind_table[i].used = 0;
        }
    }

    for (int i = 0; i < MAX_RELAY_CLIENTS; i++) {
        if (!g_bind_table[i].used) {
            g_bind_table[i].used    = 1;
            g_bind_table[i].peer_id = peer_id;
            g_bind_table[i].fd      = fd;
            log_info("绑定成功：peer_id=%u -> fd=%d", peer_id, fd);
            return;
        }
    }

    log_error("绑定表已满，无法记录 peer_id=%u", peer_id);
    send_error(fd, req_id, ERR_RELAY_ID);
    close(fd);
}

/* ---------- 处理一条完整报文 ---------- */

static void handle_packet(int fd, struct v1_header *h, const uint8_t *body)
{
    uint16_t req_id = ntohs(h->req_id);

    switch (h->cmd) {
        case CMD_RELAY_BIND:
            handle_relay_bind(fd, req_id, body, ntohs(h->body_len));
            break;
        case CMD_RELAY_WRAP:
            log_info("收到 RELAY_WRAP（第 1 周不转发）");
            break;
        default:
            log_error("中继收到未知指令 0x%02X", h->cmd);
            send_error(fd, req_id, ERR_UNKNOWN_CMD);
            break;
    }
}

/* ---------- main ---------- */

int main(void)
{
    log_init("logs/relay.log");
    log_info("中继服务启动，TCP 端口 %d", PORT_RELAY_TCP);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT_RELAY_TCP);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(listen_fd, 10) < 0) {
        perror("listen"); return 1;
    }
    log_info("中继 TCP 监听 0.0.0.0:%d", PORT_RELAY_TCP);

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        int maxfd = listen_fd;

        for (int i = 0; i < MAX_RELAY_CLIENTS; i++) {
            if (g_bind_table[i].used) {
                FD_SET(g_bind_table[i].fd, &rfds);
                if (g_bind_table[i].fd > maxfd) maxfd = g_bind_table[i].fd;
            }
        }

        int n = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (n < 0) { perror("select"); break; }

        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
            if (cfd >= 0) {
                log_info("新中继连接：%s:%d",
                         inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));
            }
        }

        for (int i = 0; i < MAX_RELAY_CLIENTS; i++) {
            if (!g_bind_table[i].used) continue;
            if (!FD_ISSET(g_bind_table[i].fd, &rfds)) continue;

            uint8_t buf[BUF_SIZE];
            int r = recv(g_bind_table[i].fd, buf, sizeof(buf), 0);
            if (r <= 0) {
                log_info("中继连接断开：fd=%d", g_bind_table[i].fd);
                close(g_bind_table[i].fd);
                g_bind_table[i].used = 0;
                continue;
            }
            if (r < 8) continue;

            struct v1_header *h = (struct v1_header *)buf;
            if (ntohs(h->magic) != V1_MAGIC || h->ver != V1_VERSION) {
                log_error("magic/ver 错误，关闭 fd=%d", g_bind_table[i].fd);
                close(g_bind_table[i].fd);
                g_bind_table[i].used = 0;
                continue;
            }
            handle_packet(g_bind_table[i].fd, h, buf + 8);
        }
    }

    log_close();
    return 0;
}