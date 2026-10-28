# Transfer 验证记录

日期：2026-10-12。基线：857b4681085a2b495ff6835f2f118c618296d0bd。
环境：Linux，GCC 13.3，Python 3，OpenSSL libcrypto。本机多进程及真实 socket。

执行 `make all`：原入口构建通过。
执行新源文件的 `-Wall -Wextra -Werror -fsyntax-only`：通过。
执行 `make test-transfer`：退出状态 0，输出如下。

```text
PASS network byte order: 46 41 01 0b 12 34 00 13
PASS all 28 split positions and byte-by-byte input
PASS two frames supplied in one read
PASS empty-body frame
PASS truncated header/body rejected at EOF
PASS bad magic, version and command rejected; reader stays failed
PASS maximum body length and output bounds
PASS short writes, EAGAIN, bounded queue, exact-byte delivery
PASS pipe wake, drain and full-pipe coalescing
ALL STREAM TESTS PASS
PASS manifest: truncation, trailing bytes, file count/id/chapter, oversize rejected
PASS TCP: verified files, B->C upload, multisource, Tracker loss, source loss, no false HAVE
PASS restart: reused persisted cache, rejected damaged cached block, same nickname rejoin
PASS UDP selective loss: NAK missing subpacket recovery and verified short final blocks
PASS UDP total loss: bounded retries -> actual TCP relay -> verified files
PASS relay: real forwarding, fragmented BIND+coalesced WRAP, spoofed src rejected
ALL TRANSFER INTEGRATION TESTS PASS
```

原始运行日志所在临时目录：`/tmp/fragment-transfer-tests-ejcwl3f_`。
临时目录不随补丁分发；运行测试可重新生成逐进程日志，结束时会输出目录。
没有采集 pcap，没有宣称真实 NAT 穿透成功，没有进行三台实体设备联调或性能基准。

---

## 2026-10-24 复测（WSL2，`LAP-NICKY`）

环境：WSL Ubuntu，`/mnt/d/Coding_Project/CS374/Project`，`make && make transfer` 后执行 `make test-transfer`。

| 阶段 | 结果 |
|------|------|
| manifest 校验 | PASS |
| TCP（多源、Tracker 丢失、源丢失、续传） | PASS |
| restart（坏块拒绝、同名重进） | PASS |
| UDP `once`（`LD_PRELOAD` + `udp_fault.so`） | **FAIL**：`AssertionError: timeout: Bob files` |
| UDP `all` / relay | 未执行（因 `udp('once')` 中断） |

证据目录示例：`/tmp/fragment-transfer-tests-a31b2n26/udp_once/`。`Bob.log` 反复 `WAITING_FOR_AVAILABLE_SOURCE`，缓存仅 `manifest.bin`；`Host.log` 未见 `FAULT_DROP`（与 2026-10-12 全绿 Linux 环境不一致，待彭侧在 WSL/实体 Linux 对照）。

**手工联调（同日）**：`session_1e22ca8e` 下 `client_transfer` 房主 `--session session_1e22ca8e` + 侦探 `get`，路径 **本网直连**，`.cache/detective1/session_1e22ca8e/file_{1,2,3}` 与清单大小一致。

刘侧 `./client` 冒烟三连、`choice_summary.log`、在线 `announce`、关房四窗实验 7 见 `docs/week1_liu.md`、`docs/experiment7_announce.md`。

## 2026-10-25 收尾补丁验证（彭侧，本次独立 Linux 环境）

基线 `354893bb861036d4061c7e648da158709b256cf1` 加本地补丁，未自动提交/推送。
环境 Linux x86_64，GCC 13.3.0，Python 3.12.14；不是用户 Mac/刘的 WSL 实机结果。

复现：补齐构建依赖但未恢复 FA_TRANSFER_CLIENT 时，TCP/restart PASS，UDP once 在 Bob files 超时；Host/Bob 均绑定 37404，无 FAULT_DROP。日志目录 `/tmp/fragment-transfer-tests-h313t0qk`。
恢复独立 UDP 端口后 once PASS；正式 relay_server 原版的 BIND+WRAP 来源伪造测试在等待 ERROR 时超时，正式中继收发补丁解决该问题。

最终 `make test-transfer` 退出码 0，`make all` 通过；`git diff --check`、Python 编译检查及 relay_server `-Wall -Wextra -Werror -fsyntax-only` 通过。

```text
ALL STREAM TESTS PASS
PASS manifest: truncation, trailing bytes, file count/id/chapter, oversize rejected
PASS TCP: verified files, B->C upload, multisource, Tracker loss, source loss, no false HAVE
PASS restart: reused persisted cache, rejected damaged cached block, same nickname rejoin
PASS UDP selective loss: NAK missing subpacket recovery and verified short final blocks
PASS UDP total loss: bounded retries -> actual TCP relay -> verified files
PASS relay: 16KiB, split/coalesced frames, room/membership/source/length checks, slow-reader isolation
PASS ANNOUNCE listener: port 37404 retained alongside independent transfer UDP socket
ALL TRANSFER INTEGRATION TESTS PASS
```

最终证据目录 `/tmp/fragment-transfer-tests-su32m5l2`，仅为本次环境路径，不是用户电脑上的路径。补丁包附最终控制台输出；本地运行会生成新的证据目录。
Mac 尚需使用者按 [transfer_closeout.md](transfer_closeout.md) 复测；不宣称真实 NAT、多机或完整游戏已验收。

## 2026-10-25 Mac M2 最终传输回归

验证人：彭成。
环境：MacBook Pro M2，本机多进程；OpenSSL 使用 Homebrew openssl@3。
命令：make -B test-transfer。

结果：ALL STREAM TESTS PASS；ALL TRANSFER INTEGRATION TESTS PASS。

通过项目：清单校验、TCP 文件传输与多源上传、Tracker/source loss、
重启续传及损坏缓存拒绝、UDP 部分丢包补缺、UDP 全丢后有限重试回落正式
relay_server、16 KiB 中继转发、拆包粘包、房间及身份和长度检查、
慢接收者隔离、ANNOUNCE 独立监听专项测试。

补充修复：Tracker JOIN 后临时连接长度清零导致的越界搬移；
保留 JOIN 后粘连数据，并避免连接移交后重复使用旧的读就绪状态。

证据目录：fragment-transfer-tests-pce1mdl5。
完整本机路径和终端输出另存 Mac 验证 Word；原始目录另行归档。

范围：本次为单机多进程回归，不代表三台真实设备联机、跨网 NAT 打洞、
关房后新玩家完整下载或完整解谜流程已经通过。

## 2026-10-28 复测（WSL2，`LAP-NICKY`）

基线 `9017eb7`。环境：WSL Ubuntu，`/mnt/d/Coding_Project/CS374/Project`。  
命令：`make clean && make && make transfer`，然后三连冒烟 + `make test-transfer`。

刘侧三连：`ACK_OK`、`liu_week2_smoke: OK`、`logs/client.log` 含 PROBE/PROBE_ACK。  
`make test-transfer` 退出码 0：

```text
ALL STREAM TESTS PASS
PASS manifest: truncation, trailing bytes, file count/id/chapter, oversize rejected
PASS TCP: verified files, B->C upload, multisource, Tracker loss, source loss, no false HAVE
PASS restart: reused persisted cache, rejected damaged cached block, same nickname rejoin
PASS UDP selective loss: NAK missing subpacket recovery and verified short final blocks
PASS UDP total loss: bounded retries -> actual TCP relay -> verified files
PASS relay: 16KiB, split/coalesced frames, room/membership/source/length checks, slow-reader isolation
PASS ANNOUNCE listener: port 37404 retained alongside independent transfer UDP socket
ALL TRANSFER INTEGRATION TESTS PASS
```

证据目录：`/tmp/fragment-transfer-tests-0x0se_xp`。  
对照 2026-10-24 同机 UDP `once` 超时：本次已过。仍不宣称真实 NAT 或多机联调完成。
