# 碎片档案 · 案卷生成模块（Python 原型 / 无 AI 原版）

> 负责人：陈（案卷生成）。本目录是「碎片档案」C/S 课程项目的 Python 原型，
> 用来在写 C 之前验证**协议、分块校验、清单（MANIFEST）生成、网络用量统计**这套思路。
> 已退回无 AI 原版：案件由随机种子当场生成，剧情/凶手只留服务端内存，绝不外发。

---

## 1. 这一版更新了什么

- **新增一键基准脚本 `run_bench.py`**：进程内起服务端线程 + 客户端，
  **只传文件、不答题**，逐字节计量真实网络用量，跑多轮后出汇总表 + 统计图。
- **`case_server.py` 清理**：去掉 AI 时期残留的 `--live/--offline/--no-ai` 参数
  （`gen_case` 已是无 AI 单参数版），原版交互 demo 现在能直接跑。
- **删除无用文件**：`doc_forge.py`、`stories_pool.json`（AI 残留，无引用）、
  以及 6 个运行日志 `*.err` / `*.log`。

---

## 2. 目录文件

| 文件 | 作用 |
|------|------|
| `case_common.py` | 帧格式（8 字节头）、SHA-256 分块、MANIFEST 编解码、案件生成 `gen_case(seed)` |
| `case_server.py` | 服务端（房主）：发清单、按块发证物、三项择一裁决 |
| `case_client.py` | 客户端（侦探）：要清单 → 逐块下载校验 → 章间问答 → 三项择一 |
| `seed_image.py` | 由种子确定性生成像素图（HMAC-SHA256 作为 PRNG） |
| `compose_evidence.py` | 用 `parts_export/` 素材拼现场图、证物卡 |
| `doc_image.py` | 素材零件库（家具/人物/房间 + 做旧滤镜），被 `compose_evidence` 引用 |
| `prepare_parts.py` | 构建工具：生成 `parts_export/` 里的零件图（非运行时依赖） |
| `parts_export/` | 8 张零件图（需提交，运行时加载） |
| `run_bench.py` | **新增** 一键基准：只传文件、统计网络用量、出图 |
| `demo_out/` | 运行产物（下载的证物 + 基准 CSV/图），已被 `.gitignore` 忽略 |

---

## 3. 怎么跑

### 3.1 交互 demo（手动答题）
```powershell
# 窗口 1：起房主
python case_server.py
# 窗口 2：当侦探（会问章间问答、三项择一）
python case_client.py
```
复现固定局：`python case_server.py --seed <hex(>=16字节)>`。看每帧字段结构加 `--trace`。

### 3.2 一键基准（不答题，只传文件 + 出统计图）
```powershell
python run_bench.py            # 默认 5 轮，出 demo_out/bench_report.png
python run_bench.py -n 10     # 跑 10 轮
python run_bench.py --seed <hex>   # 固定种子只跑 1 轮（复现）
python run_bench.py --no-chart # 只出表不出图
```
产物：
- `demo_out/bench_stats.csv` —— 每轮明细（应用层/线上字节、开销、帧数、块数、各文件体积…）
- `demo_out/bench_report.png` —— 2×2 统计图

---

## 4. 网络用量统计口径（对应实验 8）

基准脚本用「计量套接字」逐字节累加真实收发量，指标：

- **应用层证物字节**：三份证物（两图一文）真实内容大小 —— 即 `cs_host_up` 的业务量。
- **线上接收字节**：客户端真实网络用量 = 8字节帧头 × 帧数 + 5字节块头 × 块数 + 清单元数据 + 文件数据。
- **协议开销** = 线上 − 应用层，再拆成三项：
  - 8 字节帧头（仿约定 v1 第 3 节：magic/ver/cmd/req_id/body_len）
  - 5 字节块头（file_id / piece_index / data_len）
  - 清单元数据（局号、seed_hash、各文件整文件 SHA-256、各块 SHA-256）
- 还有：逐帧/逐块计数、单轮传输耗时、线上吞吐、应用层吞吐。

> 实测一轮：应用层 ≈ 168 KB，线上 ≈ 168.7 KB，开销 ≈ 0.7 KB（帧头/块头/清单占比很小，
> 说明 16 KiB 分块 + 摘要校验的协议开销可忽略）。

---

## 5. 协议要点（陈负责的部分）

- **分块**：16384 字节逻辑块；单文件 ≤ 32 块（≤ 512 KiB），超限在生成时直接断言。
- **摘要**：每块 SHA-256（32 字节），整文件再算一次；客户端逐块校验，不符就拒绝重传
  （同一块最多补缺 `MAX_RETRY=5` 次）。
- **清单 MANIFEST（约定 5.4 布局）**：`session_id(4) + seed_hash(32) + file_n(1)`，
  之后每项 `file_id/chapter/kind/file_size/整文件SHA/piece_count/各块SHA`。
  **种子只留服务端进程内存，对外只给 `SHA-256(种子)`**，不是种子本身。
- **三份证物**：file_1 现场勘查图（章一）、file_2 证物卡（章一，备注即线索）、
  file_3 案卷正文（章二，真凶藏在 `[判据]` 哨兵行之后，界面不展示，本机判定）。

---

## 6. 红线

- 种子、真凶姓名**绝不**出现在清单或任何下发字节里；对外只有摘要。
- 本原型不接 AI（已退回原版）；如以后再接，剧情可上云，但种子/凶手名不得外发。
