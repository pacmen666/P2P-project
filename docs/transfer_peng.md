# 彭：P2P 分片传输交付与联调指南

基线：`pacmen666/P2P-project` main `857b468`。本次为基于现有代码的辅助开发成果，提交前请本人阅读、运行并能解释实现；不能将联调通过表述为全组游戏或真实 NAT 实验已完成。

## 1. 分工与价值

项目方案第 6、8 节将彭的责任定为：报文组装与读状态机、玩家分块互传、多路 select、位图与 HAVE、多源请求、Tracker 离线后继续传送、来源切换、UDP 子包与重试，以及实验 1、3、6 的材料。

这些是数据面的核心能力。界面负责表达“缺什么”，transfer 负责真实取得、校验、保存并再次上传这些字节。工作量和技术价值都较高；成绩仍取决于课程评分、个人理解、实验材料和答辩，不能由代码量保证。

## 2. 本次交付

| 文件 | 内容 |
| --- | --- |
| `src/transfer/transfer.h` / `transfer.c` | 单网络线程的非阻塞 peer TCP、分片调度、BITFIELD/HAVE、SHA-256 校验、磁盘恢复、UDP 子包与 NAK、中继路径 |
| `src/transfer/client_main.c` | 独立联调入口，复用刘的 UI/打洞函数、陈的 manifest 装载器；主线程命令队列 + 唤醒管道 |
| `src/relay/relay_forward.c` | 可运行的中继适配层，补齐现有骨架无法实际转发的问题；保留王的原文件 |
| `tests/test_transfer.py` | 真实进程、真实 socket 的本机端到端及故障测试 |
| `tests/test_transfer_stream.c` | 复用第一周测试，验证规范路径中的公共读写状态机 |
| `tests/udp_fault.c` | 仅测试用 LD_PRELOAD 丢包注入器，不进入成品客户端 |

**保留原来的 `make all`、`client`、`relay_server` 入口。** 新功能运行 `make transfer`，生成 `client_transfer` 与 `relay_transfer`，便于组员分别审阅、逐步合并。

跨模块改动限定为联调需要：

- Tracker：修复临时 JOIN 连接转正时接收缓冲区重置、粘包尾部搬移及同一轮 select 重复读取；增加入房字段检查、重复昵称检查和 peer_id 复用。
- punch：仅新入口编译时使用临时 UDP 端口，避免同机多个 peer 共享 37404 导致单播被错误分配；PROBE、PUNCH、数据始终共用这个 fd。原入口行为保留。
- ui_term：仅新入口显示 `get / pause / stats`；原入口仍显示原命令。
- 不修改 v1 字段、指令码、协议文档、LICENSE，不宣称组员模块为彭独立完成。

## 3. 已实现的具体行为

1. 只有小 peer_id 发起 TCP 连接；1.5 秒连接超时，增量解析报文，非阻塞发送队列，心跳和请求超时。
2. 建连与周期交换位图；下载并校验的块才进入本机 HAVE，可立即上传给第三人。远端 HAVE 只代表远端拥有，不会点亮本机证物。
3. 从多个可用来源安排不同块，每个来源最多一个在途块。某来源掉线、其他来源有副本时继续下载；没有副本时等待，不虚构成功。
4. 每块校验和整文件校验；按 manifest 保存 `session_<id>/file_<id>`。重启逐块校验缓存，损坏块重新下载。
5. UDP 每个数据子包最多 1024 字节，应用层包小于 1200 字节。全局每次 tick 最多发送一个子包，至少间隔约 1 ms。
6. 每 500 ms 检查缺失子包，用 NAK 请求缺失项；超时计数达到 5 切换中继（前四次发送 NAK，第五次回落）。中继使用相同子包格式封装在 TCP RELAY_WRAP 中。
7. 所有 peer 尝试建立备用 RELAY_BIND，包括只上传的房主；平时直连文件字节不经中继。否则仅接收端回落时，上传端尚未绑定，会无法转发。
8. Tracker 断开后保留已知 peer 地址和位图继续传送。来源 id 被重新分配、端口变化时清理旧连接与旧位图。

## 4. 构建与快速测试

运行环境：Linux / Ubuntu / WSL2，GCC、Python 3、OpenSSL 开发库。Mac 上可以提交源码，但这里未验收原生 macOS 编译；故障注入测试依赖 Linux 的 LD_PRELOAD。

```bash
# Ubuntu / WSL2 缺依赖时执行
sudo apt-get update
sudo apt-get install -y build-essential python3 libssl-dev

# 仓库根目录
make transfer
make test-transfer
```

测试会使用 37401、37402、37405，请先关闭占用这些端口的旧进程。测试数据和日志放在最后打印的 `/tmp/fragment-transfer-tests-...` 下，不入 Git。测试会更新原 Tracker 使用的 `/tmp/room_peers.txt`，不要同时运行另一局。

测试内容：

- 分包/粘包、逐字节输入、半包 EOF、非法报头、最大长度、短写/EAGAIN、队列容量、唤醒管道。
- 非法 manifest：截断、多余字节、文件数/编号/章节错误、文件超限。
- A 给 B，B 给 C；C 同时从 A/B 取块，关闭 Tracker 并杀掉 B 后仍拼齐；逐字节比较三个文件。
- 下载一部分后重启；破坏一个缓存块，确认坏块重下且其他完整块复用。
- 每块第一次丢第 3 号子包：通过缺失项重传拼齐，不触发中继。
- 丢弃全部 UDP 数据：超时后真实通过中继完成并校验文件。
- 中继的拆分 BIND、粘连 WRAP、伪造 src_id 拒绝。

这属于本机多进程网络测试，**不等同于三台实体机或跨 NAT 实验**。

## 5. 手动三端演示

所有终端进入同一仓库根目录。生成一次测试案卷：

```bash
python3 tools/gen_case.py
```

记录输出的 `session_XXXXXXXX`，下面替换为真实目录名。这个生成器提供真实可传输字节和哈希，内容是测试字节，不是最终可展示的图片剧情。

终端 1：

```bash
./room_server
```

终端 2（验证回落时启动，可独立关闭；与 `relay_server` **不能同时占 37405**）：

```bash
./relay_server    # 正式答辩 / 实验 9 推荐
# 或（仅 make test-transfer 仍依赖时）
./relay_transfer
```

终端 3，A 房主：

```bash
./client_transfer --room 127.0.0.1 --nick Host --host --session session_XXXXXXXX
```

终端 4，B 先取得一份：

```bash
./client_transfer --room 127.0.0.1 --nick Bob --data-dir cache_b --auto
```

等待 B 输出三个 `FILE_COMPLETE`，再开终端 5，C：

```bash
./client_transfer --room 127.0.0.1 --nick Carol --data-dir cache_c --delay-ms 350
```

C 加入后输入 `get`。观察 `TRANSFER PIECE_OK 2 ...`，说明块来自 B。关闭终端 1 的 Tracker，再关闭 B，C 仍可从 A 补齐。`--delay-ms 350` 是为了看清演示的人为请求间隔，不可用于宣传性能。

`stats` 查看贡献数据，`pause` 暂停安排新请求（在途请求仍可能完成），`get` 继续，`quit` 正常退出。多台设备时把 `127.0.0.1` 替换为 Tracker 的 LAN IPv4，并让防火墙放行服务端口与客户端动态端口。

`--mode tcp` 为默认自动路径（TCP 优先，其次已确认 UDP，再中继）；`--mode udp` 用于强制本网 UDP 实验；`--mode relay` 用于中继联调。`--cs` 限定只向房主 id=1 索取，是分发对照开关，并非一个另行实现的完整 C/S 服务。

## 6. 计数与证据，避免误报

- `from_1 ... from_4`：本机从对应来源新收到并成功校验的证物字节，不重复计算已有块。
- `tcp_up / relay_up`：成功进入发送队列的证物负载字节，可能包含重传；不是远端确认收到的字节，也不是抓包线速流量。
- `udp_up`：sendto 返回成功的证物负载字节，仍不保证对方收到；丢包注入器会模拟发送成功。
- 都不含协议头，不能直接据此宣称节省多少真实服务器带宽。严谨的实验 8 需与陈统一计数口径，再对照抓包/接收端记录。
- `PIECE_OK` 第二个数字为块号，不含文件号；来源归属按 peer_id，整文件完成另有 `FILE_COMPLETE`。详细实验分析应结合前后请求及 manifest，不把不同文件同号块混为一谈。
- 正常直连时 Tracker 仅处理名单、摘要等控制信息；备用中继连接只绑定，并不代表文件都经过中继。回落期间中继确实承载文件负载，不能称作“文件始终不经过服务器”。

## 7. 仍需全组联调的边界

- 关房晚进通告：`client_transfer` 已可收 `CMD_ANNOUNCE` 并 `tr_connect_announced`（与 `./client` 同路径）；**不**通过 Tracker 分配 peer，晚进仍为 peer 0。本机四窗验收见 `docs/experiment7_announce.md`。已知 peer 继续传输已测试。
- 刘的真实跨网打洞：当前实现对来源地址、session 和 peer_id 做一致性检查，但这不是密码学认证；不同 NAT、校园网和异地网络尚未验收。
- 王的中继正式实现可替换此适配层。目前沿用单房间 room=1 和本机 `/tmp/room_peers.txt`，Tracker 与中继必须部署同一机器；无身份令牌，不能作为公网生产服务。
- 陈的正式图像/剧情生成与实验 8 统计接口还要接入；当前收块保存和统计在 transfer 内，可按本地 API 对接，不需要更改线上协议。
- 新入口的 `answer / choose` 保留流程占位，只推进阶段，没有正式答案判定；`get` 会取所有文件，未做第二章资料访问控制。不是完整游戏验收。
- 本次未采集 Wireshark pcap、未做吞吐量或难度系数保证。收到小文件也能证明协议正确，不能推导大规模性能。

## 8. 彭的答辩讲法与提交建议

可陈述：我负责把“向队友索取”变成真实网络传输。先交换位图找持有者，用 select 同时处理房间和多个 peer；每块通过 SHA-256 校验后保存并发布 HAVE，使 B 能再给 C。Tracker 掉线不影响已知 peer，UDP 缺子包时只补缺项，多次超时转中继。

现场重点解释：为什么 TCP 一次 recv 不等于一条报文；UI 怎样用队列和管道唤醒网络线程；收到远端 HAVE 为什么不能算本机完成；唯一副本丢失为什么不能恢复；中继是连通性兜底而非服务器零负载。

建议单独功能分支提交，通过 PR 让组员审查。PR 说明明确列出 transfer 主体、Tracker 必要修复、可替换中继适配层和实验边界。提交记录保留真实作者与协作过程，不需要改写其他人的历史。
