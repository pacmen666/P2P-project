# src/relay/

中继（TCP 37405），与 `tracker` 同机，可链成同一进程。

## 范围

- `RELAY_BIND`：`peer_id` 须在该房间名单中
- `RELAY_WRAP`：`body_len == 4 + inner_len` 且 `inner_len >= 8`
- `src_id` 等于该连接 BIND 登记，否则 `0x0012` 并断开
- 转发给目标时仍封装 `RELAY_WRAP`，禁止内层 v1 裸写到中继 TCP

## 规则

| 类别 | 说明 |
|------|------|
| GitHub | 提交源码与 README |
| gitignore | 不忽略 |
| 本地 | 与 tracker 联调日志不提交 |
| 只读 | 封装长度恒等式、身份绑定规则 |
| 可编辑 | 中继实现（王主责）。统计口径由陈在 `generate/` 计数，不在此改公式 |
