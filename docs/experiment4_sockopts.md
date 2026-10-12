# 实验 4：套接字选项汇总（刘维护）

记录各模块创建套接字时实际设置的选项，便于验收与对照 v1 约定。公网 IP 不写本表。

| 模块 / 套接字 | 文件 | 选项 | 用途 |
|---------------|------|------|------|
| 房间 TCP 监听 | `src/tracker/room_server.c` | `SO_REUSEADDR` | 重启后快速绑定 37401 |
| 房间 UDP 37402 | `src/tracker/room_server.c` | `SO_REUSEADDR` | 探测 PROBE / PROBE_ACK |
| 中继 TCP 37405 | `src/relay/relay_server.c` | `SO_REUSEADDR` | 重启绑定 |
| 客户端互传 listen | `src/ui/client_main.c` | `SO_REUSEADDR` | 临时 TCP 端口 |
| 打洞/探测/通告 UDP | `src/punch/udp_sock.c` | `SO_BROADCAST`, `SO_REUSEADDR`, `SO_REUSEPORT`（若系统支持） | 发 ANNOUNCE、收广播；本机多实例 |
| 打洞 UDP | `src/punch/udp_sock.c` | `O_NONBLOCK`（fcntl） | 与 `select` 同线程 |
| 实验辅助 | `src/common/config.c` | `SO_REUSEADDR`（`test_reuseaddr`） | 实验 4 对比 |

**约定红线（刘客户端）**

- 打洞用 UDP **禁止** `connect()`；PROBE / PUNCH / ANNOUNCE / 后续子包均 `sendto`。
- 本网互传 `connect` 须非阻塞 + `select`，超时 1.5 s（第 3 周起与彭状态机对接）。

**本机自检（2026-10-12，WSL Ubuntu，仓库根 `make`）**

- `make clean && make` 产出 `room_server`、`client`。
- 单实例 `./client` 可绑定 UDP 37404；同机第二实例需 `SO_REUSEPORT` 或错开启动。
