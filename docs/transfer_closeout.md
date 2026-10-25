# 传输收尾与单机验收（彭，2026-10-25）

## 本次范围与状态

基线：`354893bb861036d4061c7e648da158709b256cf1`（pacmen666/P2P-project）。
本补丁修改构建、传输客户端接收口、传输测试及正式中继收发实现，不改 v1 字段。
正式中继只有 `relay_server`，客户端为 `client_transfer`。旧 `relay_forward.c` 暂存为历史源码，不再提供 `relay_transfer` 构建目标，不需运行旧二进制。
本次修改尚未自动提交或推送；补丁应用后才在使用者本地生效。

## 已复现并修复

1. **UDP once 卡住**：Makefile 丢失 `-DFA_TRANSFER_CLIENT`，同机 Host/Bob 都绑定 UDP 37404；Linux SO_REUSEPORT 将单播交给其中一个 socket，不能代表两个独立玩家。复现日志为 `timeout: Bob files`、`WAITING_FOR_AVAILABLE_SOURCE`，无 FAULT_DROP。恢复该宏后每个玩家使用临时端口；PROBE/PUNCH/PIECE_* 仍共用同一个 fd。测试增加端口不重复、UDP_READY、实际 FAULT_DROP 的断言，不能通过跳过 UDP 消除报错。
2. **测试依赖缺失**：test-transfer 现在自动构建 room_server、client_transfer、relay_server、udp_fault.so 和流测试；恢复 `ALL STREAM TESTS PASS`。Linux 使用 LD_PRELOAD，macOS 使用已有 DYLD interpose 实现。
3. **macOS 编译回归**：编译输入过滤头文件；Darwin 自动使用 Homebrew openssl@3 的 include/lib 路径。Makefile 修改会触发传输客户端重编译。
4. **正式中继 BIND 粘包超时**：原实现移交 WRAP 后先调用 recv，再解析已有数据，阻塞等新数据；本次在 relay_server 接入已有 fa_stream 增量解析器和有界非阻塞发送队列。一次 recv 中的 BIND/WRAP 顺序处理，无第二次阻塞 recv，慢接收方不再阻塞全服务。
5. **保留刘的通告接收**：临时传输端口恢复后，另建只接收 ANNOUNCE 的 37404 socket，仍由同一网络线程 select/分发。没有新建第二个打洞 socket；探测、打洞、子包共用原 punch.fd。固定通告口失败会打印明确警告。

## 与王、刘、陈的边界

- **王**：正式中继入口、名单与 room=1 规则仍归王。此次 relay_server 收发结构复用了彭旧适配层及公共 fa_stream，是为通过联调提供的合并补丁，需要王审阅，不应记为王原提交已包含的修复。保留错误来源拒绝、旧身份重绑定、目标缺席报错；未绑定超时 5 秒、致命错误发送后关闭。
- 中继 `evidence_queued` 仅计格式匹配的 PIECE_DATA 中数据区、在成功入队时累计，可能含重传；不是收到确认的字节数，不是唯一有效下载量。它替代原先把所有内层正文都算作证物字节的累计方式。实验 8/9 仍由陈确认统计口径；客户端 TRANSFER_STATS 本次未改。
- **刘**：保留已有 ANNOUNCE 解析、清单请求、next/answer/choose 和选项记录。收发线程唯一；新加固定通告 socket 的方式需刘在真实局域网复核。单机测试不证明广播可跨子网，也不证明未 join 的 peer 0 已能下载完整文件。
- **陈**：不修改案卷生成、保存校验和统计实现。自动测试使用 tools/gen_case.py 的测试文件；正式演示请另加载陈的新案卷，核对图片、文字和清单。
- **彭**：网络收发、位图、来源切换、UDP 重传、中继接入与本文件列出的回归测试。

## Mac：一台电脑就可以验收你的网络模块

首先应用下载的补丁（包内 APPLY_FIX.sh 先执行 git apply --check，不覆盖有冲突的修改）。退出之前手工运行的 room_server/relay_server/client_transfer；不需要批量杀死所有同名进程。

```bash
cd ~/P2P-project-sync
# 首次需要：Xcode Command Line Tools，以及 Homebrew。
# 如果已安装，不必重复运行安装步骤。
xcode-select -p
brew install openssl@3
python3 --version
make -B test-transfer
```

若尚无 Xcode 命令行工具，先运行 `xcode-select --install` 并完成弹窗安装。
如果机器没有 Homebrew，先按 brew.sh 的安装说明安装后再执行上述 brew 命令。
应同时看到 `ALL STREAM TESTS PASS` 和 `ALL TRANSFER INTEGRATION TESTS PASS`。
测试自动启动、停止自己创建的子进程，不要求手工启动服务器；会占用 TCP 37401/37405、UDP 37402/37404。
每次输出 `Evidence directory: ...`，其中有每个测试场景的进程日志及 environment.json（基线提交、本地修改清单、系统、Python、用例）。保留本次目录用于向组员反馈，不要把缓存和案卷提交进仓库。

### 分项定位

先 `make transfer tests/udp_fault.so tests/test_transfer_stream`，再按需要执行：

```bash
python3 -u tests/test_transfer.py --case tcp
python3 -u tests/test_transfer.py --case resume
python3 -u tests/test_transfer.py --case udp-once
python3 -u tests/test_transfer.py --case udp-all
python3 -u tests/test_transfer.py --case relay
python3 -u tests/test_transfer.py --case announce
```

- tcp：三进程真实收发，B 给 C 上传，Tracker 关闭、来源退出后仍完成；远端 HAVE 不会令本机假完成。
- resume：重启复用缓存、破坏一块后重新校验并补齐。
- udp-once：实际丢弃选定子包，NAK 补齐；不得走中继。
- udp-all：实际丢弃全部 UDP 数据，有限重试后转正式 relay_server；最终文件逐字节核对。
- relay：完整文件经中继传输；原始 socket 额外验证 16 KiB 数据报文、拆分 BIND、BIND+WRAP 粘包、多报文粘包、错房间、未登记身份、伪造来源、错误长度；慢接收方不阻塞其他目标。
- announce：定向本机 ANNOUNCE 到 37404，验证接收分发；这不是跨机器广播或晚进下载完整闭环验收。

出错时把最后的报错、Evidence directory 及相应 Host/Bob/relay 日志发给组员。不要只把 timeout 调大，也不要删除 FAULT_DROP 断言。

## Mac：三个终端手工演示

先 `make transfer`，关闭自动测试后再执行。以下用测试案卷验证传输；如已有陈生成的目录，用真实路径替换 `<案卷目录>`，不要把尖括号原样输入。

终端 1：
```bash
cd ~/P2P-project-sync
mkdir -p logs
./room_server
```

终端 2：
```bash
cd ~/P2P-project-sync
python3 tools/gen_case.py
# 从输出复制 session_xxxxxxxx，填到下一行
./client_transfer --host --session <案卷目录> --room 127.0.0.1 --nick Host
```

终端 3：
```bash
cd ~/P2P-project-sync
./client_transfer --room 127.0.0.1 --nick Bob --data-dir /tmp/fragment-bob --auto
```

看到 3 条 FILE_COMPLETE 后，在 Bob 窗输入 `stats`。Bob 必须使用独立缓存目录，不能指向房主文件夹。退出输入 `quit`。
需要手工强制中继时，另开终端运行 `./relay_server`，房主和 Bob 均追加 `--mode relay`，Bob 换一个空缓存目录以避免只读取已有文件。不要运行 relay_transfer。

## 能单独验证什么，不能证明什么

一台 Mac 可独立验证本次所有自动化项目：真实多进程/socket、分块内容、缓存恢复、丢包重传、中继回落。这足以向王、刘反馈传输接口是否通过。
仍需全组完成：三台实体机互传、实际 NAT 打洞、局域网通告送达、未 join 玩家发现后完整取块、剧情与答案判定、陈真实案卷的展示及统计实验。
当前 peer 0 虽可索取清单，调度仍在无身份时返回；不能把“收到通告/清单”写成“晚进玩家已经拿全证物”。
中继仍和 Tracker 共享 /tmp/room_peers.txt，按单房间 1 工作；不是多房间认证服务。

## 建议收尾顺序

1. 彭在 Mac 运行全套，保存自己的真实结果；本补丁的 Linux PASS 不能替代 Mac 结果。
2. 王审阅正式中继补丁、确认统计含义；刘确认独立通告口及晚进身份方案。
3. 全组采用同一提交和两个正式入口 client_transfer / relay_server；刘统一其它启动文档。
4. 接入陈的真实案卷，做三机演示；有失败按模块反馈，不再并行维护两套中继。
