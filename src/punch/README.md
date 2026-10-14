# src/punch/

打洞客户端、探测、局域网通告。中继**服务端**在 `src/relay/`。

## 文件

| 文件 | 作用 |
|------|------|
| `udp_sock.c` | 单一 UDP fd：`PROBE` / `PROBE_ACK` / `ANNOUNCE` / `PUNCH`；优先 bind 37404，失败回退临时端口 |

`client` 与 `client_transfer` 共用本模块。**禁止**对该 fd 调用 `connect()`；PROBE / PUNCH / ANNOUNCE 均 `sendto`。

第 3 周：同网段通告与关房晚进在 `src/ui/client_main.c` 已验收（四窗见 `docs/wsl.md`）。`client_transfer` 目前只发 `announce`、不收广播。第 4 周：完整对发认证、NAT 类型记录（`docs/punch_hosts.md`）与失败回落中继，与彭 `tr_should_punch` / `tr_accept_udp` 对接。

## 规则

| 类别 | 说明 |
|------|------|
| GitHub | 提交客户端源码与 README |
| gitignore | 不忽略 |
| 本地 | NAT 类型记在 `docs/punch_hosts.md`，勿提交家庭公网 IP |
| 只读 | 「同一 fd、禁止另建 socket」 |
| 可编辑 | 打洞与通告（刘主责） |
