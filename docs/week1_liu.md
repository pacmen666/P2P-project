# 第 1 周（刘）：界面骨架、进房、探测与通告

日期：2026-10-07  
对应方案 8.2 第 1 周刘的验收：局号、昵称、空案情板、整文件摘要（不显示种子）；可进入房间；打洞设备名单。

## 交付文件

| 路径 | 说明 |
|------|------|
| `include/ui_state.h` `src/ui/ui_state.c` `src/ui/ui_term.c` | 界面状态与终端案情板 |
| `include/punch.h` `src/punch/udp_sock.c` | 单一 UDP fd：PROBE / ANNOUNCE（禁止 connect） |
| `src/ui/client_main.c` | 客户端：网络线程 select + UI 线程 + 唤醒管道 |
| `docs/punch_hosts.md` | 打洞演示用主机名单（组内填写） |
| `Makefile` 目标 `client` | 构建客户端 |

顺带：房间服务补发 `PROBE_ACK`（`src/tracker/room_server.c`），否则探测无法验收。

## 如何跑（本机双进程）

```bash
make room_server client
mkdir -p logs
./room_server          # 终端 1
./client               # 终端 2
# 在 client 里：
join 侦探 localhost
probe
# 房主另开一终端：
./client
join 房主 localhost host
announce
```

预期：JOIN_OK 后案情板显示 peer 与昵称；输入 `probe` 后本机反射行应更新（`PROBE` 发往房间 TCP 的 `getpeername` 地址:37402）。收到清单时只显示整文件摘要前缀，不出现种子。

自检：`python3 tests/probe_ack_check.py`（需先起 `room_server`）或 `tests/probe_client_min`。起两个 `room_server` 时 UDP 探测会错乱——先杀掉旧进程。

## 本周未做（按计划属后续周）

- 真实 GUI（当前为终端板）
- 完整打洞对发与中继回落（第 4 周）
- 通告后 `MANIFEST_REQ` 走房主互传 TCP（第 3 周实验 7）
- 案情板「来源」列依赖彭的传块（第 2 周）
