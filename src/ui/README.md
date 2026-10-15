# src/ui/

界面线程与终端案情板。不在网络套接字上直接读写；操作时向唤醒通道写 1 字节。房间表与证物状态须加锁。

## 文件

| 文件 | 作用 |
|------|------|
| `ui_state.c` | 局号、昵称、成员、三文件摘要前缀、已有/缺失/来源、路径类型（**无种子字段**） |
| `ui_term.c` | 终端案情板；`./client` 与 `client_transfer` 共用 |
| `client_main.c` | 刘侧入口 `./client`：进房、PEER_LIST、PROBE、ANNOUNCE、关房后 MANIFEST_REQ、PUNCH 骨架、`markhave` / `next` / `answer` / `choose` |

构建：仓库根 `make client` → `./client`（Linux / WSL）。传块联调入口是 `make transfer` → `./client_transfer`（`src/transfer/client_main.c` 复用本目录 `ui_state` / `ui_term`）。说明见 `docs/week1_liu.md`、`docs/wsl.md`。

## 路径文案（约定四类）

`PATH_LAN_TCP` 本网直连、`PATH_PUNCH` 打洞、`PATH_RELAY` 中继、`PATH_ANNOUNCE` 通告发现。刘侧 `./client` 主要置 `PATH_ANNOUNCE` / `PATH_PUNCH`；本网直连与中继由 `client_transfer` 写入 `ui->path_type`。

## 规则

| 类别 | 说明 |
|------|------|
| GitHub | 提交 UI 源码与 README |
| gitignore | 不忽略本目录；截图走 `captures/` |
| 本地 | 可调终端布局 |
| 只读 | 禁止显示种子；路径四类文案与约定第 9 节一致 |
| 可编辑 | 界面（刘主责）。房间表/位图访问须加锁 |
