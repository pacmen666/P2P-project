# src/transfer/

数据面：TCP 互传、UDP 子包、位图、多路 `select`。

## 范围

- `PIECE_*` / `BITFIELD` / `HAVE`
- 非阻塞 `connect`，1.5 秒；仅较小 `peer_id` 发起
- 玩家 TCP：`SO_KEEPALIVE` + 5 秒请求超时
- UDP：禁止对打洞 fd `connect`；`recvfrom` 须在已认证表
- 超时 NAK 计入 `udp_retry`，到 5 改中继

## 规则

| 类别 | 说明 |
|------|------|
| GitHub | 提交源码与 README |
| gitignore | 不忽略本目录 |
| 本地 | 实验 6 开关、限速可走配置文件（若提交须不含密钥） |
| 只读 | 子包尺寸与「禁止 UDP connect」 |
| 可编辑 | 传送与 select（彭主责）。第 2 周起必须用 `generate/` 真分块 |
