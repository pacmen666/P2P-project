#!/usr/bin/env python3
"""生成一局密室逃脱的房间：云端 AI 画背景 + 物件，抠白后拼到背景顶层。

链路：call_ark(背景/物件) -> remove_white(抠白幕) -> alpha_composite(拼顶层)
       -> 输出合成房间图 + 透明物件 PNG + 热点坐标 JSON（接 escape_room 前端）

输出（每间房）：
  room_<n>.jpg        合成房间图（物件已拼入，门作为背景的一部分）
  item_<n>_<id>.png   每个物件的透明抠图（供前端独立摆放 / 调试）
  hotspots_<n>.json   热点坐标（百分比中心点 + 像素尺寸），对齐 escape_room/mock/app.js

用法：
  python gen_room.py                    生成全部 3 间房（需 .env 里的 ARK_API_KEY）
  python gen_room.py --room 1           只生成第 1 间
  python gen_room.py --seed <hex>       固定种子复现
  python gen_room.py --dotenv /path/.env --out ../../escape_room/gen
  python gen_room.py --no-ai            无 key 时强制走程序化占位（保底可跑）
"""
import argparse
import json
import os
import sys
import time
from io import BytesIO

from PIL import Image, ImageDraw, ImageFilter

_HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _HERE)

from ai_gen_case import ARK_API, call_ark, load_dotenv          # noqa: E402
from remove_bg import remove_white                              # noqa: E402
from seed_image import PRNG                                     # noqa: E402

# 房间模板：坐标与 escape_room/mock/app.js 的 hotspots 对齐（百分比中心点）
ROOMS = [
    {
        "name": "书房",
        "bg_prompt": ("a cozy Victorian study room, wooden bookshelves and a desk, "
                      "a closed wooden door on the right wall, warm dim candle light, "
                      "cinematic, moody, empty room, no people, no readable text"),
        "items": [
            {"id": "desk", "name": "书桌", "x": 22, "y": 70, "w": 280, "h": 220,
             "prompt": "an antique wooden writing desk, isolated on pure solid white background, product shot, centered, no shadow, no text"},
            {"id": "painting", "name": "挂画", "x": 50, "y": 28, "w": 200, "h": 240,
             "prompt": "an old framed oil painting of a landscape, isolated on pure solid white background, no shadow, no text"},
            {"id": "shelf", "name": "书架", "x": 68, "y": 66, "w": 240, "h": 260,
             "prompt": "a wooden bookshelf full of books, isolated on pure solid white background, product shot, no text"},
        ],
        "door": {"id": "door", "name": "房门", "x": 88, "y": 42},
    },
    {
        "name": "实验室",
        "bg_prompt": ("a dim laboratory room, shelves with glass bottles, a lab bench, "
                      "a closed metal door on the right wall, cold blue lighting, "
                      "cinematic, empty room, no people, no readable text"),
        "items": [
            {"id": "bench", "name": "实验台", "x": 26, "y": 66, "w": 300, "h": 220,
             "prompt": "a laboratory bench with glassware, isolated on pure solid white background, product shot, no shadow, no text"},
            {"id": "cabinet", "name": "药品柜", "x": 62, "y": 60, "w": 240, "h": 260,
             "prompt": "a medicine cabinet with glass doors and bottles, isolated on pure solid white background, product shot, no text"},
        ],
        "door": {"id": "door", "name": "房门", "x": 88, "y": 42},
    },
    {
        "name": "档案室",
        "bg_prompt": ("an archive room, tall filing cabinets and a wall map, "
                      "a closed door on the right wall, dim warm light, dust in air, "
                      "cinematic, empty room, no people, no readable text"),
        "items": [
            {"id": "map", "name": "挂图", "x": 48, "y": 30, "w": 280, "h": 220,
             "prompt": "an old wall map on parchment, isolated on pure solid white background, no text labels"},
            {"id": "file", "name": "档案柜", "x": 24, "y": 68, "w": 240, "h": 260,
             "prompt": "a tall metal filing cabinet with drawers, isolated on pure solid white background, product shot, no text"},
        ],
        "door": {"id": "door", "name": "出口", "x": 88, "y": 42},
    },
]

BG_SIZE = 1024
ITEM_SIZE = 1024   # 方舟只支持标准尺寸（如 1024），512 会 400；生成后再缩到目标 w/h


def _to_image(raw):
    return Image.open(BytesIO(raw)).convert("RGBA")


def _call_ark_retry(model, key, prompt, size, tries=3):
    """带重试的云端调用：应对 IncompleteRead / 偶发网络中断。"""
    last = None
    for i in range(tries):
        try:
            return call_ark(model, key, prompt, size=size)
        except Exception as e:
            last = e
            if i < tries - 1:
                sys.stderr.write(f"  [retry {i + 1}/{tries - 1}] {e}\n")
                time.sleep(2 * (i + 1))
    raise last


def _resize_bg(im, size):
    s = min(im.size)
    im = im.crop(((im.width - s) // 2, (im.height - s) // 2,
                  (im.width - s) // 2 + s, (im.height - s) // 2 + s))
    return im.resize((size, size))


def placeholder_bg(name, size):
    """无 key 时的程序化占位底图（纯色渐变 + 房名），保证脚本可跑。"""
    im = Image.new("RGB", (size, size), (40, 44, 58))
    d = ImageDraw.Draw(im)
    for y in range(size):
        c = (40 + y // 20, 44 + y // 20, 58 + y // 14)
        d.line([(0, y), (size, y)], fill=c)
    # 模拟"门"在右侧墙
    d.rectangle([size - 200, size // 2 - 180, size - 60, size // 2 + 180],
                outline=(150, 120, 80), width=6)
    d.text((40, 40), name + "（占位底图）", fill=(200, 200, 210))
    return im.convert("RGBA")


def gen_one_room(room, idx, key, model, use_ai, out_dir, thresh):
    name = room["name"]
    items = []

    # 1) 背景
    if use_ai:
        try:
            bg = _resize_bg(_to_image(_call_ark_retry(model, key, room["bg_prompt"], size=BG_SIZE)), BG_SIZE)
        except Exception as e:
            if os.environ.get("STRICT"):
                raise
            sys.stderr.write(f"[warn] 房间{idx}「{name}」背景 AI 失败，用占位底图：{e}\n")
            bg = placeholder_bg(name, BG_SIZE)
    else:
        bg = placeholder_bg(name, BG_SIZE)

    # 2) 物件：生成 -> 抠白 -> 缩到目标尺寸 -> 拼顶层
    hotspots = []
    for it in room["items"]:
        cx = int(BG_SIZE * it["x"] / 100)
        cy = int(BG_SIZE * it["y"] / 100)
        pos = (cx - it["w"] // 2, cy - it["h"] // 2)
        img_path = None
        if use_ai:
            try:
                raw = _call_ark_retry(model, key, it["prompt"], size=ITEM_SIZE)
                cut = remove_white(_to_image(raw), thresh=thresh)
                # 缩到目标尺寸并轻度羽化软边
                cut = cut.resize((it["w"], it["h"]), Image.LANCZOS)
                bg.alpha_composite(cut, pos)
                img_path = f"item_{idx}_{it['id']}.png"
                cut.save(os.path.join(out_dir, img_path), "PNG")
            except Exception as e:
                if os.environ.get("STRICT"):
                    raise
                sys.stderr.write(f"[warn] 物件「{it['name']}」AI 失败，跳过：{e}\n")
        hotspots.append({
            "id": it["id"], "name": it["name"],
            "x": it["x"], "y": it["y"], "w": it["w"], "h": it["h"],
            "img": img_path,
        })

    # 3) 落盘
    bg_path = f"room_{idx}.jpg"
    bg.convert("RGB").save(os.path.join(out_dir, bg_path), "JPEG", quality=90)

    manifest = {
        "room": name,
        "bg": bg_path,
        "width": BG_SIZE, "height": BG_SIZE,
        "door": room["door"],
        "hotspots": hotspots,
    }
    jpath = f"hotspots_{idx}.json"
    with open(os.path.join(out_dir, jpath), "w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)

    n_ai = sum(1 for h in hotspots if h["img"])
    print(f"[房间{idx}] {name}  背景={bg_path}  物件 {n_ai}/{len(hotspots)} 个已拼入  -> {jpath}")
    return manifest


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--room", type=int, default=0, help="只生成第 n 间（1 起），0=全部")
    ap.add_argument("--seed", default=None, help="固定种子 hex（>=16 字节）")
    ap.add_argument("--dotenv", default=None)
    ap.add_argument("--out", default=None, help="输出目录，默认 <repo>/escape_room/gen")
    ap.add_argument("--no-ai", action="store_true", help="跳过云端，只出占位图")
    ap.add_argument("--thresh", type=int, default=235, help="抠白阈值（RGB 均 >= 该值视为背景）")
    ap.add_argument("--strict", action="store_true", help="AI 失败即报错（默认降级占位）")
    args = ap.parse_args()

    load_dotenv(args.dotenv)
    if args.strict:
        os.environ["STRICT"] = "1"
    key, model = os.environ.get("ARK_API_KEY"), os.environ.get("ARK_MODEL")
    use_ai = bool(key and model) and not args.no_ai
    if not use_ai:
        sys.stderr.write("[warn] 未配置 ARK_API_KEY/ARK_MODEL 或 --no-ai，走程序化占位图\n")

    out_dir = args.out or os.path.normpath(os.path.join(_HERE, "..", "..", "escape_room", "gen"))
    os.makedirs(out_dir, exist_ok=True)

    sel = list(range(1, len(ROOMS) + 1)) if not args.room else [args.room]
    for idx in sel:
        if idx < 1 or idx > len(ROOMS):
            sys.exit(f"房间序号越界：{idx}（共 {len(ROOMS)} 间）")
        gen_one_room(ROOMS[idx - 1], idx, key, model, use_ai, out_dir, args.thresh)

    print(f"\n全部输出到：{out_dir}")
    print("前端接入：把 escape_room/mock/app.js 的 rooms 换成加载这些 hotspots_<n>.json 即可。")


if __name__ == "__main__":
    main()
