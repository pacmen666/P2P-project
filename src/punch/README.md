# src/punch/

打洞客户端、向房间探测、局域网通告。中继服务端不在本目录（见 `src/relay/`）。

## 范围

- 进程内**一个** UDP fd：`bind(INADDR_ANY, 0)`，探测、打洞、打洞后子包共用
- `JOIN_OK` 之后再 `PROBE`，必须带已分配 `peer_id`
- `PUNCH`；8 秒未互达或强制失败则通知改连中继
- 通告 UDP 37404、`SO_BROADCAST`；多实例才用 `SO_REUSEPORT`
- 房间关闭后：通告里的房主互传 TCP 用于 `MANIFEST_REQ`（实验 7）

反射地址为 0 时不得对发。实验 9 记录锥型 / 对称型，不得把中继完成写成打洞成功。
