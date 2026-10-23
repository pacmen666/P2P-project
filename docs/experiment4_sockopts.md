# 实验 4：套接字选项汇总（刘维护）

记录各模块创建套接字时实际设置的选项，便于验收与对照 v1 约定及方案 ch 5。公网 IP 不写本表。

**说明**

- 下表「选项」列只列 `setsockopt` / `getsockopt`；`fcntl(F_SETFL, O_NONBLOCK)` 与 `connect` 后用 `getsockopt(SO_ERROR)` 验连单独成行，**不算** sockopt 设置项。
- UDP **37404**（`punch_open`）与房间 **37402** 探测口不同；彭侧 peer **listen** 为内核分配的临时 TCP 端口，**未**设 `SO_REUSEADDR`。
- 中继：**正式部署与实验 9 答辩**用 `relay_server`（37405）；`make test-transfer` 仍启动 `relay_transfer`，待彭切测试后再停用适配层（刘不改 `tests/test_transfer.py`）。

| 模块 / 套接字 | 文件 | 选项 / 机制 | 用途 |
|---------------|------|-------------|------|
| 房间 TCP 监听 | `src/tracker/room_server.c` | `SO_REUSEADDR` | 重启后快速绑定 37401 |
| 房间 UDP 37402 | `src/tracker/room_server.c` | `SO_REUSEADDR` | 探测 PROBE / PROBE_ACK |
| 中继 TCP 37405（正式） | `src/relay/relay_server.c` | `SO_REUSEADDR` | 重启绑定；BIND/WRAP 转发 |
| 中继联调适配（待退役） | `src/relay/relay_forward.c` | `SO_REUSEADDR` | 仅 `relay_transfer` + `test-transfer` |
| 中继联调适配 | `src/relay/relay_forward.c` | `fcntl` → `O_NONBLOCK` | 监听套接字非阻塞 accept |
| 刘侧互传 listen | `src/ui/client_main.c` | `SO_REUSEADDR` | 临时 TCP 端口 |
| 彭侧 peer listen | `src/transfer/transfer.c` | （无 `SO_REUSEADDR`） | `INADDR_ANY` + 临时端口 |
| 彭侧 peer listen | `src/transfer/transfer.c` | `fcntl` → `O_NONBLOCK` | 与 `select` 同线程 |
| 打洞/探测/通告 UDP 37404 | `src/punch/udp_sock.c` | `SO_BROADCAST`、`SO_REUSEADDR`、`SO_REUSEPORT`（若系统支持） | ANNOUNCE；本机多实例 |
| 打洞 UDP 37404 | `src/punch/udp_sock.c` | `fcntl` → `O_NONBLOCK` | 与 `select` 同线程 |
| 彭侧 peer TCP（出站/入站） | `src/transfer/transfer.c` | `SO_KEEPALIVE` | 玩家互传长连接 |
| 彭侧 peer TCP | `src/transfer/transfer.c` | `fcntl` → `O_NONBLOCK`；`getsockopt(SO_ERROR)` | 非阻塞 `connect` 完成判定 |
| 刘侧房间 TCP | `src/ui/client_main.c` | `fcntl` → `O_NONBLOCK` | 连 Tracker |
| 彭侧房间 TCP | `src/transfer/client_main.c` | `fcntl` → `O_NONBLOCK`；`getsockopt(SO_ERROR)` | 连 Tracker |
| 实验辅助记录 | `src/common/config.c` | `getsockopt`：`SO_TYPE`、`SO_RCVBUF`、`SO_SNDBUF` | 只读类型与默认缓冲区 |
| 实验辅助对比 | `src/common/config.c` | `SO_REUSEADDR`（`test_reuseaddr`） | 开/关对比第二次 bind |

**约定红线（刘客户端）**

- 打洞用 UDP **禁止** `connect()`；PROBE / PUNCH / ANNOUNCE / 后续子包均 `sendto`。
- 本网互传 `connect` 须非阻塞 + `select`，超时 1.5 s（`include/common.h` 中 `TCP_CONNECT_TIMEOUT_MS`）。彭侧 `client_transfer` 已按此实现。

**本机自检（2026-10-12，WSL Ubuntu，仓库根 `make`）**

- `make clean && make` 产出 `room_server`、`client`。
- 单实例 `./client` 或 `./client_transfer` 可绑定 UDP 37404；同机第二实例需 `SO_REUSEPORT` 或错开启动。
- 彭侧：`make transfer` 后 `client_transfer` 对 peer TCP 开 `SO_KEEPALIVE`；listen 行见上表（无 `SO_REUSEADDR`）。
