> 2026-10-25 更新：当前构建、中继入口及 Mac 验证以 [transfer_closeout.md](transfer_closeout.md) 为准。下文早期 relay_transfer 命令保留为历史记录，不再用于当前联调。

# 在 Mac 上应用补丁并提交到自己的仓库

本补丁基于 main `857b468`，不会替你提交或推送，不会改写其他人的提交。
运行测试按项目要求使用 Linux/WSL；下面的 Git 步骤可以在 Mac 终端执行。

## 1. 下载

把 `P2P_Transfer_PengCheng.patch` 保存到 Mac 的“下载”文件夹，文件名保持不变。

## 2. 检查仓库

```bash
cd ~/P2P-project-sync
git status --short
git remote -v
```

`git status --short` 应没有输出；如有未提交工作，先妥善保存，不要覆盖。
确认 origin 指向你自己的 `pacmen666/P2P-project`。

## 3. 建功能分支并应用

```bash
git switch main
git pull --ff-only origin main
git switch -c pengcheng/transfer-integration
git apply --check ~/Downloads/P2P_Transfer_PengCheng.patch
git apply ~/Downloads/P2P_Transfer_PengCheng.patch
git diff --stat
git status --short
```

如果 `--check` 报错，停止后续步骤，保留错误信息进行对照；不要强制覆盖。
补丁基线之后若 main 发生变化，可能需要手动合并。

## 4. 审阅并提交

先阅读 `docs/transfer_peng.md` 和 `docs/transfer_validation.md`，有 Linux 环境时运行 `make test-transfer`。
当前测试记录来自辅助开发环境；只有自己运行成功后才能写“我本机测试通过”。

```bash
git add .gitignore Makefile docs/transfer_peng.md docs/transfer_validation.md docs/transfer_submit.md src/transfer/README.md src/transfer/client_main.c src/transfer/transfer.c src/transfer/transfer.h src/relay/relay_forward.c src/punch/udp_sock.c src/tracker/room_server.c src/ui/ui_term.c tests/test_transfer.py tests/test_transfer_stream.c tests/udp_fault.c
git diff --cached --stat
git commit -m "feat(transfer): add verified peer transfer and recovery integration"
git push -u origin pengcheng/transfer-integration
```

Git 提示身份未配置时，使用本人已有的 GitHub 身份按提示配置；不要冒用组员身份。

## 5. GitHub 发起 PR

打开你自己的仓库，选刚推送的分支，点击 Compare & pull request。
确认 base 仓库是 `pacmen666/P2P-project`、base 分支是 `main`，compare 是 `pengcheng/transfer-integration`。
PR 标题：`实现 P2P 分片传输、故障恢复与独立联调测试`。

PR 正文可写：

> 本次完成彭负责的数据面联调：非阻塞 TCP 分块互传、多源调度、位图/HAVE、块及整文件校验、Tracker 离线续传、缓存恢复、UDP 子包补传与中继回落。复用已有 common、generate、ui 和 punch 模块，新增独立 client_transfer 入口，保留原 client 行为。
>
> 同时修复影响联调的 Tracker 接收缓冲区和 peer_id 复用问题，提供可替换的 relay_transfer 适配层，原中继骨架保留。
>
> 附本机多进程测试记录及可复现 make test-transfer。真实跨 NAT、广播新玩家加入、正式图像剧情和完整游戏答案判定尚待全组联调；不将本次测试当作这些功能的验收。
>
> 代码基于现有小组工程、借助 AI 辅助实现；具体复用与跨模块变更见 docs/transfer_peng.md。

先在自己的仓库保留可审阅成果。向组长原仓库提交时应再创建以小组 main 为 base 的 PR，并确认小组仓库最新变更已合并；不要直接覆盖组长 main。
