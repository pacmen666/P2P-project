#include "common.h"
#include "log.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static int send_packet(int fd, uint8_t cmd, uint16_t req_id,
                       const void *body, uint16_t body_len)
{
    struct v1_header h;
    h.magic    = htons(V1_MAGIC);
    h.ver      = V1_VERSION;
    h.cmd      = cmd;
    h.req_id   = htons(req_id);
    h.body_len = htons(body_len);

    if (send(fd, &h, sizeof(h), 0) != sizeof(h)) return -1;
    if (body_len > 0 && body) {
        if (send(fd, body, body_len, 0) != body_len) return -1;
    }
    return 0;
}

static int recv_packet(int fd, struct v1_header *h, uint8_t *body, int body_max)
{
    int r = recv(fd, h, sizeof(*h), MSG_WAITALL);
    if (r != sizeof(*h)) return -1;
    if (ntohs(h->magic) != V1_MAGIC) return -2;
    if (h->ver != V1_VERSION) return -3;

    uint16_t blen = ntohs(h->body_len);
    if (blen > body_max) return -4;

    if (blen > 0) {
        r = recv(fd, body, blen, MSG_WAITALL);
        if (r != blen) return -5;
    }
    return blen;
}

static int connect_to(const char *ip, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = inet_addr(ip);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return -1;
    }
    return fd;
}

/* 加入房间，返回 peer_id */
static int test_join(int fd, uint8_t role, const char *nick)
{
    uint8_t body[64];
    int off = 0;

    body[off++] = 0;
    body[off++] = 0;
    body[off++] = role;
    uint32_t ip = inet_addr("127.0.0.1");
    memcpy(body + off, &ip, 4); off += 4;
    uint16_t tcp_port = htons(50000);
    memcpy(body + off, &tcp_port, 2); off += 2;
    uint16_t udp_port = htons(50001);
    memcpy(body + off, &udp_port, 2); off += 2;
    uint8_t nick_len = strlen(nick);
    body[off++] = nick_len;
    memcpy(body + off, nick, nick_len); off += nick_len;

    printf("发送 JOIN_REQ：role=%u, nick_len=%u, nick=%s\n",
           role, nick_len, nick);
    send_packet(fd, CMD_JOIN_REQ, 1, body, off);

    struct v1_header h;
    uint8_t resp[1024];
    int r = recv_packet(fd, &h, resp, sizeof(resp));
    if (r < 0) {
        printf("recv JOIN_OK 失败：%d\n", r);
        return -1;
    }
    if (h.cmd == CMD_JOIN_OK) {
        uint16_t room_id = (resp[0] << 8) | resp[1];
        uint8_t  my_id   = resp[2];
        uint8_t  peer_n  = resp[3];
        printf("JOIN_OK：room_id=%u, 我的 peer_id=%u, 房间人数=%u\n",
               room_id, my_id, peer_n);
        return my_id;
    } else if (h.cmd == CMD_JOIN_ERR) {
        uint16_t err = (resp[0] << 8) | resp[1];
        printf("JOIN_ERR：错误码 0x%04X\n", err);
    } else {
        printf("未知响应：cmd=0x%02X\n", h.cmd);
    }
    return -1;
}

/* 单独处理 PEER_LIST */
static void test_peer_list(int fd)
{
    struct v1_header h;
    uint8_t resp[1024];
    int r = recv_packet(fd, &h, resp, sizeof(resp));
    if (r < 0) {
        printf("recv PEER_LIST 失败：%d\n", r);
        return;
    }
    if (h.cmd == CMD_PEER_LIST) {
        uint8_t peer_n = resp[0];
        printf("收到 PEER_LIST：房间人数=%u\n", peer_n);
    } else {
        printf("未知响应：cmd=0x%02X\n", h.cmd);
    }
}

static void test_heartbeat(int fd)
{
    send_packet(fd, CMD_HEARTBEAT, 2, NULL, 0);

    struct v1_header h;
    uint8_t resp[64];
    int r = recv_packet(fd, &h, resp, sizeof(resp));
    if (r < 0) {
        printf("recv HEARTBEAT_ACK 失败：%d\n", r);
        return;
    }
    if (h.cmd == CMD_HEARTBEAT_ACK) {
        printf("HEARTBEAT_ACK：心跳正常\n");
    } else {
        printf("未知响应：cmd=0x%02X\n", h.cmd);
    }
}

static void test_probe(const char *room_ip, uint8_t peer_id)
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("udp socket"); return; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(PORT_ROOM_UDP);
    addr.sin_addr.s_addr = inet_addr(room_ip);

    uint8_t body[3];
    body[0] = 0;
    body[1] = 0;
    body[2] = peer_id;

    struct v1_header h;
    h.magic    = htons(V1_MAGIC);
    h.ver      = V1_VERSION;
    h.cmd      = CMD_PROBE;
    h.req_id   = htons(4);
    h.body_len = htons(3);

    uint8_t packet[8 + 3];
    memcpy(packet, &h, 8);
    memcpy(packet + 8, body, 3);

    sendto(fd, packet, sizeof(packet), 0,
           (struct sockaddr *)&addr, sizeof(addr));
    printf("已发送 PROBE\n");

    uint8_t resp[64];
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    int r = recvfrom(fd, resp, sizeof(resp), 0,
                     (struct sockaddr *)&from, &flen);
    if (r >= 14) {
        struct v1_header *rh = (struct v1_header *)resp;
        if (rh->cmd == CMD_PROBE_ACK) {
            uint32_t ip;
            uint16_t port;
            memcpy(&ip, resp + 8, 4);
            memcpy(&port, resp + 12, 2);
            printf("PROBE_ACK：你的公网地址是 %s:%u\n",
                   inet_ntoa(*(struct in_addr *)&ip), ntohs(port));
        } else {
            printf("未知响应：cmd=0x%02X\n", rh->cmd);
        }
    } else {
        printf("PROBE_ACK 接收失败：%d\n", r);
    }
    close(fd);
}

static void test_addr_query(int fd, uint8_t target)
{
    uint8_t body[1];
    body[0] = target;

    send_packet(fd, CMD_ADDR_QUERY, 5, body, 1);
    printf("已发送 ADDR_QUERY：目标 peer_id=%u\n", target);

    struct v1_header h;
    uint8_t resp[256];
    int r = recv_packet(fd, &h, resp, sizeof(resp));
    if (r < 0) {
        printf("recv ADDR_REPLY 失败：%d\n", r);
        return;
    }
    if (h.cmd == CMD_ADDR_REPLY) {
        printf("ADDR_REPLY：收到 peer_id=%u 的地址\n", target);
    } else if (h.cmd == CMD_ERROR) {
        uint16_t err = (resp[0] << 8) | resp[1];
        printf("ADDR_QUERY 失败：0x%04X\n", err);
    } else {
        printf("未知响应：cmd=0x%02X\n", h.cmd);
    }
}

static void test_relay(int fd, uint8_t peer_id, uint16_t room_id)
{
    uint8_t body[3];
    body[0] = peer_id;
    body[1] = (room_id >> 8) & 0xFF;
    body[2] = room_id & 0xFF;

    send_packet(fd, CMD_RELAY_BIND, 6, body, 3);
    printf("已发送 RELAY_BIND：peer_id=%u, room_id=%u\n", peer_id, room_id);
}

int main(int argc, char *argv[])
{
    const char *room_ip  = "127.0.0.1";
    const char *relay_ip = "127.0.0.1";
    uint8_t role = 2;
    const char *nick = "Alice";

    if (argc >= 2) room_ip  = argv[1];
    if (argc >= 3) relay_ip = argv[2];
    if (argc >= 4) role     = atoi(argv[3]);
    if (argc >= 5) nick     = argv[4];

    log_init("logs/test_client.log");
    log_info("测试客户端启动：room=%s, relay=%s, role=%u, nick=%s",
             room_ip, relay_ip, role, nick);

    int fd = connect_to(room_ip, PORT_ROOM_TCP);
    if (fd < 0) {
        printf("连接房间服务失败\n");
        return 1;
    }
    printf("已连接房间服务 %s:%d\n", room_ip, PORT_ROOM_TCP);

    int my_id = test_join(fd, role, nick);

    /* 房间服务在 JOIN_OK 后立即推 PEER_LIST */
    test_peer_list(fd);

    test_heartbeat(fd);
    test_addr_query(fd, 1);

    test_probe(room_ip, my_id);

    int rfd = connect_to(relay_ip, PORT_RELAY_TCP);
    if (rfd < 0) {
        printf("连接中继服务失败\n");
        return 1;
    }
    printf("已连接中继服务 %s:%d\n", relay_ip, PORT_RELAY_TCP);
    test_relay(rfd, my_id, 1);
    close(rfd);

    printf("保持在线 30 秒...\n");
    sleep(30);

    close(fd);
    log_close();
    return 0;
}