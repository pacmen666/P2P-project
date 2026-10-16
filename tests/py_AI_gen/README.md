# py_AIgen — 案卷生成器（变体入口）

本目录是 `Fragment_Archive` 案卷生成的一种**变体**入口：`ai_gen_case.py` 由种子生成一局，
`file_1` / `file_2` 由云端图像接口渲染、`file_3` 沿用程序化解谜文本。
输出布局与主生成器完全一致（`session_<id>/` + `manifest.bin`，清单按 v1 §5.4 含每块 SHA-256），
可直接作为主流程 `--session` 的输入。

## 依赖
本目录自带：`case_common.py` / `seed_image.py` / `compose_evidence.py` / `doc_image.py` / `prepare_parts.py`。
运行时还需 `Pillow`：`pip install pillow`。
素材零件复用仓库内已提交的 `tests/py_test/parts_export/`（无文字通用素材，供拼接；缺失时自动退回程序化底图）。本目录不重复存放，避免与 `py_test` 冗余。

## 运行
```bash
python ai_gen_case.py <seed_hex> [--dotenv PATH] [--preview]
# 例：
python ai_gen_case.py 00112233445566778899aabbccddeeff --preview
```
- 未传 seed 则随机。
- `--preview` 同时导出 `file_1.jpg` / `file_2.jpg` 便于肉眼查看。
- 未配置密钥时自动降级为程序化图（见 `case_common.gen_case`），保证可跑。

## 密钥
云端图像接口的 Key 放在 `.env`（`ARK_API_KEY` / `ARK_MODEL`），**已被本目录 `.gitignore` 排除，切勿入库**。
可用 `--dotenv /path/to/.env` 指向仓库外的密钥文件。

## 与主流程的对接
生成的 `session_<id>/` 交给主项目房主侧：
```bash
./client_transfer --host --session session_<id> --room localhost --nick host
```
清单格式、块哈希、种子红线（种子只留内存、对外仅 SHA-256 摘要）均与主生成器相同。
