# AGENT.md

给在本仓库工作的编码 Agent（Cursor 等）用，与根目录 `README.md` 并列。人看 README；Agent **先读本文件再读 `docs/`**，不要只扫 README 就改协议。

## 必读顺序

1. 本文件  
2. `docs/碎片档案_v1格式约定.md`  
3. `docs/碎片档案_项目方案.md` 第 4、6、8 节  
4. 将要改的那个子目录里的 `README.md`

字段宽度、端口、指令码、错误码以约定为准。约定与实现冲突时，**改代码去符合约定**，不得擅自改约定（除非任务明确要求升 `ver`）。

## 允许改 / 禁止改

| 路径 | Agent |
|------|--------|
| `src/**/*.c` `src/**/*.h`（模块范围内） | 可编辑；跨模块先对一下职责表 |
| `include/*.h` | 可编辑，但增删字段等于改协议，须同步 `docs` 约定并升 `ver` |
| `tests/` | 可编辑步骤与用例，不把 pcap 写进来 |
| `assets/` | 只可加零件（字体、空白底图、姓名表），禁止写本局成品 |
| `docs/碎片档案_v1格式约定.md` | **默认只读**。用户明确要求改协议时才动，且必须升 `ver`、通知四人 |
| `docs/碎片档案_项目方案.md` | 可补实验记录与措辞，不得把已否决的设计写回去（种子上线、16 KiB 整包 UDP、打洞另建 socket 等） |
| `LICENSE` | 只读 |
| `.gitignore` | 可编辑，但 `/tracker`、`/client` 必须带前导 `/` |
| `captures/*.pcap` 等 | **不要 git add**；本地可写 |
| `session_*/` `*.bin` | **不要提交** |

不要按成员姓名建目录（禁止 `陈/` `彭/`）。不要把微信稿、课程 PPT、临时脚本留在仓库根目录。

## 协议红线（实现时不得违反）

- 种子不下发、不进界面、不进日志对外字段。  
- 打洞 / 探测 / 打洞后 UDP 子包共用**同一个** UDP fd；禁止对该 fd `connect()`。  
- UDP 应用层整包（8 字节头 + 体）≤ 1200；逻辑块 16 KiB 仅 TCP 或切子包后走 UDP。  
- `JOIN_OK` 之后再 `PROBE`，且带已分配 `peer_id`。  
- `JOIN_REQ` 必须带本机私网 IPv4，不得填 `0.0.0.0`。  
- 中继：`src_id` 等于该连接 `RELAY_BIND`；转发给对端仍包一层 `RELAY_WRAP`。  
- Windows：`select` 只放 `SOCKET`，禁止 CRT `_pipe`。  
- 本网 `connect` 非阻塞、1.5 秒；仅较小 `peer_id` 发起。

## 本机开发与文档

- Windows 开发机：默认 **WSL2 + Ubuntu**，路径 `/mnt/d/.../Project`，`make` 构建；见 `docs/wsl.md`。  
- 源码按 **Linux POSIX** 维护，不在此仓库做 MSYS2/原生 Windows 编译主线。

## Git

- 默认在当前分支工作；用户未要求时不要 `commit` / `push` / `--force`。  
- 用户要求提交时：不要把 `.gitignore` 已忽略的文件用 `-f` 加进去。  
- 不要改 `git config`。  
- 远程仓库：`https://github.com/Flin0498/Fragment_Archive.git`。

## 对用户说话

用户规则要求简体中文。不要把「已完成」说成 GitHub 已有，除非已经 `push` 且远程能列出该路径。
