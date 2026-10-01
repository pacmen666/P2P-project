# src/relay/

中继（TCP 37405），与 `tracker` 同机部署，可链成同一进程。

## 范围

- `RELAY_BIND`：`peer_id` 必须已在该房间名单中
- `RELAY_WRAP`：校验 `body_len == 4 + inner_len` 且 `inner_len >= 8`
- `src_id` 必须等于该连接 BIND 登记的 id，否则 `0x0012` 并断开
- 转发给目标时仍封装为 `RELAY_WRAP`，禁止把内层 v1 裸写到中继 TCP 上

只在打洞失败或 UDP 补缺超过 5 次时走这里。统计证物字节时只计内层 `PIECE_DATA` 数据区（陈）。
