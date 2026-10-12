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
