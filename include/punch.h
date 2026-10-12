#ifndef PUNCH_H
#define PUNCH_H

#include "common.h"
#include <netinet/in.h>

/* 进程内唯一打洞 / 探测 / 通告 UDP 套接字。禁止对该 fd 调用 connect。 */

struct punch_sock {
    int      fd;           /* -1 未创建 */
    uint16_t local_port;   /* 主机序，bind 后填入 */
};

int  punch_open(struct punch_sock *ps);
void punch_close(struct punch_sock *ps);

/* JOIN_OK 之后调用：向房间 UDP 37402 发 PROBE，正文带已分配 peer_id。
 * room_ip_n 为房间服务 IPv4（网络序），建议用 getpeername(房间 TCP) 得到，
 * 勿单独依赖 hostname 的 localhost 解析（部分环境可能不一致）。填 0 则回退 host 字符串。 */
int punch_send_probe(struct punch_sock *ps, const char *room_host,
                     uint32_t room_ip_n, uint16_t room_id, uint8_t peer_id);

/* 解析本机已认证的房间服务发来的 PROBE_ACK；成功返回 0 并填写反射地址 */
int punch_parse_probe_ack(const uint8_t *body, uint16_t body_len,
                          uint32_t *refl_ip_n, uint16_t *refl_port_h);

/* 房主通告：发到 255.255.255.255:37404 */
int punch_send_announce(struct punch_sock *ps, uint16_t room_id,
                        uint32_t session_id, uint32_t host_ip_n,
                        uint16_t tcp_port_h, uint16_t udp_port_h,
                        const char *nick);

/* 收一条通告；成功返回 0 */
int punch_parse_announce(const uint8_t *body, uint16_t body_len,
                         uint16_t *room_id, uint32_t *session_id,
                         uint32_t *host_ip_n, uint16_t *tcp_port_h,
                         uint16_t *udp_port_h, char *nick, size_t nick_cap);

int punch_send_punch(struct punch_sock *ps, const struct sockaddr_in *dst,
                     uint32_t session_id, uint8_t from_id, uint8_t to_id);
int punch_parse_punch(const uint8_t *body, uint16_t body_len,
                      uint32_t *session_id, uint8_t *from_id, uint8_t *to_id);

#endif
