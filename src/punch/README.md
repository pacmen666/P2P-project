# src/punch/

打洞客户端、探测、局域网通告。中继**服务端**在 `src/relay/`。

## 本周文件

| 文件 | 作用 |
|------|------|
| `udp_sock.c` | 单一 UDP fd：`bind(0)`，`PROBE` / 解析 `PROBE_ACK` / `ANNOUNCE` |

**禁止**对该 fd 调用 `connect()`。完整 `PUNCH` 对发与失败回落中继在第 4 周。

## 规则

| 类别 | 说明 |
|------|------|
| GitHub | 提交客户端源码与 README |
| gitignore | 不忽略 |
| 本地 | NAT 类型记在 `docs/punch_hosts.md`，勿提交家庭公网 IP |
| 只读 | 「同一 fd、禁止另建 socket」 |
| 可编辑 | 打洞与通告（刘主责） |
