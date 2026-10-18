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

    printf("发送 JOIN_REQ：role=%u, nick_len=%u, nick=%s\n", role, nick_len, nick);
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
        printf("JOIN_OK：room_id=%u, 我的 peer_id=%u, 房间人数=%u\n", room_id, my_id, peer_n);
        return my_id;
    }
    return -1;
}

static void test_peer_list(int fd)
{
    struct v1_header h;
    uint8_t resp[1024];
    int r = recv_packet(fd, &h, resp, sizeof(resp));
    if (r >= 0 && h.cmd == CMD_PEER_LIST) {
        printf("收到 PEER_LIST：房间人数=%u\n", resp[0]);
    }
}

static void test_heartbeat(int fd)
{
    send_packet(fd, CMD_HEARTBEAT, 2, NULL, 0);
    struct v1_header h;
    uint8_t resp[64];
    if (recv_packet(fd, &h, resp, sizeof(resp)) >= 0 && h.cmd == CMD_HEARTBEAT_ACK) {
        printf("HEARTBEAT_ACK：心跳正常\n");
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

static void test_relay_stress(int fd, uint8_t src_id, uint8_t dst_id)
{
    uint16_t inner_len = 16384 + 8; 
    uint8_t *inner = malloc(inner_len);
    
    struct v1_header ih;
    ih.magic    = htons(V1_MAGIC);
    ih.ver      = V1_VERSION;
    ih.cmd      = CMD_PIECE_DATA;
    ih.req_id   = htons(99);
    ih.body_len = htons(16384);
    memcpy(inner, &ih, 8);
    memset(inner + 8, 0xAA, 16384);

    uint16_t body_len = 4 + inner_len;
    uint8_t *body = malloc(body_len);
    body[0] = dst_id;
    body[1] = src_id;
    body[2] = (inner_len >> 8) & 0xFF;
    body[3] = inner_len & 0xFF;
    memcpy(body + 4, inner, inner_len);

    struct v1_header h;
    h.magic    = htons(V1_MAGIC);
    h.ver      = V1_VERSION;
    h.cmd      = CMD_RELAY_WRAP;
    h.req_id   = htons(7);
    h.body_len = htons(body_len);

    printf("【测试】发送 16KB WRAP (src=%u, dst=%u)\n", src_id, dst_id);
    send(fd, &h, 8, 0);
    send(fd, body, body_len, 0);

    free(inner);
    free(body);
}

int main(int argc, char *argv[])
{
    const char *room_ip  = "127.0.0.1";
    const char *relay_ip = "127.0.0.1";
    uint8_t role = 2;
    const char *nick = "Alice";
    uint8_t dst_id = 1;

    if (argc >= 2) room_ip  = argv[1];
    if (argc >= 3) relay_ip = argv[2];
    if (argc >= 4) role     = atoi(argv[3]);
    if (argc >= 5) nick     = argv[4];
    if (argc >= 6) dst_id   = atoi(argv[5]);

    log_init("logs/test_client.log");

    // 1. 连房间服务
    int fd = connect_to(room_ip, PORT_ROOM_TCP);
    if (fd < 0) return 1;
    int my_id = test_join(fd, role, nick);
    test_peer_list(fd);
    test_heartbeat(fd);
    
    // 2. 连中继
    int rfd = connect_to(relay_ip, PORT_RELAY_TCP);
    if (rfd < 0) {
        close(fd);
        return 1;
    }
    printf("已连接中继服务\n");

    // 3. 发 BIND
    test_relay(rfd, my_id, 1);

    // 4. 等待对方连上中继并完成 BIND
    printf("等待 6 秒...\n");
    sleep(6);

    // 5. 发 16KB 测试包
    test_relay_stress(rfd, my_id, dst_id);

    // 6. 保持连接
    printf("保持 10 秒...\n");
    sleep(10);

    close(rfd);
    close(fd); // 先关中继，再关房间
    log_close();
    return 0;
}