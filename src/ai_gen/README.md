# ai_gen — AI 案卷生成器

本目录是 `Fragment_Archive` 的 AI 案卷生成模块：`ai_gen_case.py` 由种子生成一局，
`file_1` / `file_2` 由云端图像接口渲染、`file_3` 沿用程序化解谜文本（或由 LLM 生成剧本）。
输出布局与主生成器完全一致（`session_<id>/` + `manifest.bin`，清单按 v1 §5.4 含每块 SHA-256），
默认输出到仓库 `cases/`，可直接作为主流程 `--session` 的输入。

## 依赖
本目录自带：`case_common.py` / `seed_image.py` / `compose_evidence.py` / `doc_image.py` /
`prepare_parts.py` / `llm_case.py` / `gen_room.py` / `remove_bg.py`。
运行时还需 `Pillow`：`pip install pillow`。
证物素材使用正式资源目录 `assets/parts/`（无文字通用零件，已入库；缺失时自动退回程序化底图），
不依赖 `tests/` 下的任何测试目录。

## 运行
```bash
python src/ai_gen/ai_gen_case.py <seed_hex> [--dotenv PATH] [--preview] [--base DIR]
# 例（在仓库根）：
python3 src/ai_gen/ai_gen_case.py 00112233445566778899aabbccddeeff --preview
```
- 未传 seed 则随机。
- `--preview` 同时导出 `file_1.jpg` / `file_2.jpg` 便于肉眼查看。
- `--base` 自定义输出根目录，默认 `<仓库>/cases`。
- 未配置密钥时自动降级为程序化图（见 `case_common.gen_case`），保证可跑。

## 密钥
方舟接口共用一把 `ARK_API_KEY`，密钥文件为 `src/ai_gen/.env`（**已被 `.gitignore` 排除，切勿入库**）：
- `ARK_API_KEY`：方舟 API 密钥。
- `ARK_MODEL`：文生图（Seedream）接入点 endpoint id，渲染 `file_1`/`file_2` 画面。
- `ARK_LLM_MODEL`：**可选**，大模型（doubao 系列）接入点 endpoint id，用于生成案件剧本（剧情 / 动机 / 时间线 / 供词 / 逻辑链）。

配置方式有三种，任选其一：
1. **网页设置（推荐）**：启动 `ui_prototype/server.py` 后，在页面右上角「⚙ 设置」里填入密钥并保存，服务端会自动写入本目录 `.env`。页面只显示掩码、不回传明文。
2. **手动复制模板**：`cp .env.example .env` 后填入真实值。
3. **环境变量 / 外部文件**：直接导出 `ARK_API_KEY` 等环境变量；或用 `--dotenv /path/to/.env` 指向仓库外的密钥文件。

## 无密钥也能跑
**完全不配置密钥也能正常生成与游玩**：程序检测不到 `ARK_API_KEY` 时会自动降级——
`file_1`/`file_2` 改用 Pillow 程序化绘制、剧情改用固定模板，流程与产物结构完全一致。
- 网页端点「现场生成新案」时，若当前无密钥会**先弹窗提示**：可选「仍要生成」（走程序化模板）或「去配置密钥」（跳到设置页）。
- 命令行端则直接降级，无需交互。
- 未配置 `ARK_LLM_MODEL`（但有 `ARK_API_KEY`）时，仅剧本回退到程序化模板，出图仍走云端。

## 与主流程的对接
生成的 `session_<id>/` 交给主项目房主侧：
```bash
./client_transfer --host --session session_<id> --room localhost --nick host
```
清单格式、块哈希、种子红线（种子只留内存、对外仅 SHA-256 摘要）均与主生成器相同。
