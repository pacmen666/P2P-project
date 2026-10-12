#include "punch.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <netdb.h>

static int pack_header(uint8_t *dst, uint8_t cmd, uint16_t req_id, uint16_t body_len)
{
    uint16_t magic = htons(V1_MAGIC);
    uint16_t rid = htons(req_id);
    uint16_t bl = htons(body_len);
    memcpy(dst, &magic, 2);
    dst[2] = V1_VERSION;
    dst[3] = cmd;
    memcpy(dst + 4, &rid, 2);
    memcpy(dst + 6, &bl, 2);
    return HEADER_SIZE;
}

int punch_open(struct punch_sock *ps)
{
    memset(ps, 0, sizeof(*ps));
    ps->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ps->fd < 0) {
        perror("punch udp socket");
        return -1;
    }

    int yes = 1;
    setsockopt(ps->fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    setsockopt(ps->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    setsockopt(ps->fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
#ifdef FA_TRANSFER_CLIENT
    /* v1: each peer owns an ephemeral PROBE/PUNCH/data socket. Sharing 37404
       with SO_REUSEPORT load-balances unicast between processes incorrectly.
       Broadcast reception is not provided by this transfer integration client. */
    addr.sin_port = htons(0);
    if (bind(ps->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(ps->fd); ps->fd = -1; return -1;
    }
#else
    /* 通告接收必须监听 37404；该 fd 同时承担 PROBE/PUNCH。 */
    addr.sin_port = htons(PORT_ANNOUNCE);

    if (bind(ps->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        addr.sin_port = htons(0);
        if (bind(ps->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
            perror("punch bind");
            close(ps->fd);
            ps->fd = -1;
            return -1;
        }
        log_info("UDP 37404 占用，已回退为临时端口（通告接收可能受限）");
    }
#endif

    socklen_t len = sizeof(addr);
    if (getsockname(ps->fd, (struct sockaddr *)&addr, &len) < 0) {
        perror("punch getsockname");
        close(ps->fd);
        ps->fd = -1;
        return -1;
    }
    ps->local_port = ntohs(addr.sin_port);

    int flags = fcntl(ps->fd, F_GETFL, 0);
    if (flags >= 0)
        fcntl(ps->fd, F_SETFL, flags | O_NONBLOCK);

    log_info("打洞 UDP fd=%d 本地端口=%u（探测/打洞/通告共用，禁止 connect）",
             ps->fd, ps->local_port);
    return 0;
}

void punch_close(struct punch_sock *ps)
{
    if (ps->fd >= 0) {
        close(ps->fd);
        ps->fd = -1;
    }
}

static int resolve_udp(const char *host, uint16_t port, struct sockaddr_in *out)
{
    char portstr[16];
    snprintf(portstr, sizeof(portstr), "%u", port);
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    int e = getaddrinfo(host, portstr, &hints, &res);
    if (e != 0 || !res) {
        log_error("resolve %s:%u 失败：%s", host, port, gai_strerror(e));
        return -1;
    }
    memcpy(out, res->ai_addr, sizeof(*out));
    freeaddrinfo(res);
    return 0;
}

int punch_send_probe(struct punch_sock *ps, const char *room_host,
                     uint32_t room_ip_n, uint16_t room_id, uint8_t peer_id)
{
    if (!ps || ps->fd < 0 || peer_id == 0) return -1;

    uint8_t pkt[HEADER_SIZE + 3];
    pack_header(pkt, CMD_PROBE, 0, 3);
    pkt[HEADER_SIZE + 0] = (uint8_t)((room_id >> 8) & 0xFF);
    pkt[HEADER_SIZE + 1] = (uint8_t)(room_id & 0xFF);
    pkt[HEADER_SIZE + 2] = peer_id;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(PORT_ROOM_UDP);
    if (room_ip_n != 0 && room_ip_n != htonl(INADDR_ANY)) {
        dst.sin_addr.s_addr = room_ip_n;
    } else if (room_host && room_host[0]) {
        if (resolve_udp(room_host, PORT_ROOM_UDP, &dst) < 0) return -1;
    } else {
        dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }

    ssize_t n = sendto(ps->fd, pkt, sizeof(pkt), 0,
                       (struct sockaddr *)&dst, sizeof(dst));
    if (n != (ssize_t)sizeof(pkt)) {
        log_error("PROBE sendto 失败：%s", strerror(errno));
        return -1;
    }
    log_info("已发 PROBE → %s:%u peer_id=%u",
             inet_ntoa(dst.sin_addr), PORT_ROOM_UDP, peer_id);
    return 0;
}

int punch_parse_probe_ack(const uint8_t *body, uint16_t body_len,
                          uint32_t *refl_ip_n, uint16_t *refl_port_h)
{
    if (!body || body_len < 6 || !refl_ip_n || !refl_port_h) return -1;
    memcpy(refl_ip_n, body, 4);
    uint16_t p;
    memcpy(&p, body + 4, 2);
    *refl_port_h = ntohs(p);
    return 0;
}

int punch_send_announce(struct punch_sock *ps, uint16_t room_id,
                        uint32_t session_id, uint32_t host_ip_n,
                        uint16_t tcp_port_h, uint16_t udp_port_h,
                        const char *nick)
{
    if (!ps || ps->fd < 0 || !nick) return -1;
    size_t nick_len = strlen(nick);
    if (nick_len == 0 || nick_len > 16) return -1;

    uint8_t body[64];
    int off = 0;
    body[off++] = (uint8_t)((room_id >> 8) & 0xFF);
    body[off++] = (uint8_t)(room_id & 0xFF);
    uint32_t sid = htonl(session_id);
    memcpy(body + off, &sid, 4); off += 4;
    memcpy(body + off, &host_ip_n, 4); off += 4;
    body[off++] = (uint8_t)((tcp_port_h >> 8) & 0xFF);
    body[off++] = (uint8_t)(tcp_port_h & 0xFF);
    body[off++] = (uint8_t)((udp_port_h >> 8) & 0xFF);
    body[off++] = (uint8_t)(udp_port_h & 0xFF);
    body[off++] = (uint8_t)nick_len;
    memcpy(body + off, nick, nick_len); off += (int)nick_len;

    uint8_t pkt[HEADER_SIZE + 64];
    pack_header(pkt, CMD_ANNOUNCE, 0, (uint16_t)off);
    memcpy(pkt + HEADER_SIZE, body, (size_t)off);

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(PORT_ANNOUNCE);
    dst.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    ssize_t n = sendto(ps->fd, pkt, HEADER_SIZE + off, 0,
                       (struct sockaddr *)&dst, sizeof(dst));
    if (n < 0) {
        log_error("ANNOUNCE sendto 失败：%s", strerror(errno));
        return -1;
    }
    log_info("已发 ANNOUNCE room=%u session=%u", room_id, session_id);
    return 0;
}

int punch_parse_announce(const uint8_t *body, uint16_t body_len,
                         uint16_t *room_id, uint32_t *session_id,
                         uint32_t *host_ip_n, uint16_t *tcp_port_h,
                         uint16_t *udp_port_h, char *nick, size_t nick_cap)
{
    if (!body || body_len < 15 || !nick || nick_cap < 2) return -1;
    *room_id = (uint16_t)((body[0] << 8) | body[1]);
    uint32_t sid;
    memcpy(&sid, body + 2, 4);
    *session_id = ntohl(sid);
    memcpy(host_ip_n, body + 6, 4);
    *tcp_port_h = (uint16_t)((body[10] << 8) | body[11]);
    *udp_port_h = (uint16_t)((body[12] << 8) | body[13]);
    uint8_t nl = body[14];
    if (15 + nl > body_len || nl >= nick_cap) return -1;
    memcpy(nick, body + 15, nl);
    nick[nl] = '\0';
    return 0;
}

int punch_send_punch(struct punch_sock *ps, const struct sockaddr_in *dst,
                     uint32_t session_id, uint8_t from_id, uint8_t to_id)
{
    if (!ps || ps->fd < 0 || !dst || from_id == 0 || to_id == 0) return -1;
    uint8_t pkt[HEADER_SIZE + 6];
    pack_header(pkt, CMD_PUNCH, 0, 6);
    uint32_t sid = htonl(session_id);
    memcpy(pkt + HEADER_SIZE, &sid, 4);
    pkt[HEADER_SIZE + 4] = from_id;
    pkt[HEADER_SIZE + 5] = to_id;
    return sendto(ps->fd, pkt, sizeof(pkt), 0,
                  (const struct sockaddr *)dst, sizeof(*dst)) ==
           (ssize_t)sizeof(pkt) ? 0 : -1;
}

int punch_parse_punch(const uint8_t *body, uint16_t body_len,
                      uint32_t *session_id, uint8_t *from_id, uint8_t *to_id)
{
    if (!body || body_len != 6 || !session_id || !from_id || !to_id) return -1;
    uint32_t sid;
    memcpy(&sid, body, 4);
    *session_id = ntohl(sid);
    *from_id = body[4];
    *to_id = body[5];
    return 0;
}
