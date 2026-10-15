#include "common.h"
#include "log.h"
#include "config.h"

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

#define MAX_CLIENTS 16
#define BUF_SIZE    4096

/* 一个已加入的客户端 */
struct client {
    int      fd;
    int      used;
    uint8_t  peer_id;
    char     nick[17];
    uint32_t priv_ip;
    uint16_t tcp_port;
    uint16_t udp_port;
    uint32_t refl_ip;
    uint16_t refl_port;
    time_t   last_heartbeat;

    uint8_t  rxbuf[BUF_SIZE];
    int      rxlen;
};

/* 一个还未发 JOIN_REQ 的临时连接 */
struct temp_conn {
    int      fd;
    int      used;
    uint8_t  rxbuf[BUF_SIZE];
    int      rxlen;
};

static struct client    g_clients[MAX_CLIENTS];
static struct temp_conn g_temps[MAX_CLIENTS];

static uint8_t g_next_peer_id = 2;

static uint8_t g_manifest[4096];
static int     g_manifest_len = 0;

static uint16_t g_room_id = 1;

static int g_udp_fd = -1;

/* 第 3 周：实验 2 开关 */
static int g_iterative_mode = 0;

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

static void temp_remove(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_temps[i].used && g_temps[i].fd == fd) {
            g_temps[i].used = 0;
            g_temps[i].rxlen = 0;
        }
    }
}

static void client_remove(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].fd == fd) {
            g_clients[i].used = 0;
            g_clients[i].rxlen = 0;
        }
    }
}

static int write_peer_entry(uint8_t *out, const struct client *p)
{
    int off = 0;
    out[off++] = p->peer_id;
    memcpy(out + off, &p->priv_ip, 4); off += 4;
    out[off++] = (p->tcp_port >> 8) & 0xFF;
    out[off++] = p->tcp_port & 0xFF;
    out[off++] = (p->udp_port >> 8) & 0xFF;
    out[off++] = p->udp_port & 0xFF;
    memcpy(out + off, &p->refl_ip, 4); off += 4;
    out[off++] = (p->refl_port >> 8) & 0xFF;
    out[off++] = p->refl_port & 0xFF;
    uint8_t nl = strlen(p->nick);
    out[off++] = nl;
    memcpy(out + off, p->nick, nl); off += nl;
    return off;
}

static void push_peer_list(void)
{
    uint8_t body[2048];
    int off = 0;

    int n_off = off++;
    uint8_t peer_n = 0;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].used) continue;
        off += write_peer_entry(body + off, &g_clients[i]);
        peer_n++;
    }
    body[n_off] = peer_n;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].used) continue;
        send_header_and_body(g_clients[i].fd, CMD_PEER_LIST, 0, body, off);
    }

    log_info("推送 PEER_LIST，共 %u 人", peer_n);
}

static void dump_peer_list_to_file(const char *path)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].used) continue;
        fprintf(fp, "%u\n", g_clients[i].peer_id);
    }
    fclose(fp);
}

/* ---------- 处理 JOIN_REQ ---------- */

static void handle_join_req(int fd, uint16_t req_id,
                            const uint8_t *body, uint16_t body_len)
{
    if (body_len < 12) {
        send_error(fd, req_id, ERR_BODY_LEN);
        return;
    }

    uint16_t room_id  = (body[0] << 8) | body[1];
    uint8_t  role     = body[2];
    uint32_t priv_ip  = 0;
    memcpy(&priv_ip, body + 3, 4);
    uint16_t tcp_port = (body[7] << 8) | body[8];
    uint16_t udp_port = (body[9] << 8) | body[10];
    uint8_t  nick_len = body[11];

    if (nick_len > 16 || body_len < 12 + nick_len) {
        send_error(fd, req_id, ERR_BODY_LEN);
        return;
    }

    char nick[17];
    memset(nick, 0, sizeof(nick));
    memcpy(nick, body + 12, nick_len);

    log_info("解析 JOIN_REQ：role=%u, nick_len=%u, nick=%s",
             role, nick_len, nick);

    if (room_id == 0) {
        room_id = g_room_id;
    }

    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        send_error(fd, req_id, ERR_ROOM_FULL);
        return;
    }

    uint8_t peer_id;
    if (role == 1) {
        peer_id = 1;
    } else {
        if (g_next_peer_id > 4) {
            send_error(fd, req_id, ERR_ROOM_FULL);
            return;
        }
        peer_id = g_next_peer_id++;
    }

    struct client *c = &g_clients[slot];
    c->fd             = fd;
    c->used           = 1;
    c->peer_id        = peer_id;
    c->priv_ip        = priv_ip;
    c->tcp_port       = tcp_port;
    c->udp_port       = udp_port;
    c->refl_ip        = 0;
    c->refl_port      = 0;
    c->last_heartbeat = time(NULL);
    c->rxlen          = 0;
    memset(c->nick, 0, sizeof(c->nick));
    memcpy(c->nick, nick, nick_len);

    temp_remove(fd);

    log_info("收到 JOIN_REQ：peer_id=%u, nick=%s", peer_id, c->nick);

    uint8_t out[1024];
    int off = 0;

    out[off++] = (room_id >> 8) & 0xFF;
    out[off++] = room_id & 0xFF;
    out[off++] = peer_id;

    int peer_n_off = off++;
    uint8_t peer_n = 0;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].used) continue;
        off += write_peer_entry(out + off, &g_clients[i]);
        peer_n++;
    }
    out[peer_n_off] = peer_n;

    send_header_and_body(fd, CMD_JOIN_OK, req_id, out, off);

    if (g_manifest_len > 0) {
        send_header_and_body(fd, CMD_MANIFEST, 0, g_manifest, g_manifest_len);
    }

    push_peer_list();
    dump_peer_list_to_file("/tmp/room_peers.txt");
}

static void handle_heartbeat(int fd, uint16_t req_id)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].fd == fd) {
            g_clients[i].last_heartbeat = time(NULL);
            log_info("收到 HEARTBEAT：peer_id=%u", g_clients[i].peer_id);
            break;
        }
    }
    send_header_and_body(fd, CMD_HEARTBEAT_ACK, req_id, NULL, 0);
}

static void handle_probe(int fd, uint16_t req_id,
                         const uint8_t *body, uint16_t body_len,
                         struct sockaddr_in *src)
{
    (void)fd;
    if (body_len < 3) return;

    uint8_t peer_id = body[2];
    uint32_t ip     = src->sin_addr.s_addr;
    uint16_t port   = ntohs(src->sin_port);

    log_info("收到 PROBE：peer_id=%u, 反射地址=%s:%u",
             peer_id, inet_ntoa(src->sin_addr), port);

    int updated = 0;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].peer_id == peer_id) {
            if (g_clients[i].refl_ip != ip || g_clients[i].refl_port != port) {
                updated = 1;
            }
            g_clients[i].refl_ip   = ip;
            g_clients[i].refl_port = port;
            break;
        }
    }

    uint8_t ack[6];
    memcpy(ack, &ip, 4);
    ack[4] = (port >> 8) & 0xFF;
    ack[5] = port & 0xFF;

    struct v1_header h;
    h.magic    = htons(V1_MAGIC);
    h.ver      = V1_VERSION;
    h.cmd      = CMD_PROBE_ACK;
    h.req_id   = htons(req_id);
    h.body_len = htons(6);

    uint8_t packet[8 + 6];
    memcpy(packet, &h, 8);
    memcpy(packet + 8, ack, 6);

    if (g_udp_fd >= 0) {
        sendto(g_udp_fd, packet, sizeof(packet), 0,
               (struct sockaddr *)src, sizeof(*src));
    }

    if (updated) {
        push_peer_list();
    }
}

static void handle_addr_query(int fd, uint16_t req_id,
                              const uint8_t *body, uint16_t body_len)
{
    if (body_len < 1) {
        send_error(fd, req_id, ERR_BODY_LEN);
        return;
    }

    uint8_t target = body[0];
    log_info("收到 ADDR_QUERY：目标 peer_id=%u", target);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].peer_id == target) {
            uint8_t out[64];
            int off = write_peer_entry(out, &g_clients[i]);
            send_header_and_body(fd, CMD_ADDR_REPLY, req_id, out, off);
            return;
        }
    }

    send_error(fd, req_id, ERR_NO_PIECE);
}

static void handle_manifest(int fd, uint16_t req_id,
                            const uint8_t *body, uint16_t body_len)
{
    if (body_len > sizeof(g_manifest)) {
        send_error(fd, req_id, ERR_BODY_LEN);
        return;
    }
    memcpy(g_manifest, body, body_len);
    g_manifest_len = body_len;
    log_info("收到 MANIFEST，已缓存 %d 字节", body_len);
}

static void handle_manifest_req(int fd, uint16_t req_id)
{
    if (g_manifest_len == 0) {
        send_error(fd, req_id, ERR_NO_MANIFEST);
        return;
    }
    send_header_and_body(fd, CMD_MANIFEST, req_id, g_manifest, g_manifest_len);
}

static void handle_packet(int fd, struct v1_header *h,
                          const uint8_t *body,
                          struct sockaddr_in *src)
{
    uint16_t req_id = ntohs(h->req_id);

    switch (h->cmd) {
        case CMD_JOIN_REQ:
            handle_join_req(fd, req_id, body, ntohs(h->body_len));
            break;
        case CMD_HEARTBEAT:
            handle_heartbeat(fd, req_id);
            break;
        case CMD_PROBE:
            handle_probe(fd, req_id, body, ntohs(h->body_len), src);
            break;
        case CMD_ADDR_QUERY:
            handle_addr_query(fd, req_id, body, ntohs(h->body_len));
            break;
        case CMD_MANIFEST:
            handle_manifest(fd, req_id, body, ntohs(h->body_len));
            break;
        case CMD_MANIFEST_REQ:
            handle_manifest_req(fd, req_id);
            break;
        default:
            log_error("未知指令 0x%02X", h->cmd);
            send_error(fd, req_id, ERR_UNKNOWN_CMD);
            break;
    }
}

static void handle_temp_data(int fd)
{
    struct temp_conn *t = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_temps[i].used && g_temps[i].fd == fd) {
            t = &g_temps[i];
            break;
        }
    }
    if (!t) return;

    int r = recv(fd, t->rxbuf + t->rxlen, sizeof(t->rxbuf) - t->rxlen, 0);
    if (r <= 0) {
        log_info("临时连接断开：fd=%d", fd);
        close(fd);
        temp_remove(fd);
        return;
    }
    t->rxlen += r;

    while (t->rxlen >= 8) {
        struct v1_header *h = (struct v1_header *)t->rxbuf;

        if (ntohs(h->magic) != V1_MAGIC || h->ver != V1_VERSION) {
            log_error("临时连接 magic/ver 错误，关闭 fd=%d", fd);
            close(fd);
            temp_remove(fd);
            return;
        }

        uint16_t blen = ntohs(h->body_len);
        if (t->rxlen < 8 + blen) break;

        if (h->cmd == CMD_JOIN_REQ) {
            handle_join_req(fd, ntohs(h->req_id), t->rxbuf + 8, blen);
        } else {
            log_error("临时连接收到非 JOIN_REQ 指令 0x%02X，关闭 fd=%d",
                      h->cmd, fd);
            close(fd);
            temp_remove(fd);
            return;
        }

        int consumed = 8 + blen;
        memmove(t->rxbuf, t->rxbuf + consumed, t->rxlen - consumed);
        t->rxlen -= consumed;

        if (!t->used) return;
    }
}

static void handle_client_data(int fd)
{
    struct client *c = NULL;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].fd == fd) {
            c = &g_clients[i];
            break;
        }
    }
    if (!c) return;

    int r = recv(fd, c->rxbuf + c->rxlen, sizeof(c->rxbuf) - c->rxlen, 0);
    if (r <= 0) {
        log_info("客户端断开：fd=%d", fd);
        close(fd);
        client_remove(fd);
        push_peer_list();
        dump_peer_list_to_file("/tmp/room_peers.txt");
        return;
    }
    c->rxlen += r;

    while (c->rxlen >= 8) {
        struct v1_header *h = (struct v1_header *)c->rxbuf;

        if (ntohs(h->magic) != V1_MAGIC || h->ver != V1_VERSION) {
            log_error("magic/ver 错误，关闭 fd=%d", fd);
            close(fd);
            client_remove(fd);
            push_peer_list();
            dump_peer_list_to_file("/tmp/room_peers.txt");
            return;
        }

        uint16_t blen = ntohs(h->body_len);
        if (c->rxlen < 8 + blen) break;

        handle_packet(fd, h, c->rxbuf + 8, NULL);

        int consumed = 8 + blen;
        memmove(c->rxbuf, c->rxbuf + consumed, c->rxlen - consumed);
        c->rxlen -= consumed;

        if (!c->used) return;
    }
}

/* ---------- 第 3 周：迭代模式 ---------- */

static void run_iterative(int listen_fd)
{
    log_info("=== 实验 2：进入迭代模式 ===");
    while (1) {
        struct sockaddr_in caddr;
        socklen_t clen = sizeof(caddr);
        int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
        if (cfd < 0) continue;

        log_info("迭代模式：开始处理 fd=%d（sleep 3 秒）", cfd);
        sleep(3);
        log_info("迭代模式：处理完 fd=%d，关闭", cfd);
        close(cfd);
    }
}

/* ---------- main ---------- */

int main(int argc, char *argv[])
{
    /* 第 3 周：解析 --iterative */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--iterative") == 0) {
            g_iterative_mode = 1;
        }
    }

    log_init("logs/room.log");
    log_info("房间服务启动，TCP 端口 %d，UDP 端口 %d，模式=%s",
             PORT_ROOM_TCP, PORT_ROOM_UDP,
             g_iterative_mode ? "迭代" : "并发");

    log_info("=== 实验 4：SO_REUSEADDR 对比 ===");
    test_reuseaddr("0.0.0.0", "37401", 0);
    test_reuseaddr("0.0.0.0", "37401", 1);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    print_sock_options(listen_fd);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PORT_ROOM_TCP);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(listen_fd, 10) < 0) {
        perror("listen"); return 1;
    }
    log_info("TCP 监听 0.0.0.0:%d", PORT_ROOM_TCP);

    /* 第 3 周：迭代模式直接走 run_iterative */
    if (g_iterative_mode) {
        run_iterative(listen_fd);
        return 0;
    }

    g_udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_fd < 0) { perror("udp socket"); return 1; }
    setsockopt(g_udp_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in uaddr;
    memset(&uaddr, 0, sizeof(uaddr));
    uaddr.sin_family      = AF_INET;
    uaddr.sin_addr.s_addr = INADDR_ANY;
    uaddr.sin_port        = htons(PORT_ROOM_UDP);

    if (bind(g_udp_fd, (struct sockaddr *)&uaddr, sizeof(uaddr)) < 0) {
        perror("udp bind"); return 1;
    }
    log_info("UDP 监听 0.0.0.0:%d", PORT_ROOM_UDP);

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        FD_SET(g_udp_fd, &rfds);
        int maxfd = (listen_fd > g_udp_fd) ? listen_fd : g_udp_fd;

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].used) {
                FD_SET(g_clients[i].fd, &rfds);
                if (g_clients[i].fd > maxfd) maxfd = g_clients[i].fd;
            }
        }
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_temps[i].used) {
                FD_SET(g_temps[i].fd, &rfds);
                if (g_temps[i].fd > maxfd) maxfd = g_temps[i].fd;
            }
        }

        struct timeval tv;
        tv.tv_sec  = 1;
        tv.tv_usec = 0;

        int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) { perror("select"); break; }

        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
            if (cfd >= 0) {
                log_info("新 TCP 连接：%s:%d",
                         inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));
                int placed = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (!g_temps[i].used) {
                        g_temps[i].used = 1;
                        g_temps[i].fd   = cfd;
                        g_temps[i].rxlen = 0;
                        placed = 1;
                        break;
                    }
                }
                if (!placed) {
                    log_error("临时连接表已满，关闭 fd=%d", cfd);
                    close(cfd);
                }
            }
        }

        if (FD_ISSET(g_udp_fd, &rfds)) {
            uint8_t buf[BUF_SIZE];
            struct sockaddr_in src;
            socklen_t slen = sizeof(src);
            int r = recvfrom(g_udp_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &slen);
            if (r >= 8) {
                struct v1_header *h = (struct v1_header *)buf;
                if (ntohs(h->magic) == V1_MAGIC &&
                    h->ver == V1_VERSION &&
                    h->cmd == CMD_PROBE) {
                    handle_probe(-1, ntohs(h->req_id),
                                 buf + 8, ntohs(h->body_len), &src);
                }
            }
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!g_temps[i].used) continue;
            if (!FD_ISSET(g_temps[i].fd, &rfds)) continue;
            handle_temp_data(g_temps[i].fd);
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i].used) continue;
            if (!FD_ISSET(g_clients[i].fd, &rfds)) continue;
            handle_client_data(g_clients[i].fd);
        }

        time_t now = time(NULL);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].used && g_clients[i].peer_id != 0) {
                if (now - g_clients[i].last_heartbeat > HEARTBEAT_TIMEOUT) {
                    log_info("心跳超时，移除 peer_id=%u", g_clients[i].peer_id);
                    close(g_clients[i].fd);
                    g_clients[i].used = 0;
                    g_clients[i].rxlen = 0;
                    push_peer_list();
                    dump_peer_list_to_file("/tmp/room_peers.txt");
                }
            }
        }
    }

    log_close();
    return 0;
}