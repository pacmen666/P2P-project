/*
 * 第 1 周客户端（刘）：终端案情板 + 进房 + 单 UDP 探测/通告骨架。
 * 网络线程 select；界面线程只改状态并通过唤醒管道叫醒网络线程。
 * 禁止显示种子；打洞 UDP 禁止 connect。
 */
#include "common.h"
#include "log.h"
#include "ui_state.h"
#include "punch.h"
#include "fa_stream.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <fcntl.h>

#define MAX_PEER_LINKS 4

struct app;
struct peer_link {
    int fd;
    struct fa_reader reader;
    struct app *app;
    char nick[UI_NICK_MAX + 1];
};

struct app {
    struct ui_state   ui;
    struct punch_sock punch;
    int               room_fd;
    int               peer_listen_fd;
    uint16_t          peer_tcp_port_h;
    uint32_t          priv_ip_n;
    uint32_t          room_ip_n;     /* 房间 TCP 对端，供 PROBE 使用 */
    char              room_host[256];
    uint16_t          room_id;
    uint8_t           role;          /* 1 房主 2 玩家 */
    uint16_t          next_req;
    int               wake_r, wake_w;
    int               stop;
    pthread_t         net_thr;
    struct fa_reader  room_reader;
    time_t            last_hb;
    struct peer_link  links[MAX_PEER_LINKS];
    uint8_t           manifest_raw[4096];
    uint16_t          manifest_len;
    uint32_t          announced_host_ip_n;
    uint16_t          announced_host_tcp_h;
    uint32_t          announced_session;
    uint8_t           punch_wait_peer;   /* 等待互达的 peer_id；0=无 */
    time_t            punch_wait_since;
    uint8_t           punch_ok_peer[UI_MAX_PEERS + 1]; /* 已认证收到 PUNCH */
};

enum wake_cmd {
    WAKE_REDRAW = 1,
    WAKE_JOIN   = 2,
    WAKE_PROBE  = 3,
    WAKE_ANNOUNCE = 4,
    WAKE_QUIT   = 5
};

static void handle_udp(struct app *a);

static void redraw(struct app *a)
{
    ui_state_lock(&a->ui);
    ui_term_draw(&a->ui);
    ui_state_unlock(&a->ui);
}

static void wake_send(struct app *a, uint8_t c)
{
    if (a->wake_w >= 0)
        (void)write(a->wake_w, &c, 1);
}

static int tcp_listen_ephemeral(uint16_t *port_h)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(0);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(a);
    getsockname(fd, (struct sockaddr *)&a, &len);
    *port_h = ntohs(a.sin_port);
    return fd;
}

static int connect_room(const char *host, uint16_t port, uint32_t *priv_ip_n)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, portstr, &hints, &res) != 0 || !res) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { freeaddrinfo(res); return -1; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);
    struct sockaddr_in local;
    socklen_t len = sizeof(local);
    if (getsockname(fd, (struct sockaddr *)&local, &len) == 0)
        *priv_ip_n = local.sin_addr.s_addr;
    return fd;
}

static int send_raw(int fd, uint8_t cmd, uint16_t req,
                    const void *body, uint16_t blen)
{
    uint8_t buf[HEADER_SIZE + 512];
    int n = fa_pack(buf, sizeof(buf), cmd, req, body, blen);
    if (n < 0) return -1;
    int sent = 0;
    while (sent < n) {
        int w = (int)send(fd, buf + sent, (size_t)(n - sent), 0);
        if (w <= 0) return -1;
        sent += w;
    }
    return 0;
}

static int parse_join_ok(struct app *a, const uint8_t *body, uint16_t blen)
{
    if (blen < 4) return -1;
    uint16_t room_id = (uint16_t)((body[0] << 8) | body[1]);
    uint8_t my_id = body[2];
    uint8_t peer_n = body[3];
    int off = 4;

    ui_state_lock(&a->ui);
    a->room_id = room_id;
    a->ui.room_id = room_id;
    a->ui.my_peer_id = my_id;
    a->ui.phase = UI_PHASE_IN_ROOM;
    a->ui.room_online = 1;
    a->ui.path_type = PATH_UNKNOWN;
    ui_clear_peers(&a->ui);
    ui_set_status(&a->ui, "已进房 peer=%u", my_id);

    for (uint8_t i = 0; i < peer_n; i++) {
        if (off + 16 > blen) break;
        struct ui_peer p;
        memset(&p, 0, sizeof(p));
        p.used = 1;
        p.peer_id = body[off++];
        memcpy(&p.priv_ip_n, body + off, 4); off += 4;
        p.tcp_port_h = (uint16_t)((body[off] << 8) | body[off + 1]); off += 2;
        p.udp_port_h = (uint16_t)((body[off] << 8) | body[off + 1]); off += 2;
        memcpy(&p.refl_ip_n, body + off, 4); off += 4;
        p.refl_port_h = (uint16_t)((body[off] << 8) | body[off + 1]); off += 2;
        uint8_t nl = body[off++];
        if (off + nl > blen || nl > UI_NICK_MAX) break;
        memcpy(p.nick, body + off, nl);
        p.nick[nl] = '\0';
        off += nl;
        ui_set_peer(&a->ui, &p);
    }
    ui_state_unlock(&a->ui);
    return 0;
}

static int parse_peer_entries(struct app *a, const uint8_t *body,
                              uint16_t blen, int replace_all)
{
    int off = 0;
    uint8_t peer_n;
    if (off >= blen) return -1;
    peer_n = body[off++];

    ui_state_lock(&a->ui);
    if (replace_all) ui_clear_peers(&a->ui);
    for (uint8_t i = 0; i < peer_n; i++) {
        if (off + 16 > blen) {
            ui_state_unlock(&a->ui);
            return -1;
        }
        struct ui_peer p;
        memset(&p, 0, sizeof(p));
        p.used = 1;
        p.peer_id = body[off++];
        memcpy(&p.priv_ip_n, body + off, 4); off += 4;
        p.tcp_port_h = (uint16_t)((body[off] << 8) | body[off + 1]); off += 2;
        p.udp_port_h = (uint16_t)((body[off] << 8) | body[off + 1]); off += 2;
        memcpy(&p.refl_ip_n, body + off, 4); off += 4;
        p.refl_port_h = (uint16_t)((body[off] << 8) | body[off + 1]); off += 2;
        uint8_t nl = body[off++];
        if (nl > UI_NICK_MAX || off + nl > blen) {
            ui_state_unlock(&a->ui);
            return -1;
        }
        memcpy(p.nick, body + off, nl);
        p.nick[nl] = '\0';
        off += nl;
        ui_set_peer(&a->ui, &p);
    }
    ui_set_status(&a->ui, "成员名单已更新（%u 人）", peer_n);
    ui_push_event(&a->ui, "PEER_LIST：%u 人", peer_n);
    ui_term_draw(&a->ui);
    ui_state_unlock(&a->ui);
    for (int i = 0; i < UI_MAX_PEERS; i++) {
        const struct ui_peer *p = &a->ui.peers[i];
        if (!p->used || p->peer_id == a->ui.my_peer_id) continue;
        if (p->refl_ip_n && p->refl_port_h && a->ui.session_id) {
            struct sockaddr_in dst;
            memset(&dst, 0, sizeof(dst));
            dst.sin_family = AF_INET;
            dst.sin_addr.s_addr = p->refl_ip_n;
            dst.sin_port = htons(p->refl_port_h);
            punch_send_punch(&a->punch, &dst, a->ui.session_id,
                             a->ui.my_peer_id, p->peer_id);
            a->punch_wait_peer = p->peer_id;
            a->punch_wait_since = time(NULL);
        } else if (a->room_fd >= 0 && !p->refl_ip_n) {
            uint16_t req = ++a->next_req;
            if (req == 0) req = ++a->next_req;
            send_raw(a->room_fd, CMD_ADDR_QUERY, req, &p->peer_id, 1);
        }
    }
    return 0;
}

/* MANIFEST：session + seed_hash(忽略展示) + file_n + 文件项 */
static int parse_manifest(struct app *a, const uint8_t *body, uint16_t blen)
{
    if (blen < 37) return -1;
    if (blen <= sizeof(a->manifest_raw)) {
        memcpy(a->manifest_raw, body, blen);
        a->manifest_len = blen;
    }
    uint32_t sid;
    memcpy(&sid, body, 4);
    sid = ntohl(sid);
    const uint8_t *seed_hash = body + 4; /* 不写入界面 */
    uint8_t file_n = body[36];
    int off = 37;

    ui_state_lock(&a->ui);
    ui_apply_manifest_header(&a->ui, sid, seed_hash);
    for (uint8_t i = 0; i < file_n && i < UI_MAX_FILES; i++) {
        if (off + 41 > blen) break;
        uint8_t file_id = body[off];
        uint8_t chapter = body[off + 1];
        uint8_t kind = body[off + 2];
        uint32_t fsz;
        memcpy(&fsz, body + off + 3, 4);
        fsz = ntohl(fsz);
        const uint8_t *fh = body + off + 7;
        uint16_t pc = (uint16_t)((body[off + 39] << 8) | body[off + 40]);
        off += 41 + (int)pc * 32;
        if (off > blen) break;
        ui_set_file_meta(&a->ui, file_id, chapter, kind, fsz, fh);
    }
    ui_set_status(&a->ui, "已收清单 session=%08X（未展示种子）", sid);
    ui_push_event(&a->ui, "MANIFEST：session=%08X，文件=%u", sid, file_n);
    ui_term_draw(&a->ui);
    ui_state_unlock(&a->ui);
    return 0;
}

static struct peer_link *add_peer_link(struct app *a, int fd, const char *nick)
{
    for (int i = 0; i < MAX_PEER_LINKS; i++) {
        if (a->links[i].fd < 0) {
            a->links[i].fd = fd;
            a->links[i].app = a;
            fa_reader_init(&a->links[i].reader);
            snprintf(a->links[i].nick, sizeof(a->links[i].nick), "%s",
                     nick && nick[0] ? nick : "peer");
            fa_nonblock(fd);
            return &a->links[i];
        }
    }
    close(fd);
    return NULL;
}

static int bitfield_is_complete(const uint8_t *bits, uint16_t piece_count)
{
    for (uint16_t i = 0; i < piece_count; i++) {
        if ((bits[i / 8] & (uint8_t)(0x80u >> (i % 8))) == 0)
            return 0;
    }
    return piece_count > 0;
}

static int on_peer_msg(void *ctx, const struct fa_message *m)
{
    struct peer_link *link = ctx;
    struct app *a = link->app;
    if (m->cmd == CMD_MANIFEST_REQ) {
        if (a->manifest_len > 0)
            send_raw(link->fd, CMD_MANIFEST, m->request,
                     a->manifest_raw, a->manifest_len);
        else {
            uint16_t err = htons(ERR_NO_MANIFEST);
            send_raw(link->fd, CMD_ERROR, m->request, &err, sizeof(err));
        }
    } else if (m->cmd == CMD_MANIFEST) {
        parse_manifest(a, m->body, m->length);
        ui_state_lock(&a->ui);
        a->ui.path_type = PATH_ANNOUNCE;
        ui_push_event(&a->ui, "已从房主取得 MANIFEST");
        ui_state_unlock(&a->ui);
        redraw(a);
    } else if (m->cmd == CMD_HAVE && m->length >= 7) {
        uint32_t sid;
        memcpy(&sid, m->body, 4);
        sid = ntohl(sid);
        uint8_t file_id = m->body[4];
        if (sid == a->ui.session_id && file_id >= 1 && file_id <= UI_MAX_FILES) {
            ui_state_lock(&a->ui);
            ui_set_file_status(&a->ui, file_id, 1, link->nick);
            ui_push_event(&a->ui, "HAVE 证物%u 来源=%s", file_id, link->nick);
            ui_term_draw(&a->ui);
            ui_state_unlock(&a->ui);
        }
    } else if (m->cmd == CMD_BITFIELD && m->length >= 8) {
        uint32_t sid;
        memcpy(&sid, m->body, 4);
        sid = ntohl(sid);
        uint8_t file_id = m->body[5];
        uint16_t pieces = (uint16_t)((m->body[6] << 8) | m->body[7]);
        size_t bytes = (pieces + 7u) / 8u;
        if (sid == a->ui.session_id && 8u + bytes <= m->length &&
            bitfield_is_complete(m->body + 8, pieces)) {
            ui_state_lock(&a->ui);
            ui_set_file_status(&a->ui, file_id, 1, link->nick);
            ui_push_event(&a->ui, "证物%u 已完整，来源=%s",
                          file_id, link->nick);
            ui_term_draw(&a->ui);
            ui_state_unlock(&a->ui);
        }
    } else if (m->cmd == CMD_HEARTBEAT) {
        send_raw(link->fd, CMD_HEARTBEAT_ACK, 0, NULL, 0);
    }
    return 0;
}

static int connect_announced_host(struct app *a, uint32_t ip_n,
                                  uint16_t port_h, const char *nick,
                                  uint32_t session)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = ip_n;
    dst.sin_port = htons(port_h);
    int rc = connect(fd, (struct sockaddr *)&dst, sizeof(dst));
    if (rc < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    if (rc < 0) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        struct timeval tv = { .tv_sec = 1, .tv_usec = 500000 };
        rc = select(fd + 1, NULL, &wfds, NULL, &tv);
        int err = 0;
        socklen_t elen = sizeof(err);
        if (rc <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 ||
            err != 0) {
            close(fd);
            return -1;
        }
    }
    struct peer_link *link = add_peer_link(a, fd, nick);
    if (!link) return -1;
    uint32_t sid = htonl(session);
    uint16_t req = ++a->next_req;
    if (req == 0) req = ++a->next_req;
    if (send_raw(fd, CMD_MANIFEST_REQ, req, &sid, sizeof(sid)) < 0) {
        close(fd);
        link->fd = -1;
        return -1;
    }
    ui_state_lock(&a->ui);
    a->ui.path_type = PATH_ANNOUNCE;
    ui_set_status(&a->ui, "已通过通告连接房主，正在索取清单");
    ui_push_event(&a->ui, "通告发现：已连接 %s:%u", nick, port_h);
    ui_term_draw(&a->ui);
    ui_state_unlock(&a->ui);
    return 0;
}

static int on_room_msg(void *ctx, const struct fa_message *m)
{
    struct app *a = ctx;
    switch (m->cmd) {
    case CMD_JOIN_OK:
        if (parse_join_ok(a, m->body, m->length) == 0) {
            punch_send_probe(&a->punch, a->room_host, a->room_ip_n,
                             a->room_id, a->ui.my_peer_id);
            ui_state_lock(&a->ui);
            ui_set_status(&a->ui, "已进房并发送 PROBE");
            ui_push_event(&a->ui, "已发 PROBE → 房间 UDP %u peer=%u",
                          PORT_ROOM_UDP, a->ui.my_peer_id);
            ui_term_draw(&a->ui);
            ui_state_unlock(&a->ui);
        }
        break;
    case CMD_JOIN_ERR:
    case CMD_ERROR: {
        uint16_t err = 0;
        if (m->length >= 2)
            err = (uint16_t)((m->body[0] << 8) | m->body[1]);
        ui_state_lock(&a->ui);
        ui_set_status(&a->ui, "错误 0x%04X", err);
        ui_state_unlock(&a->ui);
        break;
    }
    case CMD_HEARTBEAT_ACK:
        break;
    case CMD_MANIFEST:
        parse_manifest(a, m->body, m->length);
        break;
    case CMD_PEER_LIST:
        parse_peer_entries(a, m->body, m->length, 1);
        break;
    case CMD_ADDR_REPLY:
        if (m->length >= 16) {
            uint8_t wrapped[1 + 64];
            wrapped[0] = 1;
            memcpy(wrapped + 1, m->body, m->length);
            parse_peer_entries(a, wrapped, (uint16_t)(m->length + 1), 0);
        }
        break;
    default:
        log_info("房间报文 cmd=0x%02X len=%u", m->cmd, m->length);
        break;
    }
    return 0;
}

static int do_join(struct app *a)
{
    if (a->room_fd >= 0) {
        close(a->room_fd);
        a->room_fd = -1;
    }
    a->room_fd = connect_room(a->room_host, PORT_ROOM_TCP, &a->priv_ip_n);
    if (a->room_fd < 0) {
        ui_state_lock(&a->ui);
        ui_set_status(&a->ui, "连接房间失败 %s:%u", a->room_host, PORT_ROOM_TCP);
        ui_state_unlock(&a->ui);
        return -1;
    }
    {
        struct sockaddr_in peer;
        socklen_t plen = sizeof(peer);
        if (getpeername(a->room_fd, (struct sockaddr *)&peer, &plen) == 0)
            a->room_ip_n = peer.sin_addr.s_addr;
        else
            a->room_ip_n = htonl(INADDR_LOOPBACK);
    }
    if (a->priv_ip_n == 0 || a->priv_ip_n == htonl(INADDR_ANY)) {
        a->priv_ip_n = inet_addr("127.0.0.1");
    }
    fa_reader_init(&a->room_reader);
    fa_nonblock(a->room_fd);

    uint8_t body[64];
    int off = 0;
    body[off++] = 0;
    body[off++] = 0; /* room_id=0 创建/加入默认房 */
    body[off++] = a->role;
    memcpy(body + off, &a->priv_ip_n, 4); off += 4;
    body[off++] = (uint8_t)((a->peer_tcp_port_h >> 8) & 0xFF);
    body[off++] = (uint8_t)(a->peer_tcp_port_h & 0xFF);
    body[off++] = (uint8_t)((a->punch.local_port >> 8) & 0xFF);
    body[off++] = (uint8_t)(a->punch.local_port & 0xFF);
    size_t nl = strlen(a->ui.my_nick);
    body[off++] = (uint8_t)nl;
    memcpy(body + off, a->ui.my_nick, nl); off += (int)nl;

    uint16_t rid = ++a->next_req;
    if (rid == 0) rid = ++a->next_req;
    if (send_raw(a->room_fd, CMD_JOIN_REQ, rid, body, (uint16_t)off) < 0) {
        ui_state_lock(&a->ui);
        ui_set_status(&a->ui, "发送 JOIN_REQ 失败");
        ui_state_unlock(&a->ui);
        return -1;
    }
    a->last_hb = time(NULL);
    return 0;
}

static void handle_udp(struct app *a)
{
    uint8_t buf[1500];
    struct sockaddr_in src;
    socklen_t slen = sizeof(src);
    ssize_t n = recvfrom(a->punch.fd, buf, sizeof(buf), 0,
                         (struct sockaddr *)&src, &slen);
    if (n < HEADER_SIZE) return;

    uint16_t magic = (uint16_t)((buf[0] << 8) | buf[1]);
    if (magic != V1_MAGIC || buf[2] != V1_VERSION) {
        log_error("UDP 丢弃：magic/ver 不符 from %s:%u",
                  inet_ntoa(src.sin_addr), ntohs(src.sin_port));
        return;
    }
    uint8_t cmd = buf[3];
    uint16_t blen = (uint16_t)((buf[6] << 8) | buf[7]);
    if (n < HEADER_SIZE + blen) return;
    const uint8_t *body = buf + HEADER_SIZE;
    log_info("UDP 收 cmd=0x%02X len=%u from %s:%u",
             cmd, blen, inet_ntoa(src.sin_addr), ntohs(src.sin_port));

    if (cmd == CMD_PROBE_ACK) {
        /* 约定要求来自房间 37402；本机环回测试时也核对地址为房间服务 */
        if (ntohs(src.sin_port) != PORT_ROOM_UDP)
            log_info("PROBE_ACK 来源端口=%u（约定为 %u），仍尝试解析",
                     ntohs(src.sin_port), PORT_ROOM_UDP);
        uint32_t rip = 0;
        uint16_t rport = 0;
        if (punch_parse_probe_ack(body, blen, &rip, &rport) == 0) {
            char ip[32];
            struct in_addr ia = { .s_addr = rip };
            snprintf(ip, sizeof(ip), "%s", inet_ntoa(ia));
            ui_state_lock(&a->ui);
            snprintf(a->ui.refl_text, sizeof(a->ui.refl_text), "%s:%u", ip, rport);
            ui_set_status(&a->ui, "PROBE_ACK 反射 %s:%u", ip, rport);
            ui_push_event(&a->ui, "PROBE_ACK %s:%u", ip, rport);
            for (int i = 0; i < UI_MAX_PEERS; i++) {
                if (a->ui.peers[i].used &&
                    a->ui.peers[i].peer_id == a->ui.my_peer_id) {
                    a->ui.peers[i].refl_ip_n = rip;
                    a->ui.peers[i].refl_port_h = rport;
                    break;
                }
            }
            ui_term_draw(&a->ui);
            ui_state_unlock(&a->ui);
            log_info("PROBE_ACK %s:%u", ip, rport);
        }
    } else if (cmd == CMD_ANNOUNCE) {
        uint16_t room = 0;
        uint32_t sid = 0, hip = 0;
        uint16_t tp = 0, up = 0;
        char nick[17];
        if (punch_parse_announce(body, blen, &room, &sid, &hip, &tp, &up,
                                 nick, sizeof(nick)) == 0) {
            char ip[32];
            struct in_addr ia = { .s_addr = hip };
            snprintf(ip, sizeof(ip), "%s", inet_ntoa(ia));
            ui_state_lock(&a->ui);
            a->ui.path_type = PATH_ANNOUNCE;
            ui_set_status(&a->ui, "收到通告 room=%u host=%s:%u nick=%s",
                          room, ip, tp, nick);
            ui_push_event(&a->ui, "ANNOUNCE：%s room=%u session=%08X",
                          nick, room, sid);
            ui_term_draw(&a->ui);
            ui_state_unlock(&a->ui);
            if (!a->ui.room_online &&
                (a->announced_host_ip_n != hip ||
                 a->announced_host_tcp_h != tp ||
                 a->announced_session != sid)) {
                a->announced_host_ip_n = hip;
                a->announced_host_tcp_h = tp;
                a->announced_session = sid;
                if (connect_announced_host(a, hip, tp, nick, sid) < 0) {
                    ui_state_lock(&a->ui);
                    ui_set_status(&a->ui, "通告房主连接失败 %s:%u", ip, tp);
                    ui_push_event(&a->ui, "通告连接失败");
                    ui_term_draw(&a->ui);
                    ui_state_unlock(&a->ui);
                }
            }
        }
    } else if (cmd == CMD_PUNCH) {
        uint32_t sid = 0;
        uint8_t from = 0, to = 0;
        if (punch_parse_punch(body, blen, &sid, &from, &to) == 0 &&
            sid == a->ui.session_id && to == a->ui.my_peer_id) {
            int authenticated = 0;
            ui_state_lock(&a->ui);
            for (int i = 0; i < UI_MAX_PEERS; i++) {
                const struct ui_peer *p = &a->ui.peers[i];
                if (p->used && p->peer_id == from &&
                    ((p->refl_ip_n == src.sin_addr.s_addr &&
                      p->refl_port_h == ntohs(src.sin_port)) ||
                     (p->priv_ip_n == src.sin_addr.s_addr &&
                      p->udp_port_h == ntohs(src.sin_port)))) {
                    authenticated = 1;
                    break;
                }
            }
            if (authenticated) {
                if (from <= UI_MAX_PEERS)
                    a->punch_ok_peer[from] = 1;
                if (a->punch_wait_peer == from)
                    a->punch_wait_peer = 0;
                a->ui.path_type = PATH_PUNCH;
                ui_set_status(&a->ui, "与 peer %u 打洞报文互达", from);
                ui_push_event(&a->ui, "PUNCH 已认证 peer=%u", from);
                ui_term_draw(&a->ui);
            }
            ui_state_unlock(&a->ui);
            if (authenticated)
                punch_send_punch(&a->punch, &src, sid,
                                 a->ui.my_peer_id, from);
        }
    }
}

static void *net_thread(void *arg)
{
    struct app *a = arg;
    while (!a->stop) {
        fd_set rfds;
        FD_ZERO(&rfds);
        int maxfd = a->wake_r;
        FD_SET(a->wake_r, &rfds);
        if (a->room_fd >= 0) {
            FD_SET(a->room_fd, &rfds);
            if (a->room_fd > maxfd) maxfd = a->room_fd;
        }
        if (a->punch.fd >= 0) {
            FD_SET(a->punch.fd, &rfds);
            if (a->punch.fd > maxfd) maxfd = a->punch.fd;
        }
        if (a->peer_listen_fd >= 0) {
            FD_SET(a->peer_listen_fd, &rfds);
            if (a->peer_listen_fd > maxfd) maxfd = a->peer_listen_fd;
        }
        for (int i = 0; i < MAX_PEER_LINKS; i++) {
            if (a->links[i].fd >= 0) {
                FD_SET(a->links[i].fd, &rfds);
                if (a->links[i].fd > maxfd) maxfd = a->links[i].fd;
            }
        }

        struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
        int n = select(maxfd + 1, &rfds, NULL, NULL, &tv);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (FD_ISSET(a->wake_r, &rfds)) {
            uint8_t cmds[32];
            ssize_t r = read(a->wake_r, cmds, sizeof(cmds));
            for (ssize_t i = 0; i < r; i++) {
                switch (cmds[i]) {
                case WAKE_JOIN:
                    do_join(a);
                    break;
                case WAKE_PROBE:
                    if (a->ui.my_peer_id)
                        punch_send_probe(&a->punch, a->room_host, a->room_ip_n,
                                         a->room_id ? a->room_id : 1,
                                         a->ui.my_peer_id);
                    break;
                case WAKE_ANNOUNCE:
                    if (a->role == 1 && a->ui.my_peer_id == 1)
                        punch_send_announce(&a->punch, a->room_id,
                                            a->ui.session_id, a->priv_ip_n,
                                            a->peer_tcp_port_h,
                                            a->punch.local_port,
                                            a->ui.my_nick);
                    break;
                case WAKE_QUIT:
                    a->stop = 1;
                    break;
                default:
                    break;
                }
            }
        }

        if (a->room_fd >= 0 && FD_ISSET(a->room_fd, &rfds)) {
            uint8_t buf[4096];
            ssize_t r = recv(a->room_fd, buf, sizeof(buf), 0);
            if (r <= 0) {
                close(a->room_fd);
                a->room_fd = -1;
                ui_state_lock(&a->ui);
                a->ui.room_online = 0;
                ui_set_status(&a->ui, "与房间服务断开");
                ui_state_unlock(&a->ui);
            } else {
                fa_feed(&a->room_reader, buf, (size_t)r, on_room_msg, a);
            }
        }

        if (a->punch.fd >= 0 && FD_ISSET(a->punch.fd, &rfds))
            handle_udp(a);

        for (int i = 0; i < MAX_PEER_LINKS; i++) {
            struct peer_link *link = &a->links[i];
            if (link->fd < 0 || !FD_ISSET(link->fd, &rfds)) continue;
            uint8_t buf[4096];
            ssize_t r = recv(link->fd, buf, sizeof(buf), 0);
            if (r <= 0) {
                close(link->fd);
                link->fd = -1;
            } else {
                fa_feed(&link->reader, buf, (size_t)r, on_peer_msg, link);
            }
        }

        if (a->peer_listen_fd >= 0 && FD_ISSET(a->peer_listen_fd, &rfds)) {
            struct sockaddr_in c;
            socklen_t cl = sizeof(c);
            int cfd = accept(a->peer_listen_fd, (struct sockaddr *)&c, &cl);
            if (cfd >= 0) {
                log_info("互传监听接到连接 %s:%u",
                         inet_ntoa(c.sin_addr), ntohs(c.sin_port));
                if (add_peer_link(a, cfd, "通告玩家")) {
                    ui_state_lock(&a->ui);
                    ui_push_event(&a->ui, "互传 TCP 已接入 %s:%u",
                                  inet_ntoa(c.sin_addr), ntohs(c.sin_port));
                    ui_term_draw(&a->ui);
                    ui_state_unlock(&a->ui);
                }
            }
        }

        time_t now = time(NULL);
        if (a->room_fd >= 0 && now - a->last_hb >= HEARTBEAT_INTERVAL) {
            send_raw(a->room_fd, CMD_HEARTBEAT, 0, NULL, 0);
            a->last_hb = now;
        }
        if (a->punch_wait_peer &&
            now - a->punch_wait_since >= PUNCH_TIMEOUT_SEC) {
            uint8_t peer = a->punch_wait_peer;
            a->punch_wait_peer = 0;
            ui_state_lock(&a->ui);
            ui_set_status(&a->ui,
                          "打洞 %us 未互达 peer=%u（第4周改中继）",
                          PUNCH_TIMEOUT_SEC, peer);
            ui_push_event(&a->ui, "PUNCH 超时 peer=%u", peer);
            ui_term_draw(&a->ui);
            ui_state_unlock(&a->ui);
        }
    }
    return NULL;
}

static void *ui_thread(void *arg)
{
    struct app *a = arg;
    char line[256];
    ui_state_lock(&a->ui);
    ui_term_draw(&a->ui);
    ui_state_unlock(&a->ui);

    while (!a->stop && fgets(line, sizeof(line), stdin)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (strncmp(line, "quit", 4) == 0) {
            wake_send(a, WAKE_QUIT);
            break;
        } else if (strncmp(line, "redraw", 6) == 0) {
            /* fall through to draw */
        } else if (strncmp(line, "probe", 5) == 0) {
            wake_send(a, WAKE_PROBE);
        } else if (strncmp(line, "announce", 8) == 0) {
            wake_send(a, WAKE_ANNOUNCE);
        } else if (strncmp(line, "markhave ", 9) == 0) {
            unsigned file_id = 0;
            char source[UI_NICK_MAX + 1] = "test";
            if (sscanf(line + 9, "%u %16s", &file_id, source) >= 1 &&
                file_id >= 1 && file_id <= UI_MAX_FILES) {
                ui_state_lock(&a->ui);
                if (!a->ui.files[file_id - 1].used) {
                    uint8_t zhash[32] = {0};
                    ui_set_file_meta(&a->ui, (uint8_t)file_id,
                                      (uint8_t)file_id, 2, 0, zhash);
                }
                if (ui_set_file_status(&a->ui, (uint8_t)file_id, 1, source) == 0)
                    ui_push_event(&a->ui, "测试注入：证物%u 来源=%s", file_id, source);
                else
                    ui_set_status(&a->ui, "markhave：未知证物 %u", file_id);
                ui_state_unlock(&a->ui);
            }
        } else if (strcmp(line, "next") == 0) {
            ui_state_lock(&a->ui);
            if (a->ui.phase == UI_PHASE_CH1 && a->ui.files[0].have) {
                ui_advance_phase(&a->ui, UI_PHASE_QUIZ);
                ui_push_event(&a->ui, "第一章完成，进入章间问答");
            } else if (a->ui.phase == UI_PHASE_CH2 &&
                       a->ui.files[1].have && a->ui.files[2].have) {
                ui_advance_phase(&a->ui, UI_PHASE_CHOICE);
                ui_push_event(&a->ui, "第二章完成，进入三项择一");
            } else {
                ui_set_status(&a->ui, "证物尚未齐全，不能推进");
            }
            ui_state_unlock(&a->ui);
        } else if (strncmp(line, "answer ", 7) == 0) {
            ui_state_lock(&a->ui);
            if (a->ui.phase == UI_PHASE_QUIZ) {
                ui_advance_phase(&a->ui, UI_PHASE_CH2);
                ui_push_event(&a->ui, "问答已提交，进入第二章");
            } else {
                ui_set_status(&a->ui, "当前不在问答阶段");
            }
            ui_state_unlock(&a->ui);
        } else if (strncmp(line, "choose ", 7) == 0) {
            ui_state_lock(&a->ui);
            if (a->ui.phase == UI_PHASE_CHOICE) {
                ui_advance_phase(&a->ui, UI_PHASE_DONE);
                ui_push_event(&a->ui, "选择已提交，本局结束");
            } else {
                ui_set_status(&a->ui, "当前不在三项择一阶段");
            }
            ui_state_unlock(&a->ui);
        } else if (strncmp(line, "join ", 5) == 0) {
            char nick[UI_NICK_MAX + 1] = {0}, host[128] = "localhost", roleword[32] = {0};
            int n = sscanf(line + 5, "%16s %127s %31s", nick, host, roleword);
            if (n < 1 || !nick[0]) {
                printf("用法：join <昵称> [主机] [host]\n");
            } else {
                ui_state_lock(&a->ui);
                memcpy(a->ui.my_nick, nick, UI_NICK_MAX);
                a->ui.my_nick[UI_NICK_MAX] = '\0';
                a->role = (n >= 3 && strcmp(roleword, "host") == 0) ? 1 : 2;
                ui_state_unlock(&a->ui);
                snprintf(a->room_host, sizeof(a->room_host), "%s",
                         n >= 2 ? host : "localhost");
                wake_send(a, WAKE_JOIN);
            }
        } else if (line[0]) {
            printf("未知命令。\n");
        }
        /* 稍等网络线程；ACK 到达时网络侧会自行重绘 */
        usleep(800000);
        ui_state_lock(&a->ui);
        ui_term_draw(&a->ui);
        ui_state_unlock(&a->ui);
    }
    a->stop = 1;
    wake_send(a, WAKE_QUIT);
    return NULL;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    (void)system("mkdir -p logs");
    log_init("logs/client.log");
    log_set_console(0); /* 清屏案情板；INFO 只进文件，事件看板上「最近事件」 */

    struct app a;
    memset(&a, 0, sizeof(a));
    a.room_fd = -1;
    a.peer_listen_fd = -1;
    a.wake_r = a.wake_w = -1;
    a.role = 2;
    a.next_req = 1;
    for (int i = 0; i < MAX_PEER_LINKS; i++) a.links[i].fd = -1;
    snprintf(a.room_host, sizeof(a.room_host), "localhost");
    ui_state_init(&a.ui);

    if (punch_open(&a.punch) < 0) return 1;
    a.peer_listen_fd = tcp_listen_ephemeral(&a.peer_tcp_port_h);
    if (a.peer_listen_fd < 0) {
        log_error("互传 listen 失败");
        return 1;
    }
    log_info("互传 TCP 监听端口 %u", a.peer_tcp_port_h);

    int pair[2];
    if (fa_wake_open(pair) < 0) {
        perror("wake pipe");
        return 1;
    }
    a.wake_r = pair[0];
    a.wake_w = pair[1];

    pthread_t ui_thr;
    pthread_create(&a.net_thr, NULL, net_thread, &a);
    pthread_create(&ui_thr, NULL, ui_thread, &a);
    pthread_join(ui_thr, NULL);
    a.stop = 1;
    pthread_join(a.net_thr, NULL);

    if (a.room_fd >= 0) close(a.room_fd);
    if (a.peer_listen_fd >= 0) close(a.peer_listen_fd);
    for (int i = 0; i < MAX_PEER_LINKS; i++)
        if (a.links[i].fd >= 0) close(a.links[i].fd);
    punch_close(&a.punch);
    close(a.wake_r);
    close(a.wake_w);
    log_close();
    return 0;
}
