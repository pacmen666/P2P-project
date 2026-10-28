# docs/

项目文档。协议细节以本目录约定为准；动机、实验、分工以方案为准。

## 现有文件

| 文件 | GitHub | 编辑 |
|------|--------|------|
| `碎片档案_v1格式约定.md` | 提交 | 四人签字后**字段只读**；改宽度/指令码须升 `ver` 并改 `include/` |
| `碎片档案_项目方案.md` | 提交 | 可修订实验表述与分工；不得推翻约定里的红线 |
| `week1_liu.md` | 提交 | 刘第 1 周交付说明 |
| `punch_hosts.md` | 提交 | 打洞主机名单表（勿填家庭公网 IP） |
| `experiment4_sockopts.md` | 提交 | 实验 4 套接字选项汇总（刘维护） |
| `experiment7_announce.md` | 提交 | 实验 7 关房通告发现书面记录 |
| `experiment9_relay.md` | 提交 | 实验 9 跨网/中继记录（测试已切 `relay_server`） |
| `全组联调计划_2026-10-28.md` | 提交 | 本周同网段两机联调计划（下发给组员） |
| `wsl.md` | 提交 | WSL 构建、自动化测试命令、关房四窗手工流程 |
| `integration_matrix.md` | 提交 | 四人谁与谁对齐、交付物与完成标准 |
| `transfer_peng.md` | 提交 | 彭传块交付与联调边界 |
| `transfer_validation.md` | 提交 | `make test-transfer` 验证记录 |
| `README.md`（本文件） | 提交 | 可编辑 |

刘侧测试命令索引：`tests/README.md`（脚本明细）、`docs/wsl.md`（WSL 快捷命令）。  
刘侧本机验收摘要：`docs/week1_liu.md`（2026-10-24 / 10-28）；关房通告实测：`docs/experiment7_announce.md`。  
全组对接索引：`docs/integration_matrix.md`。本周联调：`docs/全组联调计划_2026-10-28.md`。

## 规则

| 类别 | 说明 |
|------|------|
| GitHub | 只提交 markdown 等小文档 |
| gitignore | 无额外规则 |
| 本地 | 可另存实验草稿，定稿后再覆盖方案中的实验节或另开 `docs/experiments/` |
| 只读 | v1 约定的字段表、端口、错误码（签字后） |
| 可编辑 | 本 README；方案里的进度说明、演示顺序 |

不要把源码、成品案卷、pcap 放在这里。
