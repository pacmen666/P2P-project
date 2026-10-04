# tests/py_test — 案卷生成与 C/S 传输的 Python 原型

**实验 0（本局生成、不进入安装包）的验证脚本。**

这批代码是**原型，不是交付代码**。它的作用是先在本机把"种子 → 案卷 → 分块 → 摘要 → 清单 → 传输 → 校验"这条链路跑通，
再把结论迁到 C 版 `src/generate/`。协议字段一律以 `docs/碎片档案_v1格式约定.md` 为准，本目录不定义协议。

---

## 1. 文件清单

| 文件 | 作用 |
|------|------|
| `seed_image.py` | 底层。`PRNG`（HMAC-SHA256 计数器模式，同种子必同结果）、程序化出图、纯标准库写 PNG |
| `doc_image.py` | 画图库：做旧纸张、噪点茶渍、红色印章、中文字体、房间/家具/警员词表 |
| `compose_evidence.py` | 零件 + 种子 → 合成两张证物图（现场勘查图、证物登记卡） |
| `case_common.py` | 协议层：8 字节帧头、`MANIFEST` 编解码（约定 5.4）、`gen_case()` 生成一局 |
| `case_server.py` | 房主端（C/S 的 S）：发清单、发块、裁决、统计上行字节 |
| `case_client.py` | 侦探端（C/S 的 C）：索清单、逐块下载校验、章间问答、三项择一 |

依赖关系：`case_client/server` → `case_common` → `compose_evidence` → `doc_image` → `seed_image`。

## 2. 素材

`parts_export/` 下 8 张**无文字通用零件**（纸张、房间线稿、指纹、证物卡/袋），由种子挑选后拼接。
按 `AGENT.md:21`，`assets/` 只允许放零件、禁止放本局成品；这些正是零件（成品是运行时生成的，不进仓库）。

命名只要含关键词即被识别（中英文均可）：`paper/纸张`、`plan/平面/线稿`、`finger/指纹`、`card/证物卡/证物袋`、`stain/污渍`。
查找顺序见 `compose_evidence.py` 的 `PARTS_DIRS`，以脚本所在目录优先，**不依赖仓库外的文件**。

## 3. 运行环境

- Python 3.9+，需要 `Pillow`（`pip install pillow`）
- 中文字体：`C:/Windows/Fonts/msyh.ttc`（Windows 自带；Linux 需另指定字体路径）
- 仅用 `127.0.0.1`，端口 **38001**（故意避开约定的 37401/37402/37404/37405）
- 中文乱码时先 `set PYTHONIOENCODING=utf-8`

## 4. 怎么跑

```powershell
# 窗口 1：房主
python tests/py_test/case_server.py

# 窗口 2：侦探
python tests/py_test/case_client.py
```

可选参数：

| 参数 | 作用 |
|------|------|
| `--seed <hex>` | 固定种子复现同一局（仅实验用；真实开局用随机种子） |
| `--trace` | 打印每帧的字段结构（头 + 正文逐字段展开） |
| `--corrupt` | 注入一个坏块，演示"摘要不符 → 拒绝 → 重传" |

单独验证生成器（不出网络）：

```powershell
python -c "import sys; sys.path.insert(0,'tests/py_test'); from case_common import gen_case; import os; c=gen_case(os.urandom(32)); print(len(c['manifest']))"
```

## 5. 实验 0 记录

**目的**：证明案卷是开局当场生成的，不是安装包里的固定文件；仅持摘要无法在本地重放。

**命令**（工作目录：仓库根）：

```
python -c "... gen_case(bytes.fromhex('00'*32)) ..."   # A 局
python -c "... gen_case(bytes.fromhex('aa'*32)) ..."   # B 局
```

**操作系统与网络**：Windows 11 / Python 3.12 / Pillow 12.3；无网络（纯本地生成）。

**预期**：两局的 `seed_hash`、三个文件的整文件 SHA-256 与分块 SHA-256 **全部不同**；单文件 ≤ 512 KiB；块大小 16384。

**实际**（已通过）：

| | A 局（seed 00×32） | B 局（seed aa×32） |
|---|---|---|
| `seed_hash` | `66687aadf862bd77…` | `e0e77a507412b120…` |
| `file_1` | 106573 字节 / 7 块 / `1eab57c924e7f41a…` | 108830 字节 / 7 块 / `7ff78743278a51f4…` |
| `file_2` | 66121 字节 / 5 块 / `b2d687a25adfe0fc…` | 45006 字节 / 3 块 / `a3f1be445801bca9…` |
| `file_3` | 567 字节 / 1 块 / `fa26df2b85fd0442…` | 573 字节 / 1 块 / `c6913440e24ae92c…` |
| `MANIFEST` | 576 字节 | 512 字节 |

**"仅持摘要无法重放"的论证**：`MANIFEST` 只含 `session_id`、`seed_hash` 与各级 SHA-256，
**不含种子、不含生成器内部状态、不含明文答案**。生成案卷必须知道种子，而种子 ≥16 字节、只留房主进程内存。
因此只拿到清单的一方无法在本地产出摘要相符的文件，只能经网络取得字节。

**答案的位置**（章间问答与三项择一的判据）：

- 章间问答答案 = `file_1` 的 **JPEG 注释段**（同时印在图的抬头，人也能读）；
- 三项择一判据 = `file_3` 中哨兵行 `[判据]` **之后**的内容，界面只展示哨兵之前的部分；
- 玩家应当靠**推理**得出：证物卡的物证否掉哪套不在场证明，说那套话的人就是真凶（`case_common.py` 的 `ALIBIS` / `CONTRA` 两张表是陈与刘约定的接口）。

## 6. 与 C 版的关系（迁移说明）

| 原型 | C 版落点 |
|---|---|
| `build_manifest()` / `parse_manifest()` | `src/generate/` 的 `fa_manifest_encode/decode`，逐字段 pack，禁 `memcpy` 结构体 |
| `PRNG` | `fa_prng_*`，Linux 用 `getrandom()`，种子 ≥16 字节只留内存 |
| 分块与 SHA-256 | 16384 字节一块，整文件 + 每块各一个 SHA-256 |
| 服务器统计 | `stats.h` 四计数器（`cs_host_up` / `p2p_host_up` / `p2p_other_up` / `relay_up`） |
| 素材拼接 | 零件进 `assets/`（用 PNG，**不要用 `.bin`**，被 `.gitignore` 忽略）；C 侧需 PNG 解码（如 `stb_image.h`） |

**原型与真实协议的差异**（迁移时必须改）：

1. `magic` 用 `b"FA"` 只是示意，真值以约定第 1 节 / `include/common.h` 为准；
2. `PIECE_DATA` 省掉了 `sub_index` / `sub_count`（约定 5.8），因为原型走 TCP 发整块；
3. `ANSWER`(`0xA0`) / `RESULT`(`0xA1`) 是原型自造指令，真实项目三项择一由本机判定，不需要这两条；
4. 原型是纯 C/S（中心分发，对应 `cs_host_up`），没有 P2P 互传、打洞、中继、通告。

## 7. 注意事项

- 运行时产物 `demo_out/`、日志 `*.log` 均**不提交**；
- 不要提交本局成品（`session_*/`、`*.bin` 已被 `.gitignore` 忽略，也不要 `-f` 强加）；
- 换端口或改协议字段前先看 `docs/碎片档案_v1格式约定.md`，改字段需四人确认并升 `ver`。
