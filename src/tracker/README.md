# src/tracker/

房间服务（TCP 37401，探测 UDP 37402）。不保存完整案卷，必须缓存本局 `MANIFEST`。

## 范围

- `JOIN_REQ` / `JOIN_OK` / 心跳 / `PEER_LIST`
- `recvfrom` 登记反射地址；变更后向全房推送名单
- `JOIN_OK` 后紧跟缓存的 `MANIFEST`；响应 `MANIFEST_REQ`
- 监听 `SO_REUSEADDR`；实验 2 迭代/并发开关；实验 5 主机名

私网 IPv4 以客户端 `JOIN_REQ` 上报为准。

## 规则

| 类别 | 说明 |
|------|------|
| GitHub | **必须提交**本目录。忽略规则只能是仓库根 `/tracker`（可执行文件），不能是裸词 `tracker` |
| gitignore | 本目录不被忽略 |
| 本地 | 运行产生的日志放 `captures/` 或临时文件，不要写进本目录再提交 |
| 只读 | 端口号以约定为准 |
| 可编辑 | 房间服务源码（王主责） |
