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

#define MAX_CLIENTS 16
#define BUF_SIZE    2048

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
};

/* 一个还未发 JOIN_REQ 的临时连接 */
struct temp_conn {
    int fd;
    int used;
};

static struct client    g_clients[MAX_CLIENTS];
static struct temp_conn g_temps[MAX_CLIENTS];

/* 房主固定 1，玩家从 2 开始 */
static uint8_t g_next_peer_id = 2;

/* 缓存最近一份 MANIFEST */
static uint8_t g_manifest[4096];
static int     g_manifest_len = 0;

/* 房间号（第 1 周先固定） */
static uint16_t g_room_id = 1;

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

/* 从临时连接表移除一个 fd */
static void temp_remove(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_temps[i].used && g_temps[i].fd == fd) {
            g_temps[i].used = 0;
        }
    }
}

/* 从已加入客户端表移除一个 fd */
static void client_remove(int fd)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].fd == fd) {
            g_clients[i].used = 0;
        }
    }
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

    /* room_id = 0 表示创建新房间，由服务分配 */
    if (room_id == 0) {
        room_id = g_room_id;
    }

    /* 找空闲 client 槽 */
    int slot = -1;
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].used) { slot = i; break; }
    }
    if (slot < 0) {
        send_error(fd, req_id, ERR_ROOM_FULL);
        return;
    }

    /* 分配 peer_id：房主 1，玩家从 2 开始 */
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

    /* 保存 client 信息 */
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
    memset(c->nick, 0, sizeof(c->nick));
    memcpy(c->nick, body + 12, nick_len);

    /* 从临时表移除这个 fd，因为它已经正式加入了 */
    temp_remove(fd);

    log_info("收到 JOIN_REQ：peer_id=%u, nick=%s", peer_id, c->nick);

    /* 构造 JOIN_OK 正文 */
    uint8_t out[1024];
    int off = 0;

    out[off++] = (room_id >> 8) & 0xFF;
    out[off++] = room_id & 0xFF;
    out[off++] = peer_id;

    int peer_n_off = off++;
    uint8_t peer_n = 0;

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_clients[i].used) continue;
        struct client *p = &g_clients[i];
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
        peer_n++;
    }
    out[peer_n_off] = peer_n;

    send_header_and_body(fd, CMD_JOIN_OK, req_id, out, off);

    /* 如果已有缓存 MANIFEST，立即再发一条 */
    if (g_manifest_len > 0) {
        send_header_and_body(fd, CMD_MANIFEST, 0, g_manifest, g_manifest_len);
    }
}

/* ---------- 处理 HEARTBEAT ---------- */

static void handle_heartbeat(int fd, uint16_t req_id)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].fd == fd) {
            g_clients[i].last_heartbeat = time(NULL);
            /* 心跳过密，不打 INFO，避免刷屏；超时仍有日志 */
            break;
        }
    }
    send_header_and_body(fd, CMD_HEARTBEAT_ACK, req_id, NULL, 0);
}

/* ---------- 处理 PROBE ---------- */

static void handle_probe(int udp_fd, uint16_t req_id,
                         const uint8_t *body, uint16_t body_len,
                         struct sockaddr_in *src)
{
    (void)req_id;
    if (body_len < 3 || udp_fd < 0) return;

    uint8_t peer_id = body[2];
    uint32_t ip     = src->sin_addr.s_addr;
    uint16_t port   = ntohs(src->sin_port);

    log_info("收到 PROBE：peer_id=%u, 反射地址=%s:%u",
             peer_id, inet_ntoa(src->sin_addr), port);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_clients[i].used && g_clients[i].peer_id == peer_id) {
            g_clients[i].refl_ip   = ip;
            g_clients[i].refl_port = port;
            break;
        }
    }

    /* PROBE_ACK：4 字节反射 IP + 2 字节反射端口（网络序） */
    uint8_t ack_body[6];
    memcpy(ack_body, &ip, 4);
    uint16_t p_n = htons(port);
    memcpy(ack_body + 4, &p_n, 2);

    struct v1_header h;
    h.magic    = htons(V1_MAGIC);
    h.ver      = V1_VERSION;
    h.cmd      = CMD_PROBE_ACK;
    h.req_id   = htons(0);
    h.body_len = htons(sizeof(ack_body));

    uint8_t pkt[8 + 6];
    memcpy(pkt, &h, 8);
    memcpy(pkt + 8, ack_body, 6);
    sendto(udp_fd, pkt, sizeof(pkt), 0, (struct sockaddr *)src, sizeof(*src));
}

/* ---------- 处理 MANIFEST ---------- */

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

/* ---------- 处理 MANIFEST_REQ ---------- */

static void handle_manifest_req(int fd, uint16_t req_id)
{
    if (g_manifest_len == 0) {
        send_error(fd, req_id, ERR_NO_MANIFEST);
        return;
    }
    send_header_and_body(fd, CMD_MANIFEST, req_id, g_manifest, g_manifest_len);
}

/* ---------- 处理一条完整报文 ---------- */

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

/* ---------- 处理临时连接的数据 ---------- */

static void handle_temp_data(int fd)
{
    uint8_t buf[BUF_SIZE];
    int r = recv(fd, buf, sizeof(buf), 0);
    if (r <= 0) {
        log_info("临时连接断开：fd=%d", fd);
        close(fd);
        temp_remove(fd);
        return;
    }
    if (r < 8) return;

    struct v1_header *h = (struct v1_header *)buf;
    if (ntohs(h->magic) != V1_MAGIC || h->ver != V1_VERSION) {
        log_error("临时连接 magic/ver 错误，关闭 fd=%d", fd);
        close(fd);
        temp_remove(fd);
        return;
    }

    /* 只有 JOIN_REQ 才允许从临时转正式 */
    if (h->cmd == CMD_JOIN_REQ) {
        handle_packet(fd, h, buf + 8, NULL);
    } else {
        log_error("临时连接收到非 JOIN_REQ 指令 0x%02X，关闭 fd=%d", h->cmd, fd);
        close(fd);
        temp_remove(fd);
    }
}

/* ---------- 处理已加入连接的数据 ---------- */

static void handle_client_data(int fd)
{
    uint8_t buf[BUF_SIZE];
    int r = recv(fd, buf, sizeof(buf), 0);
    if (r <= 0) {
        log_info("客户端断开：fd=%d", fd);
        close(fd);
        client_remove(fd);
        return;
    }
    if (r < 8) return;

    struct v1_header *h = (struct v1_header *)buf;
    if (ntohs(h->magic) != V1_MAGIC || h->ver != V1_VERSION) {
        log_error("magic/ver 错误，关闭 fd=%d", fd);
        close(fd);
        client_remove(fd);
        return;
    }
    handle_packet(fd, h, buf + 8, NULL);
}

/* ---------- main ---------- */

int main(void)
{
    log_init("logs/room.log");
    log_info("房间服务启动，TCP 端口 %d，UDP 端口 %d",
             PORT_ROOM_TCP, PORT_ROOM_UDP);

    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("socket"); return 1; }

    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

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

    int udp_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_fd < 0) { perror("udp socket"); return 1; }
    /* UDP 不要 SO_REUSEADDR：多实例同时 bind 时 PROBE 可能进错进程 */

    struct sockaddr_in uaddr;
    memset(&uaddr, 0, sizeof(uaddr));
    uaddr.sin_family      = AF_INET;
    uaddr.sin_addr.s_addr = INADDR_ANY;
    uaddr.sin_port        = htons(PORT_ROOM_UDP);

    if (bind(udp_fd, (struct sockaddr *)&uaddr, sizeof(uaddr)) < 0) {
        perror("udp bind"); return 1;
    }
    log_info("UDP 监听 0.0.0.0:%d", PORT_ROOM_UDP);

    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(listen_fd, &rfds);
        FD_SET(udp_fd, &rfds);
        int maxfd = (listen_fd > udp_fd) ? listen_fd : udp_fd;

        /* 已加入客户端 */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].used) {
                FD_SET(g_clients[i].fd, &rfds);
                if (g_clients[i].fd > maxfd) maxfd = g_clients[i].fd;
            }
        }
        /* 临时连接 */
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

        /* 新 TCP 连接 */
        if (FD_ISSET(listen_fd, &rfds)) {
            struct sockaddr_in caddr;
            socklen_t clen = sizeof(caddr);
            int cfd = accept(listen_fd, (struct sockaddr *)&caddr, &clen);
            if (cfd >= 0) {
                log_info("新 TCP 连接：%s:%d",
                         inet_ntoa(caddr.sin_addr), ntohs(caddr.sin_port));
                /* 放入临时连接表 */
                int placed = 0;
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (!g_temps[i].used) {
                        g_temps[i].used = 1;
                        g_temps[i].fd   = cfd;
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

        /* UDP 探测 */
        if (FD_ISSET(udp_fd, &rfds)) {
            uint8_t buf[BUF_SIZE];
            struct sockaddr_in src;
            socklen_t slen = sizeof(src);
            int r = recvfrom(udp_fd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&src, &slen);
            if (r >= 8) {
                struct v1_header *h = (struct v1_header *)buf;
                if (ntohs(h->magic) == V1_MAGIC &&
                    h->ver == V1_VERSION &&
                    h->cmd == CMD_PROBE) {
                    handle_probe(udp_fd, ntohs(h->req_id),
                                 buf + 8, ntohs(h->body_len), &src);
                }
            }
        }

        /* 临时连接 */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!g_temps[i].used) continue;
            if (!FD_ISSET(g_temps[i].fd, &rfds)) continue;
            handle_temp_data(g_temps[i].fd);
        }

        /* 已加入客户端 */
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (!g_clients[i].used) continue;
            if (!FD_ISSET(g_clients[i].fd, &rfds)) continue;
            handle_client_data(g_clients[i].fd);
        }

        /* 心跳超时 */
        time_t now = time(NULL);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_clients[i].used && g_clients[i].peer_id != 0) {
                if (now - g_clients[i].last_heartbeat > HEARTBEAT_TIMEOUT) {
                    log_info("心跳超时，移除 peer_id=%u", g_clients[i].peer_id);
                    close(g_clients[i].fd);
                    g_clients[i].used = 0;
                }
            }
        }
    }

    log_close();
    return 0;
}