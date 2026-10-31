# tests/

可复现的测试步骤与小型用例。pcap 本体不放这里。

## 规则

| 类别 | 说明 |
|------|------|
| GitHub | 提交步骤 md、小型输入、脚本；不提交抓包原文件 |
| gitignore | 无额外规则（pcap 在 `captures/` 忽略）；`logs/` 已忽略，勿提交运行日志 |
| 本地 | 可跑实验 0–9 |
| 只读 | 无 |
| 可编辑 | 步骤与断言。实验书面记录见 [`docs/实验记录.md`](../docs/实验记录.md) |

每条记录：命令、操作系统与网络、预期、实际、日志/抓包**文件名**、commit。

## 环境（Linux / WSL）

路径示例（Windows 开发机）：

```bash
cd /mnt/d/Coding_Project/CS374/Project
sudo apt install -y build-essential make python3   # 首次
# 默认 all：room_server、relay_server、test_client、client
make clean && make
mkdir -p logs
```

Windows 上使用 WSL2，见 [`docs/构建说明.md`](../docs/构建说明.md)。跑脚本前若端口占用，可先：

```bash
pkill -9 -f room_server 2>/dev/null
pkill -9 -f './client' 2>/dev/null
sleep 0.5
```

---

## 推荐执行顺序（刘侧自动化）

| 顺序 | 命令 | 说明 | 通过标志 |
|------|------|------|----------|
| 1 | `bash tests/probe_only.sh` | 起 `room_server`，Python 发最小 PROBE，收 PROBE_ACK | 退出码 0，打印 `ACK_OK` |
| 2 | `bash tests/join_board_smoke.sh` | 进房 + PROBE_ACK/PEER_LIST + `markhave` 案情板 | 打印 `join_board_smoke: OK` |
| 3 | `bash tests/full_liu_smoke.sh` | 管道驱动 `client`：`join` + `probe` | 查看 `logs/client.log` 含 PROBE 相关记录 |

**一键三连（CI / 本地自检）：**

```bash
cd /mnt/d/Coding_Project/CS374/Project
bash tests/probe_only.sh && bash tests/join_board_smoke.sh && bash tests/full_liu_smoke.sh
```

---

## 脚本明细

### `probe_only.sh`

- 构建 `room_server`，后台运行，执行 `python3 tests/probe_ack_check.py`，结束后杀 room。
- **依赖**：`room_server` 监听 UDP 37402。
- **不**构建 `client`。

### `join_board_smoke.sh`

- 构建 `room_server` + `client`，管道输入：`join 侦探 localhost` → `markhave 1 测试源` → `redraw` → `quit`。
- **断言**：`logs/client.log` 或 `logs/client_run.log` 含 `PROBE_ACK` 或 `PEER_LIST`；运行日志含「已有」。

### `full_liu_smoke.sh`

- 构建 `room_server` + `client`，管道：`join detA localhost` → `probe` → `quit`。
- **无严格 exit 断言**；失败时人工看 `logs/client.log`、`logs/client_run.log`。

### `run_min_probe.sh`

- 构建 `room_server` + `make probe_client_min`，跑 C 最小探测 `tests/probe_client_min`。
- 与 `probe_only.sh` 类似，走刘侧 punch 栈而非纯 Python 包头。

### `quick_join.sh`

释放 37401/37402 后做一次 `join`。需本机有 `fuser`。日常以 `probe_only` + `join_board_smoke` + `full_liu_smoke` 为准。

---

## 手工交互（不经过脚本）

### 双终端：进房 + 探测

终端 A：

```bash
./room_server
```

终端 B：

```bash
./client
join 侦探 localhost
probe
```

### 双终端：房主通告（在线）

终端 B 另开或第三终端：

```bash
./client
join 房主 localhost host
announce
```

已 join 的侦探端应见 `ANNOUNCE`；**不会**自动 `MANIFEST_REQ`（设计如此）。

### 客户端命令（`./client` 内）

| 命令 | 说明 |
|------|------|
| `join 侦探 localhost` | 玩家进房 |
| `join 房主 localhost host` | 房主进房（须 peer 1 才能发有效 `announce`） |
| `probe` | 再发 PROBE（join 后通常已自动发） |
| `announce` | 房主 UDP 通告（`role==1 && peer_id==1`） |
| `markhave <1-3> [来源]` | 联调：证物已有 + 来源 |
| `next` / `answer <文本>` / `choose <n>` | 游戏阶段（需有成局清单后才有完整效果） |
| `redraw` | 重绘案情板 |
| `quit` | 退出 |

完整日志：`logs/client.log`（需 `mkdir -p logs`）。

---

## 关房 + 晚进通告（手工四窗，无自动化脚本）

四窗手工步骤见 [`docs/构建说明.md`](../docs/构建说明.md)「关房实验」。D 窗保持未进房。

**D 验收要点（peer 0、未进房）**：`ANNOUNCE` → `通告发现：已连接 房主:<端口>` → 状态「正在索取清单」。  
通常**无**「已从房主取得 MANIFEST」（房主无陈侧清单缓存时为预期）。

---

## Python / 其它

| 路径 | 用途 |
|------|------|
| `tests/probe_ack_check.py` | `probe_only.sh` 调用；最小 PROBE_ACK |
| `tests/check_manifest.py` | 清单格式检查（陈侧联调） |
| `tests/py_test/` | 生成与案卷相关脚本，见 `tests/py_test/README.md` |
| `make test_client` | 王侧 TCP 测试客户端，非刘冒烟主路径 |

---

## 彭侧传块（`make transfer`）

```bash
sudo apt-get install -y libssl-dev   # 缺 OpenSSL 时
make transfer
make test-transfer
```

生成 `room_server`、`client_transfer`、正式 `relay_server`。说明见 [`docs/传块模块.md`](../docs/传块模块.md)。测试占用 37401、37402、37405，先结束旧 `room_server` / `client`。`make test-transfer` 使用 `relay_server`。

## 王侧 / 全组（参考）

```bash
make room_server relay_server test_client
./room_server    # 终端 1
./test_client    # 按 tests/test_client.c 说明
```

相关文档：[`docs/界面与通告.md`](../docs/界面与通告.md)、[`docs/实验记录.md`](../docs/实验记录.md)、[`docs/构建说明.md`](../docs/构建说明.md)。
