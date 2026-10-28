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

在 **Linux 实验机** 或 **WSL**（Windows 路径 `D:\Coding_Project\CS374\Project` → WSL 内 `/mnt/d/Coding_Project/CS374/Project`）。见 `docs/wsl.md`。

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

## 自动化测试（WSL / Linux）

```bash
cd /mnt/d/Coding_Project/CS374/Project
mkdir -p logs
bash tests/probe_only.sh
bash tests/liu_week2_smoke.sh
bash tests/full_liu_smoke.sh
```

详见 `tests/README.md`。起两个 `room_server` 时 UDP 探测会错乱——脚本会先 `pkill` 旧进程。

## 第 2～3 周已做（刘侧）

- `PEER_LIST` 成员表、`markhave` 联调证物来源
- 房主 `announce`、已 join 端收 `ANNOUNCE`（路径「通告发现」）
- 关房后晚进玩家（**peer 0、不 join**）收通告并 TCP `MANIFEST_REQ`：手工四窗流程见 `docs/wsl.md`
- `next` / `answer` / `choose` 命令骨架；真实局号依赖陈侧 MANIFEST
- 套接字选项总表：`docs/experiment4_sockopts.md`

**与 `client_transfer`：** `ui_state` / `ui_term` / punch 已被彭复用（`make transfer`）。真传块、HAVE 点亮证物、本网直连/中继路径以 `./client_transfer` 为准。关房晚进通告 + `MANIFEST_REQ`：`./client` 与 `./client_transfer`（`tr_connect_announced`）均可作 D 窗，见 `docs/experiment7_announce.md`。

## 本机验收（2026-10-24，`LAP-NICKY` / WSL）

| 项 | 命令 / 现象 | 证据 |
|----|-------------|------|
| 自动化 | `bash tests/probe_only.sh && bash tests/liu_week2_smoke.sh && bash tests/full_liu_smoke.sh` | `ACK_OK`、`liu_week2_smoke: OK`、`logs/client.log` 含 PROBE |
| 章节 + 择一 | `./client`：`join` → `markhave 1–3` → `next` → `answer 1` → `next` → `choose 2` | `logs/choice_summary.log` 含 `nick=侦探 choice=2` |
| 在线通告 | 房主 `announce`，侦探已 join | `路径：通告发现`、`ANNOUNCE` |
| 关房晚进 | 四窗，D 禁止 join | 见 `experiment7_announce.md` 实测表 |
| `make test-transfer` | 基线 `9017eb7`，WSL 全绿 | 见 `transfer_validation.md` 2026-10-28 节 |

交互行读取：`fa_chomp_line`（`include/fa_stream.h`）去掉 `\r\n` 与首尾空白，避免 Windows 终端下 `next` 被误判为「未知命令」。

## 仍未做 / 依赖他组

- 真实 GUI（当前为终端板）
- 完整打洞对发、NAT 记录与中继回落（第 4 周；彭 `tr_should_punch` 已有骨架）
- 房主缓存并下发真实 MANIFEST（陈 + 王侧上传）
- 案情板来源由彭 HAVE/BITFIELD 驱动（`client_transfer` 已接；`./client` 仍可用 `markhave` 联调）
- 实验 9 跨网 / 第二台机（模板 `docs/experiment9_relay.md`）；`answer`/`choose` 正文校验仍为流程占位
