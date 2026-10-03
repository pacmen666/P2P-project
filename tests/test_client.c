#include "common.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* 发送一条完整报文 */
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

/* 接收一条完整报文 */
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

/* 连接服务器 */
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

/* 测试 1：加入房间 */
static void test_join(int fd, uint8_t role, const char *nick)
{
    uint8_t body[64];
    int off = 0;

    /* room_id = 0（创建新房间） */
    body[off++] = 0;
    body[off++] = 0;
    /* role */
    body[off++] = role;
    /* 私网 IP 先填 127.0.0.1 */
    uint32_t ip = inet_addr("127.0.0.1");
    memcpy(body + off, &ip, 4); off += 4;
    /* TCP 互传端口 */
    uint16_t tcp_port = htons(50000);
    memcpy(body + off, &tcp_port, 2); off += 2;
    /* UDP 端口 */
    uint16_t udp_port = htons(50001);
    memcpy(body + off, &udp_port, 2); off += 2;
    /* 昵称 */
    uint8_t nick_len = strlen(nick);
    body[off++] = nick_len;
    memcpy(body + off, nick, nick_len); off += nick_len;

    send_packet(fd, CMD_JOIN_REQ, 1, body, off);

    struct v1_header h;
    uint8_t resp[1024];
    int r = recv_packet(fd, &h, resp, sizeof(resp));
    if (r < 0) {
        printf("recv JOIN_OK 失败：%d\n", r);
        return;
    }
    if (h.cmd == CMD_JOIN_OK) {
        uint16_t room_id = (resp[0] << 8) | resp[1];
        uint8_t  my_id   = resp[2];
        uint8_t  peer_n  = resp[3];
        printf("JOIN_OK：room_id=%u, 我的 peer_id=%u, 房间人数=%u\n",
               room_id, my_id, peer_n);
    } else if (h.cmd == CMD_JOIN_ERR) {
        uint16_t err = (resp[0] << 8) | resp[1];
        printf("JOIN_ERR：错误码 0x%04X\n", err);
    } else {
        printf("未知响应：cmd=0x%02X\n", h.cmd);
    }
}

/* 测试 2：心跳 */
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

/* 测试 4：中继骨架 */
static void test_relay(int fd, uint8_t peer_id, uint16_t room_id)
{
    uint8_t body[3];
    body[0] = peer_id;
    body[1] = (room_id >> 8) & 0xFF;
    body[2] = room_id & 0xFF;

    send_packet(fd, CMD_RELAY_BIND, 3, body, 3);
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

    /* 测试 1、2：房间服务 */
    int fd = connect_to(room_ip, PORT_ROOM_TCP);
    if (fd < 0) {
        printf("连接房间服务失败\n");
        return 1;
    }
    printf("已连接房间服务 %s:%d\n", room_ip, PORT_ROOM_TCP);

    test_join(fd, role, nick);
    test_heartbeat(fd);

    /* 测试 4：中继骨架 */
    int rfd = connect_to(relay_ip, PORT_RELAY_TCP);
    if (rfd < 0) {
        printf("连接中继服务失败\n");
        return 1;
    }
    printf("已连接中继服务 %s:%d\n", relay_ip, PORT_RELAY_TCP);
    test_relay(rfd, role, 1);
    close(rfd);

    /* 关键：保持连接 30 秒，方便第二个客户端看到自己 */
    printf("保持在线 30 秒...\n");
    sleep(30);

    close(fd);
    log_close();
    return 0;
}