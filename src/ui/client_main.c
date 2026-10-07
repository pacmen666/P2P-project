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
};

enum wake_cmd {
    WAKE_REDRAW = 1,
    WAKE_JOIN   = 2,
    WAKE_PROBE  = 3,
    WAKE_ANNOUNCE = 4,
    WAKE_QUIT   = 5
};

static void handle_udp(struct app *a);

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

/* MANIFEST：session + seed_hash(忽略展示) + file_n + 文件项 */
static int parse_manifest(struct app *a, const uint8_t *body, uint16_t blen)
{
    if (blen < 37) return -1;
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
        ui_state_lock(&a->ui);
        ui_set_status(&a->ui, "收到 PEER_LIST（%u 字节）", m->length);
        ui_state_unlock(&a->ui);
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
            ui_state_unlock(&a->ui);
        }
    } else if (cmd == CMD_PUNCH) {
        log_info("收到 PUNCH（第1周仅记录）来自 %s:%u",
                 inet_ntoa(src.sin_addr), ntohs(src.sin_port));
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

        if (a->peer_listen_fd >= 0 && FD_ISSET(a->peer_listen_fd, &rfds)) {
            struct sockaddr_in c;
            socklen_t cl = sizeof(c);
            int cfd = accept(a->peer_listen_fd, (struct sockaddr *)&c, &cl);
            if (cfd >= 0) {
                log_info("互传监听接到连接（第1周暂不传块）%s:%u",
                         inet_ntoa(c.sin_addr), ntohs(c.sin_port));
                close(cfd);
            }
        }

        time_t now = time(NULL);
        if (a->room_fd >= 0 && now - a->last_hb >= HEARTBEAT_INTERVAL) {
            send_raw(a->room_fd, CMD_HEARTBEAT, 0, NULL, 0);
            a->last_hb = now;
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
    punch_close(&a.punch);
    close(a.wake_r);
    close(a.wake_w);
    log_close();
    return 0;
}
