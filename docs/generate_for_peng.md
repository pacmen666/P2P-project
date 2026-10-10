# 案卷生成模块对接说明（给彭 / transfer）

**作者**：陈（生成与统计）　**日期**：2026-10-11
**对应**：v1 格式约定 §5.4 `MANIFEST`、§4 分块规则；`src/generate/`

## 背景

采用方案 A：host 端用 Python 生成器把一局落盘到 `session_<id>/`，C 侧**只装载**
（`src/generate/generate.c`），不生成、不渲染图像。这样你可以在**不等陈把生成写成 C**
的情况下，直接拿真实案卷字节开发/测试 transfer。

本局三份证物（file_id）：`file_1` 现场勘查图（图）、`file_2` 证物登记卡（图）、
`file_3` 案卷正文（文本）。单文件 ≤ 512 KiB，逻辑分块 16384 B。

## 快速开始（三步）

```bash
# 1) 生成一局（无 Pillow 依赖；内容是用种子派生的确定性字节，网络实验只关心字节与哈希）
python3 tools/gen_case.py [seed_hex] [base_dir]
#    -> base_dir/session_<id>/file_1 file_2 file_3 manifest.bin

# 2) C 侧自检：解析 manifest 并读块
gcc -o test_generate src/generate/generate.c src/generate/test_generate.c
./test_generate session_<id>

# 3) 哈希一致性校验（manifest 每块摘要 == 文件实切块哈希，transfer 校验依赖此）
python3 tests/check_manifest.py session_<id>     # 期望 HASH-CONSISTENCY: PASS
```

> `session_*/` 被根 `.gitignore` 忽略，不入库——你本地各自生成即可。

## C 接口（transfer 调用，见 `src/generate/generate.h`）

```c
int  gen_load_manifest(const char *session_dir, struct gen_manifest *m);
void gen_free_manifest(struct gen_manifest *m);
int  gen_read_piece(const struct gen_manifest *m, uint8_t file_id,
                    uint16_t piece_idx, uint8_t *out, size_t *out_len);
```

- `gen_load_manifest`：解析 `session_dir/manifest.bin`，填 `struct gen_manifest`
  （`session_id`、`seed_hash`、`file_n`，每个文件含 `file_size`、`file_hash`、
  `piece_count`、`pieces[].hash`、磁盘 `path`）。
- `gen_read_piece`：取第 `file_id` 个文件第 `piece_idx` 块的字节写入 `out`
  （`out` 至少 16384 B），`*out_len` 返回实际长度——**末块更短**，计算：
  `min(16384, file_size - piece_idx*16384)`。失败返回 -1（文件/块号越界）。
- 收方拿到块后，用 `m.files[k].pieces[piece_idx].hash` 比对 SHA-256 即可，无需自己算布局。

## 数据契约速查（v1 §5.4）

`manifest.bin` = 线上 `MANIFEST` 正文（大端）：

| 偏移 | 长度 | 字段 |
|------|------|------|
| 0 | 4 | `session_id` |
| 4 | 32 | `seed_hash`（不是种子） |
| 36 | 1 | `file_n`（本项目 3） |
| 37 起 | 每项 41 + `piece_count*32` | 文件项 |
| 文件项内 | | `file_id`(1) `chapter`(1) `kind`(1 图/2 文本) `file_size`(4) `file_sha256`(32) `piece_count`(2) `piece_hash × 32`（块号升序） |

落盘：`session_<id>/file_<file_id>`。块号从 0：`piece_count = ceil(file_size/16384)`。

## 开发注意

- **校验**：收块按 `pieces[piece_idx].hash` 校验；整文件可按 `file_hash` 最终复核。
- **末块**：最后一块不足 16384 B，发包/写盘按实际长度，别硬编码 16384。
- **BITFIELD / HAVE**：`piece_count` 须与清单一致（见 §5.5 / §5.6）。
- **TCP / UDP**：TCP 回整块；UDP 子包 1024 B、末子包可短（§5.7 / §5.8）。
- **实验 8 中继计数**：中继只计内层 `PIECE_DATA` 的数据区，不含 `RELAY_WRAP` 头（§5.12）。
- **红线**：种子绝不进 `MANIFEST`/日志/文件名；真凶只在 `file_3` 正文里。

## 联调完成判据

1. `gen_load_manifest` + `gen_read_piece` 能稳定取出所有 file 的所有 piece；
2. 任一 peer 用 `pieces[].hash` 校验全部通过（`check_manifest.py` 在 host 端 PASS）；
3. 两 client 经 room_server 名单互传，能把三文件完整拼回且哈希一致。

> 后端可替换：将来陈把生成做成进程内 C（`gen_generate_session(seed)`），transfer 调用方不变。
