# 实验 9：跨网 / 中继（记录模板）

正式中继进程为 **`./relay_server`**（TCP 37405，`RELAY_BIND` / `RELAY_WRAP`）。`make test-transfer` **已切** `relay_server`（`Makefile` / 提交 `9017eb7`）。历史适配层 `relay_transfer` 不再作为测试默认进程，见 [`experiment4_sockopts.md`](experiment4_sockopts.md)。

## 前置

- [`punch_hosts.md`](punch_hosts.md) 填齐组员机 2/3（内网主机名即可，勿写家庭公网 IP）。
- 王侧：`relay_server` 已监听 37405。
- 彭侧：`client_transfer --mode relay` 或自动路径下 UDP 失败回落。

## 待测场景（有第二台机后勾选）

| # | 场景 | 命令 / 条件 | 路径栏（`client_transfer`） | 中继字节 |
|---|------|-------------|------------------------------|----------|
| 1 | 本网直连 | 同机或同网段，`--mode tcp` 或默认 | 本网直连 | 0 |
| 2 | UDP 打洞 | 锥型 NAT，`--mode udp` 或默认回落 | 打洞 | 0 |
| 3 | 中继回落 | 对称 NAT 或 `--mode relay` | 中继 | `relay_up` > 0 |
| 4 | 锥型成功 | 打洞互达 | 打洞 | **不得**宣称中继成功 |

## 记录栏（每次实验填一行）

| 日期 | A 机 | B 机 | NAT（A/B） | 模式 | 结果 | 备注 |
|------|------|------|------------|------|------|------|
| | LAP-NICKY | （待填） | 未知 / 待测 | | | |

统计输出：`client_transfer` 退出时 `TRANSFER_STATS` 行中的 `relay_up`；王侧 `relay_server` 日志中的转发字节（实验 9 打印）。
