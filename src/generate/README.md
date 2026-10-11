# src/generate/

房主侧案卷生成、切块、SHA-256、本地保存与实验 8 计数。

## 范围

- 种子 ≥ 16 字节，只留内存，不写文件名、界面、上线字段
- 三文件、单文件 ≤ 512 KiB；`MANIFEST`（可含 `seed_hash`）
- 落盘 `session_<id>/`：该目录被根 `.gitignore` 忽略，**不要**强制 add

## 规则

| 类别 | 说明 |
|------|------|
| GitHub | 提交生成器源码与 README，不提交 `session_*/`、不提交成品图 |
| gitignore | `session_*/`、`*.bin` |
| 本地 | 运行后会在工作目录出现 `session_*`，仅本机保留 |
| 只读 | 「种子不得出现在对外字段」 |
| 可编辑 | 生成器与统计代码（陈主责）；零件从 `assets/` 读 |

## 对接 transfer（彭）

本目录给传送层提供「案卷来源」。方案 A（当前）：案卷由 Python 生成器落盘到
`session_<id>/`，C 侧只装载（generate.c），不碰图像。

### 产出（host 端先跑生成器）
- 轻量、无 Pillow 依赖：`python3 tools/gen_case.py [seed_hex] [base_dir]`
  → 产出 `base_dir/session_<id>/file_1`、`file_2`、`file_3`、`manifest.bin`。
- 或用陈的完整原型（含真图）：`test_code/case_common.py` 的
  `generate_session(seed, base_dir)`，内部调 `export_manifest` 写 `manifest.bin`。
- `manifest.bin` 字节布局严格对齐 v1 §5.4（= 线上 `MANIFEST` 正文）：
  `session_id(4) seed_hash(32) file_n(1)`，其后每项
  `file_id(1) chapter(1) kind(1) file_size(4) file_sha256(32) piece_count(2) piece_hash×32`。

### C 接口（transfer 调用，见 generate.h）
- `gen_load_manifest(session_dir, &m)`：解析 `manifest.bin` 填 `struct gen_manifest`。
- `gen_read_piece(&m, file_id, piece_idx, out, &len)`：取第 file_id 个文件第
  piece_idx 块的字节（≤16384 B，末块更短）。transfer 拿到后用
  `m.files[].pieces[idx].hash` 比对摘要即可。
- 后端可替换：将来要做进程内 C 生成，只需另实现 `gen_generate_session(seed)`，
  transfer 调用方不变。

### 联调最小步骤
1. `python3 tools/gen_case.py` → 得到 `session_xxxxxxxx/`。
2. `gcc -o test_generate src/generate/generate.c src/generate/test_generate.c && ./test_generate session_xxxxxxxx`
   确认 C 侧能解析 manifest 并读块。
3. transfer 用 `gen_load_manifest` + `gen_read_piece` 起 `PIECE_DATA`，按
   `pieces[].hash` 校验。
