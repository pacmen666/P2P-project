#ifndef COMMON_H
#define COMMON_H
#include <stdint.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#define V1_MAGIC      0x4641
#define V1_VERSION    1
#define PORT_ROOM_TCP    37401
#define PORT_ROOM_UDP    37402
#define PORT_ANNOUNCE    37404
#define PORT_RELAY_TCP   37405
//指令码
#define CMD_JOIN_REQ       0x01
#define CMD_JOIN_OK        0x02
#define CMD_JOIN_ERR       0x03
#define CMD_HEARTBEAT      0x04
#define CMD_HEARTBEAT_ACK  0x05
#define CMD_PEER_LIST      0x06
#define CMD_MANIFEST       0x07
#define CMD_BITFIELD       0x08
#define CMD_HAVE           0x09
#define CMD_PIECE_REQ      0x0A
#define CMD_PIECE_DATA     0x0B
#define CMD_PIECE_NAK      0x0C
#define CMD_PROBE          0x0D
#define CMD_PROBE_ACK      0x0E
#define CMD_PUNCH          0x0F
#define CMD_RELAY_BIND     0x10
#define CMD_RELAY_WRAP     0x11
#define CMD_ANNOUNCE       0x12
#define CMD_ERROR          0x13
#define CMD_ADDR_QUERY     0x14
#define CMD_ADDR_REPLY     0x15
#define CMD_MANIFEST_REQ   0x16
//错误码
#define ERR_MAGIC            0x0001
#define ERR_VERSION          0x0002
#define ERR_UNKNOWN_CMD      0x0003
#define ERR_BODY_LEN         0x0004
#define ERR_ROOM_FULL        0x0005
#define ERR_ROOM_NOT_FOUND   0x0006
#define ERR_NICK_DUP         0x0007
#define ERR_PIECE_HASH       0x0008
#define ERR_NO_PIECE         0x0009
#define ERR_PEER_NO_HAVE     0x000A
#define ERR_TIMEOUT          0x000B
#define ERR_PUNCH_FAIL       0x000C
#define ERR_RELAY_UNAVAIL    0x000D
#define ERR_MANIFEST_BAD     0x000E
#define ERR_SESSION_MISMATCH 0x000F
#define ERR_WRAP_LEN         0x0010
#define ERR_NO_MANIFEST      0x0011
#define ERR_RELAY_ID         0x0012
//其他
#define LOGIC_PIECE_SIZE     16384   // 逻辑分块 16 KiB
#define SUB_PACKET_SIZE      1024    // 子包 1024 字节
#define MAX_UDP_DATAGRAM     1200    // UDP 数据报最大 1200 字节
#define HEADER_SIZE          8       // 公共头部 8 字节

#define HEARTBEAT_INTERVAL   5       // 心跳间隔 5 秒
#define HEARTBEAT_TIMEOUT    15      // 心跳超时 15 秒
#define TCP_CONNECT_TIMEOUT_MS 1500  // 本网 connect 超时 1.5 秒
#define PIECE_NAK_TIMEOUT_MS   500   // UDP 超时补缺 500 ms
#define UDP_RETRY_MAX          5     // UDP 超时补缺 5 次
#define PUNCH_TIMEOUT_SEC      8     // 打洞超时 8 秒
#define TCP_PIECE_TIMEOUT_SEC  5     // TCP 未完成请求 5 秒超时
#define MAX_PEERS              4     // 房间最多 4 人
#define SESSION_ID_SIZE        4     // session_id 4 字节
#define SHA256_SIZE            32    // SHA-256 32 字节
#define SEED_HASH_SIZE         32    // seed_hash 32 字节

struct v1_header {
    uint16_t magic;     // 0x4641
    uint8_t  ver;       // 1
    uint8_t  cmd;       // 指令码
    uint16_t req_id;    // 请求编号
    uint16_t body_len;  // 正文长度
} __attribute__((packed));

struct peer_entry {
    uint8_t  peer_id;
    uint32_t priv_ip;      // 私网 IPv4，网络序
    uint16_t tcp_port;     // TCP 互传端口，网络序
    uint16_t udp_port;     // UDP 本地端口，网络序
    uint32_t refl_ip;      // 反射 IPv4，网络序
    uint16_t refl_port;    // 反射 UDP 端口，网络序
    uint8_t  nick_len;
    char     nick[16];
} __attribute__((packed));

struct file_entry {
    uint8_t  file_id;
    uint8_t  chapter;
    uint8_t  kind;
    uint32_t file_size;
    uint8_t  file_hash[32];
    uint16_t piece_count;
    uint8_t  piece_hash[][32];  // 变长，实际用时另算
} __attribute__((packed));

#define PATH_UNKNOWN   0
#define PATH_LAN_TCP   1
#define PATH_PUNCH     2
#define PATH_RELAY     3
#define PATH_ANNOUNCE  4

#endif // COMMON_H