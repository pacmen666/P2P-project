# Windows 上通过 WSL 构建与测试

源码为 **Linux / POSIX**（`Makefile` + gcc）。在 Windows 上请用 **WSL2 + Ubuntu**，不要用 MSYS2 作为本仓库默认环境。

仓库在 Windows 上的路径示例：`D:\Coding_Project\CS374\Project`  
在 WSL 内对应：`/mnt/d/Coding_Project/CS374/Project`

## 首次环境（WSL 里执行一次）

```bash
sudo apt update
sudo apt install -y build-essential make python3
```

## 编译

```bash
cd /mnt/d/Coding_Project/CS374/Project
make clean
make              # room_server、relay_server、test_client、client
make transfer     # client_transfer、relay_transfer（传块 / D 窗可选入口）
```

`join` / `next` / `announce` 等是 **`./client` 案情板内命令**，不要在 bash 里直接输入（否则会报 `No such file or directory`）。

## 测试

在仓库根目录执行（会先 `pkill` 旧 `room_server` / `client`，见各脚本）。完整说明见 [`tests/README.md`](../tests/README.md)。

### 推荐自动化（按顺序）

```bash
cd /mnt/d/Coding_Project/CS374/Project
mkdir -p logs
bash tests/probe_only.sh
bash tests/liu_week2_smoke.sh
bash tests/full_liu_smoke.sh
```

或一行：

```bash
bash tests/probe_only.sh && bash tests/liu_week2_smoke.sh && bash tests/full_liu_smoke.sh
```

| 脚本 | 通过标志 |
|------|----------|
| `probe_only.sh` | 退出码 0，`ACK_OK` |
| `liu_week2_smoke.sh` | 输出 `liu_week2_smoke: OK` |
| `full_liu_smoke.sh` | 查看 `logs/client.log` 有 PROBE 相关记录 |

### 可选 / 调试

```bash
bash tests/run_min_probe.sh      # C 最小 probe_client_min
bash tests/run_liu_smoke.sh      # 旧版管道冒烟
bash tests/smoke_week1_liu.sh
bash tests/quick_join.sh         # 需 fuser 时释放端口
python3 tests/probe_ack_check.py # 需已手动起 ./room_server
```

### 手工双终端（快速）

终端 1：`./room_server`  
终端 2：`./client` → `join 侦探 localhost` →（可选）`probe`  
房主通告：再开终端 → `./client` → `join 房主 localhost host` → `announce`

`client` 内常用：`markhave <1-3> [来源]`、`next`、`answer <文本>`、`choose <n>`、`quit`。日志：`logs/client.log`；三项择一汇总：`logs/choice_summary.log`（`client` / `client_transfer` 共用）。Windows 终端输入经 `fa_chomp_line` 规范化，避免 `next` 因行尾 `\r` 显示「未知命令」。

自动化脚本结束时的 `Killed ... ./room_server` 为脚本 `kill -9` 收尾，**不是失败**。

## 关房实验：标准四窗（D 禁止 join）

验证 **room_server 停掉后**，晚进玩家只靠 **房主 UDP `announce`** 发现房主、**TCP 直连**并发送 **MANIFEST_REQ**。  
需要 **4 个 WSL 终端标签**，角色固定；**一个 `./client` 进程只扮演一个角色**，不要在同一窗口里先后 `join 侦探` 再 `join 房主`。

### 角色对照

| 标签 | 进程 | 允许的操作 |
|------|------|------------|
| **A** | `room_server` | 关房时仅在此按 `Ctrl+C`，不要用 `pkill -f client` |
| **B** | 房主 client | `join 房主 localhost host`，之后可多次 `announce` |
| **C** | 侦探 client（可选） | 仅 `join 侦探 localhost`，用于确认**在线**时能收到 ANNOUNCE |
| **D** | 晚进 client | **只** `./client` 或 `make transfer` 后的 `./client_transfer`，**禁止** `join` / `probe` / `announce` |

### 0. 干净启动（建议每次实验前执行一次）

在任意标签执行（会结束所有 room 与 client）：

```bash
cd /mnt/d/Coding_Project/CS374/Project
pkill -9 -f room_server 2>/dev/null
pkill -9 -f './client' 2>/dev/null
sleep 0.5
```

### 1. 标签 A — 房间服务

```bash
cd /mnt/d/Coding_Project/CS374/Project
./room_server
```

保持运行，不要关。

### 2. 标签 B — 房主（必须先于其他人 join）

```bash
cd /mnt/d/Coding_Project/CS374/Project
./client
```

在案情板命令行输入（整行回车）：

```text
join 房主 localhost host
```

等待约 3～5 秒，确认：

- `我：房主 (peer 1)`（若不是 peer 1，回到「0. 干净启动」重来）
- 本机反射 eventually 为 `127.0.0.1:37404`
- `房间服务：在线`

**不要 `quit`，保持 B 运行。**

### 3. 标签 C — 侦探（可选，验证在线 ANNOUNCE）

```bash
cd /mnt/d/Coding_Project/CS374/Project
./client
```

```text
join 侦探 localhost
```

确认 `我：侦探 (peer 2)`（或更大编号）、成员表含房主。

### 4. 标签 B — 在线通告（可选自检）

在 **B** 输入：

```text
announce
```

**C**（若开着）应出现：`路径：通告发现`、`ANNOUNCE：房主 room=1 session=…`。  
已 join 的客户端**不会**自动连房主要清单（设计如此）；此处只确认 UDP 通告可达。

### 5. 标签 D — 晚进玩家（关键：从未 join）

**新开标签 D**（或在关房前再开，见下）：

```bash
cd /mnt/d/Coding_Project/CS374/Project
./client
# 或统一演示入口：make transfer && ./client_transfer
```

**不要输入任何 join 命令。** 界面应长期保持：

- `阶段：空闲`
- `我：- (peer 0)`
- `状态：未进房`
- `房间服务：在线` 或 `离线/未知`（取决于 A 是否还在）

D 窗口在关房前后都**只观察**，不操作（除非实验结束后 `quit`）。

### 6. 标签 A — 关房

切换到 **A**，对 `room_server` 按 **`Ctrl+C`**（不要用 `pkill` 杀 client）。

- **B / C** 若仍开着：数秒内可能显示 `与房间服务断开`、`房间服务：离线/未知`
- **D**：若从未连过 tracker，可能一直显示离线/未知；仍以 peer 0 未进房为准

### 7. 标签 B — 关房后发通告

在 **B（房主 client 仍在跑）** 输入，可间隔 2～3 秒连发 2～3 次：

```text
announce
announce
```

### 8. 标签 D — 验收看什么

关房后 **D** 的「最近事件」理想顺序类似：

1. `ANNOUNCE：房主 room=1 session=…`
2. `通告发现：已连接 房主:<TCP端口>`（如 `33905`，以 B 成员表里房主私网端口为准）
3. 状态行：`已通过通告连接房主，正在索取清单`

**通常不会出现** `已从房主取得 MANIFEST`：当前房主若无陈侧上传的清单缓存（`session` 常为 `00000000`、局号「待清单」），房主会对 `MANIFEST_REQ` 回 **ERR_NO_MANIFEST**，属预期。

若 D **只有 ANNOUNCE、没有「已连接」**，在 D 外另开 shell：

```bash
tail -40 /mnt/d/Coding_Project/CS374/Project/logs/client.log
```

### 9. 常见误操作（避免）

- 在 **D** 里输入 `join 侦探` / `join 房主` → 测的是 tracker 在线进房，**不是**晚进通告发现。
- 同一窗口先侦探再房主 → peer / 成员表混乱。
- `pkill -9 -f './client'` 关房 → 房主被杀，无法 `announce`。
- 本机环回：房主 B 关房后自己也可能收到自己的 ANNOUNCE 并出现「连自己」；**以 D（peer 0）上的事件为准**做验收。

### 10. 实验后收尾

各 client 可输入 `quit`；需要再测时从「0. 干净启动」重来。

## 从 PowerShell 一键进 WSL 编译（可选）

```powershell
wsl bash -lc "cd /mnt/d/Coding_Project/CS374/Project && make clean && make"
```

## 卸载 MSYS2（若曾安装）

1. **关掉**所有 MSYS2 / UCRT64 / mingw 终端窗口。  
2. **删 PATH**：Windows「设置 → 系统 → 关于 → 高级系统设置 → 环境变量」→ 用户 `Path` 中删除含 `msys64`、`ucrt64`、`mingw64` 的项（例如 `C:\msys64\ucrt64\bin`）。  
3. **卸载程序**：设置 → 应用 → 已安装的应用 → 搜索 **MSYS2** → 卸载。或 PowerShell：`winget uninstall MSYS2.MSYS2`（名称以列表为准）。  
4. **删残留目录**（若还在）：删除 `C:\msys64`（需管理员资源管理器或 `Remove-Item -Recurse -Force C:\msys64`）。  
5. **重开 PowerShell**，确认：`where.exe gcc` 不应再指向 `msys64`（未装其它 MinGW 时可能报找不到 gcc，正常）。  
6. 若还装过 **WinLibs** 等独立 MinGW，在「应用」里一并卸载，并删对应 PATH。

本仓库的 `room_server.exe` / `client.exe`（若生成过）可删，WSL 下使用无后缀的 `./room_server`、`./client`。
