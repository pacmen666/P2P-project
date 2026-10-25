# 实验 7：同网段通告发现（书面记录）

## 目标

房间服务（Tracker）关闭后，晚进客户端不执行 `join`，仅监听 UDP **37404** 上的 `CMD_ANNOUNCE`，再对房主 **TCP 非阻塞直连**（1.5 s 超时）并发送 `CMD_MANIFEST_REQ`。

## 环境

| 项 | 值 |
|----|-----|
| 日期 | 2026-10-24（WSL Ubuntu，`LAP-NICKY`） |
| 网络 | 本机环回 / 同一 WSL，**不跨路由** |
| 端口 | Tracker TCP 37401、UDP 37402；通告 UDP 37404 |
| 入口 | D 窗 `./client`（刘侧）；同等路径可用 `./client_transfer`（`tr_connect_announced`） |
| 构建 | 根目录 `make` 含 `client`；`make transfer` 含 `client_transfer` |

## 步骤摘要

标准四窗见 [`wsl.md`](wsl.md)「关房实验」：A=`room_server`，B=房主 `join … host`，C=可选侦探，**D=仅启动客户端、禁止 join**。A 关房后 B 多次 `announce`，D 应出现 ANNOUNCE → TCP 已连 →「正在索取清单」。

## 实测记录（2026-10-24，`LAP-NICKY`）

| 阶段 | 操作 | 结果 |
|------|------|------|
| 在线通告（B/C 已 join） | 房主 `announce` | 侦探窗：`路径：通告发现`，`ANNOUNCE：房主 room=1 session=00000000`，状态「收到通告 … nick=房主」；**不**自动 `MANIFEST_REQ` |
| 关房四窗 | A `Ctrl+C` 停 `room_server`；B 连发 `announce`；D 从未 `join` | D：`阶段：空闲`、`peer 0` → 事件 `ANNOUNCE` → `通告发现：已连接 房主:32867` → 状态「已通过通告连接房主，正在索取清单」 |
| 清单 | 房主无陈侧 `session` 缓存 | 无「已从房主取得 MANIFEST」；`session=00000000`、顶栏「待清单」为预期 |
| 副作用 | 本机环回 | 房主窗也可能看到自己的 `ANNOUNCE` 与「互传 TCP 已接入」；**书面验收以 D（peer 0）为准** |

日志：`logs/client.log`（多进程共用文件名时按时间戳辨认 D 窗对应行）。

## 实测结论（D = peer 0）

- D 在未 join 状态下能收到 `ANNOUNCE：房主 room=1 session=…`。
- 随后出现通告路径连接（状态「已通过通告连接房主，正在索取清单」；事件含房主 TCP 端口，如 `32867` 等临时端口）。
- 房主若无陈侧 `manifest` 缓存时，**不会**出现「已从房主取得 MANIFEST」；对 `MANIFEST_REQ` 回 `ERR_NO_MANIFEST` 为预期。
- 在线成员 C 可收到 ANNOUNCE 但不会自动索要清单（与方案一致）。

## 与代码对应

- 发送：`punch_send_announce`（`src/punch/udp_sock.c`）。
- 接收 + 连房主：`src/ui/client_main.c`；统一入口 `src/transfer/client_main.c` + `tr_connect_announced`（`src/transfer/transfer.c`）。
- 路径栏：`PATH_ANNOUNCE` → 界面「通告发现」。

## 未覆盖（留待跨网）

- 非环回、宿舍多机、路由器隔离下的通告可达性（依赖实验 9 前 `punch_hosts.md` 填机位）。
