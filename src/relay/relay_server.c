#include "config.h"
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
#define BUF_SIZE          32768 
#define ROOM_PEERS_FILE   "/tmp/room_peers.txt"

struct relay_bind {
    uint8_t peer_id;
    int     fd;
    int     used;
    uint8_t rxbuf[BUF_SIZE];
    int     rxlen;
};

struct temp_relay {
    int     fd;
    int     used;
    uint8_t rxbuf[BUF_SIZE];
    int     rxlen;
};

static struct relay_bind  g_bind_table[MAX_RELAY_CLIENTS];
static struct temp_relay  g_relay_temps[MAX_RELAY_CLIENTS];

/* ---------- 函数前置声明（防止 C 语言编译顺序报错） ---------- */
static void handle_relay_bound_data(int fd);

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

static int is_peer_in_room(uint8_t peer_id)
{
    FILE *fp = fopen(ROOM_PEERS_FILE, "r");
    if (!fp) {
        log_error("无法打开房间名单文件 %s", ROOM_PEERS_FILE);
        return 0;
    }
    char line[32];
    int found = 0;
    while (fgets(line, sizeof(line), fp)) {
        int id = atoi(line);
        if (id == peer_id) { found = 1; break; }
    }
    fclose(fp);
    return found;
}

static void relay_temp_remove(int fd)
{
    for (int i = 0; i < MAX_RELAY_CLIENTS; i++) {
        if (g_relay_temps[i].used && g_relay_temps[i].fd == fd) {
            g_relay_temps[i].used = 0;
            g_relay_temps[i].rxlen = 0;
        }
    }
}

static void relay_bind_remove(int fd)
{
    for (int i = 0; i < MAX_RELAY_CLIENTS; i++) {
        if (g_bind_table[i].used && g_bind_table[i].fd == fd) {
            g_bind_table[i].used = 0;
            g_bind_table[i].rxlen = 0;
        }
    }
}

static struct relay_bind *find_bind_by_peer(uint8_t peer_id)
{
    for (int i = 0; i < MAX_RELAY_CLIENTS; i++) {
        if (g_bind_table[i].used && g_bind_table[i].peer_id == peer_id) {
            return &g_bind_table[i];
        }
    }
    return NULL;
}

static struct relay_bind *find_bind_by_fd(int fd)
{
    for (int i = 0; i < MAX_RELAY_CLIENTS; i++) {
        if (g_bind_table[i].used && g_bind_table[i].fd == fd) {
            return &g_bind_table[i];
        }
    }
    return NULL;
}

/* ---------- 处理 RELAY_WRAP ---------- */

static void handle_relay_wrap(int fd, uint16_t req_id,
                              const uint8_t *body, uint16_t body_len)
{
    struct relay_bind *src = find_bind_by_fd(fd);
    if (!src) {
        log_error("RELAY_WRAP 来自未绑定连接 fd=%d", fd);
        send_error(fd, req_id, ERR_RELAY_ID);
        close(fd);
        relay_bind_remove(fd);
        return;
    }

    if (body_len < 4) {
        log_error("RELAY_WRAP 正文太短");
        send_error(fd, req_id, ERR_WRAP_LEN);
        close(fd);
        relay_bind_remove(fd);
        return;
    }

    uint8_t  dst_id    = body[0];
    uint8_t  src_id    = body[1];
    uint16_t inner_len = (body[2] << 8) | body[3];

    log_info("收到 RELAY_WRAP：src=%u, dst=%u, inner_len=%u",
             src_id, dst_id, inner_len);

    if (body_len != 4 + inner_len || inner_len < 8) {
        log_error("RELAY_WRAP 长度不一致：body_len=%u, inner_len=%u",
                  body_len, inner_len);
        send_error(fd, req_id, ERR_WRAP_LEN);
        close(fd);
        relay_bind_remove(fd);
        return;
    }

    if (src_id != src->peer_id) {
        log_error("RELAY_WRAP 身份不符：src_id=%u, 连接登记=%u",
                  src_id, src->peer_id);
        send_error(fd, req_id, ERR_RELAY_ID);
        close(fd);
        relay_bind_remove(fd);
        return;
    }

    struct relay_bind *dst = find_bind_by_peer(dst_id);
    if (!dst) {
        log_error("RELAY_WRAP 目标未绑定：dst_id=%u", dst_id);
        send_error(fd, req_id, ERR_RELAY_UNAVAIL);
        return;
    }

    uint8_t out_body[4 + 32768];
    out_body[0] = dst_id;
    out_body[1] = src_id;
    out_body[2] = (inner_len >> 8) & 0xFF;
    out_body[3] = inner_len & 0xFF;
    memcpy(out_body + 4, body + 4, inner_len);

    send_header_and_body(dst->fd, CMD_RELAY_WRAP, 0, out_body, 4 + inner_len);

    log_info("转发 RELAY_WRAP：src=%u -> dst=%u, inner_len=%u",
             src_id, dst_id, inner_len);
}

/* ---------- 处理一条完整报文 ---------- */

static void handle_packet(int fd, struct v1_header *h, const uint8_t *body)
{
    uint16_t req_id = ntohs(h->req_id);

    switch (h->cmd) {
        case CMD_RELAY_WRAP:
            handle_relay_wrap(fd, req_id, body, ntohs(h->body_len));
            break;
        default:
            log_error("中继收到未知指令 0x%02X", h->cmd);
            send_error(fd, req_id, ERR_UNKNOWN_CMD);
            break;
    }
}

/* ---------- 处理临时连接数据 ---------- */

static void handle_relay_temp_data(int fd)
{
    struct temp_relay *t = NULL;
    for (int i = 0; i < MAX_RELAY_CLIENTS; i++) {
        if (g_relay_temps[i].used && g_relay_temps[i].fd == fd) {
            t = &g_relay_temps[i];
            break;
        }
    }
    if (!t) return;

    int r = recv(fd, t->rxbuf + t->rxlen, sizeof(t->rxbuf) - t->rxlen, 0);
    if (r <= 0) {
        log_info("临时中继连接断开：fd=%d", fd);
        close(fd);
        relay_temp_remove(fd);
        return;
    }
    t->rxlen += r;

    while (t->rxlen >= 8) {
        struct v1_header *h = (struct v1_header *)t->rxbuf;

        if (ntohs(h->magic) != V1_MAGIC || h->ver != V1_VERSION) {
            log_error("临时中继连接 magic/ver 错误，关闭 fd=%d", fd);
            close(fd);
            relay_temp_remove(fd);
            return;
        }

        uint16_t blen = ntohs(h->body_len);
        if (t->rxlen < 8 + blen) break;

        if (h->cmd == CMD_RELAY_BIND) {
            uint8_t  peer_id = t->rxbuf[8];
            uint16_t room_id = (t->rxbuf[9] << 8) | t->rxbuf[10];

            log_info("收到 RELAY_BIND：peer_id=%u, room_id=%u", peer_id, room_id);

            if (room_id != 1 || !is_peer_in_room(peer_id)) {
                log_error("RELAY_BIND 校验失败：peer_id=%u, room_id=%u", peer_id, room_id);
                send_error(fd, ntohs(h->req_id), ERR_RELAY_ID);
                close(fd);
                relay_temp_remove(fd);
                return;
            }

            for (int i = 0; i < MAX_RELAY_CLIENTS; i++) {
                if (g_bind_table[i].used && g_bind_table[i].peer_id == peer_id) {
                    log_info("peer_id=%u 重新绑定，关闭旧连接 fd=%d", peer_id, g_bind_table[i].fd);
                    close(g_bind_table[i].fd);
                    g_bind_table[i].used = 0;
                }
            }

            int slot = -1;
            for (int i = 0; i < MAX_RELAY_CLIENTS; i++) {
                if (!g_bind_table[i].used) { slot = i; break; }
            }
            if (slot < 0) {
                log_error("绑定表已满，关闭 fd=%d", fd);
                close(fd);
                relay_temp_remove(fd);
                return;
            }

            struct relay_bind *b = &g_bind_table[slot];
            b->used = 1;
            b->peer_id = peer_id;
            b->fd = fd;
            log_info("绑定成功：peer_id=%u -> fd=%d", peer_id, fd);

            b->rxlen = t->rxlen - (8 + blen);
            if (b->rxlen > 0) {
                memcpy(b->rxbuf, t->rxbuf + 8 + blen, b->rxlen);
                log_info("移交剩余数据长度：%d", b->rxlen);
            }

            relay_temp_remove(fd);
            
            if (b->rxlen > 0) {
                handle_relay_bound_data(fd);
            }
            return;

        } else {
            log_error("临时中继连接收到非 RELAY_BIND 指令 0x%02X", h->cmd);
            close(fd);
            relay_temp_remove(fd);
            return;
        }
    }
}

/* ---------- 处理已绑定连接数据 ---------- */

static void handle_relay_bound_data(int fd)
{
    struct relay_bind *b = find_bind_by_fd(fd);
    if (!b) return;

    if (b->rxlen < sizeof(b->rxbuf)) {
        int r = recv(fd, b->rxbuf + b->rxlen, sizeof(b->rxbuf) - b->rxlen, 0);
        if (r <= 0) {
            log_info("中继连接断开：fd=%d", fd);
            close(fd);
            relay_bind_remove(fd);
            return;
        }
        b->rxlen += r;
    }

    while (b->rxlen >= 8) {
        struct v1_header *h = (struct v1_header *)b->rxbuf;

        if (ntohs(h->magic) != V1_MAGIC || h->ver != V1_VERSION) {
            log_error("magic/ver 错误，关闭 fd=%d", fd);
            close(fd);
            relay_bind_remove(fd);
            return;
        }

        uint16_t blen = ntohs(h->body_len);
        if (b->rxlen < 8 + blen) break;

        handle_packet(fd, h, b->rxbuf + 8);

        int consumed = 8 + blen;
        memmove(b->rxbuf, b->rxbuf + consumed, b->rxlen - consumed);
        b->rxlen -= consumed;

        if (!find_bind_by_fd(fd)) return;
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
        for (int i = 0; i < MAX_RELAY_CLIENTS; i++) {
            if (g_relay_temps[i].used) {
                FD_SET(g_relay_temps[i].fd, &rfds);
                if (g_relay_temps[i].fd > maxfd) maxfd = g_relay_temps[i].fd;
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
                int placed = 0;
                for (int i = 0; i < MAX_RELAY_CLIENTS; i++) {
                    if (!g_relay_temps[i].used) {
                        g_relay_temps[i].used  = 1;
                        g_relay_temps[i].fd    = cfd;
                        g_relay_temps[i].rxlen = 0;
                        placed = 1;
                        break;
                    }
                }
                if (!placed) {
                    log_error("临时中继连接表已满，关闭 fd=%d", cfd);
                    close(cfd);
                }
            }
        }

        for (int i = 0; i < MAX_RELAY_CLIENTS; i++) {
            if (!g_relay_temps[i].used) continue;
            if (!FD_ISSET(g_relay_temps[i].fd, &rfds)) continue;
            handle_relay_temp_data(g_relay_temps[i].fd);
        }

        for (int i = 0; i < MAX_RELAY_CLIENTS; i++) {
            if (!g_bind_table[i].used) continue;
            if (!FD_ISSET(g_bind_table[i].fd, &rfds)) continue;
            handle_relay_bound_data(g_bind_table[i].fd);
        }
    }

    log_close();
    return 0;
}