# src/transfer/

玩家之间的数据面：TCP 互传、UDP 子包、位图、多路 `select`。

## 范围

- `PIECE_REQ`（含 `sub_mask`）/ `PIECE_DATA` / `PIECE_NAK` / `BITFIELD` / `HAVE`
- 非阻塞 `connect`，1.5 秒超时；仅较小 `peer_id` 发起连接
- 玩家 TCP：`SO_KEEPALIVE` + 未完成请求 5 秒超时；房间关闭后互发心跳
- UDP：禁止对该打洞 fd 调用 `connect`；`recvfrom` 源地址必须在已认证表
- 首次 `PIECE_REQ`，其后超时只发 NAK 原因 3；`udp_retry` 到 5 改中继

同一对端已有本网 TCP 时不要再走 UDP 要块。第 2 周起必须使用 `generate/` 的真实分块。
