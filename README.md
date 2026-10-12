# Fragment Archive / 碎片档案

CS374 网络编程课程项目。合作解谜为外壳，核心是 P2P 分块交换、故障恢复与 NAT 穿越。

- 人读入口：本文件  
- Agent 入口：同目录 [`AGENT.md`](AGENT.md)（与本 README 并列，不互相替代）  
- 协议与方案：`docs/`（不要在 README 里重复字段宽度）

## 仓库布局

| 目录 | 内容 |
|------|------|
| `docs/` | 项目方案、v1 格式约定、实验记录 |
| `include/` | 全组共用的协议头与常量 |
| `src/` | 实现（按模块，不按人名） |
| `assets/` | 生成器用零件，禁止本局成品 |
| `tests/` | 可复现的测试步骤与用例 |
| `captures/` | 抓包与日志（大体量不提交） |

`src/` 下：`common` 编解码与状态机，`tracker` 房间服务，`relay` 中继，`generate` 案卷生成，`transfer` 分块传送，`punch` 探测/打洞/通告，`ui` 界面。

## 规则（全库）

| 类别 | 说明 |
|------|------|
| GitHub | 提交文档、头文件、源码、各目录 README、`AGENT.md`、`LICENSE`、`.gitignore` |
| gitignore | 根目录可执行文件 `/tracker`、`/client`（**不会**忽略 `src/tracker/`）；`*.o` `*.exe`；本局 `session_*/`；`captures/` 下 pcap/日志/录像 |
| 本地 | 可保留生成输出、pcap、私有笔记；不要指望 clone 后能得到这些 |
| 只读 | `LICENSE`；`docs/碎片档案_v1格式约定.md` 在四人签字后**字段宽度/指令码只读**（要改必须升 `ver` 并全组确认） |
| 可编辑 | 本 README、`AGENT.md`、各子目录 README、方案中的实验记录与分工微调、`src/` `include/` `tests/` `assets/` 零件 |

改忽略规则时：禁止再写裸词 `tracker`，必须写成 `/tracker`，否则 `src/tracker/` 无法进库。

## 构建

**Linux / WSL（推荐）**：仓库根 `make` 或 `make room_server relay_server test_client client`。  
Windows 开发机见 [`docs/wsl.md`](docs/wsl.md)。不要把 C 运行时 `pipe` 丢进 Winsock `select`（见 `AGENT.md`）。

刘侧说明见 `docs/week1_liu.md`。  
**测试（WSL）**：`bash tests/probe_only.sh` → `bash tests/liu_week2_smoke.sh` → `bash tests/full_liu_smoke.sh`；关房四窗与全部脚本见 [`tests/README.md`](tests/README.md)、[`docs/wsl.md`](docs/wsl.md)。

## 成员

陈（生成与统计）　彭（传送与多路）　王（房间服务与中继）　刘（界面、通告、打洞客户端）
