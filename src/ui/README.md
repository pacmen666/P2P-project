# src/ui/

界面线程与第 1 周客户端入口。不在网络套接字上直接读写；操作时向唤醒通道写 1 字节。

## 本周文件

| 文件 | 作用 |
|------|------|
| `ui_state.c` | 局号、昵称、成员、三文件摘要前缀、路径类型（**无种子字段**） |
| `ui_term.c` | 终端案情板 |
| `client_main.c` | 进房、心跳、收 MANIFEST、对接 punch、select + 唤醒 |

构建：仓库根 `make client` → `./client`（Linux / WSL）。说明见 `docs/week1_liu.md`、`docs/wsl.md`。

## 规则

| 类别 | 说明 |
|------|------|
| GitHub | 提交 UI 源码与 README |
| gitignore | 不忽略本目录；截图走 `captures/` |
| 本地 | 可调终端布局 |
| 只读 | 禁止显示种子；路径四类文案与约定第 9 节一致 |
| 可编辑 | 界面（刘主责）。房间表/位图访问须加锁 |
