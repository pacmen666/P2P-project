# src/common/

与具体业务无关的收发基础，所有进程都要链进来。

## 范围

- 8 字节公共头的打包 / 解包（`htons` / `ntohl`）
- TCP：`READ_HEADER` / `READ_BODY`，不足 8 字节不得丢已读前缀
- 唤醒通道：Linux 用 `pipe` 或 `eventfd`；Windows 只用环回 `SOCKET` 对
- 错误码与日志前缀的公共实现

第 1 周彭可用占位块做实验 1，不依赖 `generate/`。
