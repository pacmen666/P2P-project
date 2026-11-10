# Fragment Archive / 碎片档案

CS374 网络编程课程项目。合作解谜为外壳，核心是 P2P 分块交换、故障恢复与 NAT 穿越。

- 人读入口：本文件  
- Agent 入口：[`AGENT.md`](AGENT.md)  
- 协议：[`docs/碎片档案_v1格式约定.md`](docs/碎片档案_v1格式约定.md)  
- 状态：[`docs/验收总表.md`](docs/验收总表.md)  
- 文档地图：[`docs/README.md`](docs/README.md)

## 仓库布局

| 目录 | 内容 |
|------|------|
| `docs/` | 协议、方案、验收、实验记录、演示提纲 |
| `include/` | 全组共用头文件 |
| `src/` | 实现（按模块） |
| `assets/` | 生成器用零件（`assets/parts/` 为无文字通用证物素材） |
| `tests/` | 可复现测试 |
| `captures/` | 抓包与日志（大体量不提交） |

`src/` 下：`common` 编解码与状态机，`tracker` 房间服务，`relay` 中继，`generate` 案卷生成，`transfer` 分块传送，`punch` 探测/打洞/通告，`ui` 界面，`ai_gen` AI 案卷生成（大模型剧本/AI 出图，见 [`src/ai_gen/README.md`](src/ai_gen/README.md)）。

网页原型在 `ui_prototype/`，由 `python3 ui_prototype/server.py` 启动（仅依赖仓库内文件，字体使用系统字体栈）。
启动后访问 `http://localhost:8080/ui_prototype/index.html`；右上角「⚙ 设置」可填写方舟 API 密钥（仅写入本机 `src/ai_gen/.env`，已被 git 忽略）。
**不配密钥也能用**：点「现场生成新案」会先弹窗提示，确认后走本地程序化模板生成，流程完整可正常游玩；详见 [`src/ai_gen/README.md`](src/ai_gen/README.md)。

## 构建

Linux / WSL：仓库根 `make`。传块入口：`make transfer`（`./client_transfer`）。正式中继：`./relay_server`。  
Windows 开发机见 [`docs/构建说明.md`](docs/构建说明.md)。

测试：`bash tests/probe_only.sh` → `bash tests/join_board_smoke.sh` → `bash tests/full_liu_smoke.sh`；传块 `make test-transfer`。脚本明细见 [`tests/README.md`](tests/README.md)。

## 成员

陈（生成与统计）　彭（传送与多路）　王（房间服务与中继）　刘（界面、通告、打洞客户端）

v1 约定签字后字段宽度与指令码只读。改宽度须升 `ver`。`LICENSE` 只读。
