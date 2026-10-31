# src/punch/

打洞客户端、探测、局域网通告。中继**服务端**在 `src/relay/`。

## 文件

| 文件 | 作用 |
|------|------|
| `udp_sock.c` | 单一 UDP fd：`PROBE` / `PROBE_ACK` / `ANNOUNCE` / `PUNCH`；优先 bind 37404，失败回退临时端口 |

`client` 与 `client_transfer` 共用本模块。**禁止**对该 fd 调用 `connect()`；PROBE / PUNCH / ANNOUNCE 均 `sendto`。

同网段通告与关房晚进在 `src/ui/client_main.c` 已验收（四窗见 [`docs/构建说明.md`](../../docs/构建说明.md)，书面 [`docs/实验记录.md`](../../docs/实验记录.md) 实验 7）。`client_transfer` 可发 `announce` 并收广播后 `tr_connect_announced`。主机登记见 [`docs/主机登记.md`](../../docs/主机登记.md)。打洞与彭 `tr_should_punch` / `tr_accept_udp` 对接。

## 规则

| 类别 | 说明 |
|------|------|
| GitHub | 提交客户端源码与 README |
| gitignore | 不忽略 |
| 本地 | NAT 类型记在 `docs/主机登记.md`。家庭公网地址不写入仓库 |
| 只读 | 「同一 fd、禁止另建 socket」 |
| 可编辑 | 打洞与通告（刘主责） |
