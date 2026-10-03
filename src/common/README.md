# src/common/

与具体业务无关的收发基础，所有进程都要链进来。

## 范围

- 8 字节公共头的打包 / 解包（`htons` / `ntohl`）
- TCP：`READ_HEADER` / `READ_BODY`，不足 8 字节不得丢已读前缀
- 唤醒通道：Linux 用 `pipe` 或 `eventfd`；Windows 只用环回 `SOCKET` 对
- 错误码与日志前缀

## 规则

| 类别 | 说明 |
|------|------|
| GitHub | 提交本目录源码与 README |
| gitignore | 不忽略 |
| 本地 | 第 1 周可用占位块测实验 1，不依赖 `generate/` |
| 只读 | 无 |
| 可编辑 | `.c` `.h`（彭主责）。改头格式必须同步 `include/` 与 v1 约定 |
