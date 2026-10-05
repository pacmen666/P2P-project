"""通用零件 + 种子拼接 = 本局证物图（悬疑刑侦风格）。

思路：
- parts/ 里放**无文字**的通用素材（AI 生成或手绘均可）：纸张、房间线稿、指纹、证物卡、污渍。
- 运行时由种子决定：用哪张纸、贴哪枚指纹、污渍在哪、色调、房间名、时间、线索句。
- 中文文字一律用 PIL 叠加（AI 画中文多半是乱码），保证清晰可控。
- parts/ 为空时自动退回程序化底图，照常能跑。

零件命名只要包含这些关键词即可自动识别：
    paper(纸张) plan(平面图线稿) finger(指纹) card(证物卡) stain(污渍)

运行：
    python compose_evidence.py                    随机种子
    python compose_evidence.py --seed <hex>       固定种子复现
"""
import hashlib
import io
import os
import sys

from PIL import Image, ImageChops, ImageDraw

from seed_image import PRNG
from doc_image import (FURNITURE, OFFICERS, ROOMS, PIECE, aged_paper, clamp,
                       fonts, stamp)

_HERE = os.path.dirname(os.path.abspath(__file__))
PARTS_DIRS = (                                        # 依次扫描（不受当前工作目录影响）
    os.path.join(_HERE, "parts"),                          # ★ 首选：脚本旁边 tests/py_test/parts/
    os.path.join(_HERE, "parts_export"),                   # ★ 同上，兼容 parts_export 目录名
    _HERE,                                                 # 素材直接平铺在脚本目录里也认
    os.path.join(_HERE, "..", "..", "assets", "parts"),    # <仓库>/assets/parts
    os.path.join(_HERE, "..", "..", "..", "image"),        # <仓库>/../image（本地素材，不进仓库）
    "parts",
    os.path.join("..", "image"),
)
KEYS = ("paper", "plan", "finger", "card", "stain")
# 文件名关键词（中英文均可）→ 零件类别
ALIAS = {
    "paper": "paper", "纸张": "paper",
    "plan": "plan", "平面": "plan", "线稿": "plan",
    "finger": "finger", "指纹": "finger",
    "card": "card", "证物卡": "card", "证物袋": "card",
    "stain": "stain", "污渍": "stain",
}


def load_parts():
    """扫描 PARTS_DIRS；同一文件经多条路径命中时只算一次（按真实路径去重）。"""
    parts = {k: [] for k in KEYS}
    seen = set()
    for directory in PARTS_DIRS:
        if not os.path.isdir(directory):
            continue
        for fn in sorted(os.listdir(directory)):
            if not fn.lower().endswith((".png", ".jpg", ".jpeg")):
                continue
            path = os.path.join(directory, fn)
            real = os.path.realpath(path)
            if real in seen:
                continue
            low = fn.lower()
            for token, key in ALIAS.items():
                if token in fn or token in low:
                    seen.add(real)
                    parts[key].append(path)
                    break
    return parts


def load_img(path):
    """读入零件并裁掉底部 9%（去掉生成平台的水印）。"""
    im = Image.open(path).convert("RGB")
    return im.crop((0, 0, im.width, int(im.height * 0.91)))


def pick(prng, lst):
    return lst[prng.randint(0, len(lst) - 1)] if lst else None


def center_crop(im, w, h):
    return im.crop(((im.width - w) // 2, (im.height - h) // 2,
                    (im.width - w) // 2 + w, (im.height - h) // 2 + h))


def overlay(base, art, cx, cy, w, h, angle=0, strength=1.0):
    """把线稿类素材以正片叠底方式贴到底图上（白底自动变透明）。"""
    a = art.convert("RGB").resize((w, h))
    a = a.rotate(angle, expand=True, fillcolor=(255, 255, 255))
    a = center_crop(a, w, h)
    x = max(0, min(base.width - w, cx - w // 2))
    y = max(0, min(base.height - h, cy - h // 2))
    region = base.crop((x, y, x + w, y + h))
    merged = ImageChops.multiply(region, a)
    if strength < 1.0:
        merged = Image.blend(region, merged, strength)
    base.paste(merged, (x, y))


def tint(base, prng):
    """按种子给整图加一点色偏，让不同局气质不同。"""
    dr, dg, db = (prng.randint(-10, 10), prng.randint(-8, 8), prng.randint(-14, 6))
    px = base.load()
    for y in range(base.height):
        for x in range(base.width):
            r, g, b = px[x, y]
            px[x, y] = (clamp(r + dr), clamp(g + dg), clamp(b + db))


# ---------- 图一：现场勘查图 ----------
def _dump(img, path, comment):
    """保存为 JPEG；comment 写入 JPEG 的 COM 注释段（答案就藏在这里，不在清单里）。"""
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=88, comment=comment or b"")
    data = buf.getvalue()
    if path:
        with open(path, "wb") as f:
            f.write(data)
    return data


def make_plan(seed, parts, path=None, comment=None):
    prng = PRNG(seed)
    f_big, f_small = fonts()
    hour, minute = prng.randint(20, 23), prng.randint(10, 59)
    W, H = 680, 480
    img = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(img)

    paper = pick(prng, parts["paper"])
    if paper:
        base = load_img(paper).resize((W, H))
        img.paste(base, (0, 0))
    else:
        aged_paper(img, d, W, H, prng)

    # 房间线稿（AI 零件）贴进图框
    plan = pick(prng, parts["plan"])
    if plan:
        overlay(img, load_img(plan), W // 2, 300, 560, 330, strength=0.85)
    # 程序化补充：家具框与倒地位置
    n_fur = prng.randint(2, 5)
    for _ in range(n_fur):
        fw, fh = prng.randint(52, 108), prng.randint(36, 66)
        fx = prng.randint(70, W - 70 - fw)
        fy = prng.randint(230, 430 - fh)
        d.rectangle([fx, fy, fx + fw, fy + fh], outline=(70, 70, 70), width=2)
        d.text((fx + 5, fy + 4), FURNITURE[prng.randint(0, len(FURNITURE) - 1)],
               font=f_small, fill=(55, 55, 55))
    bx, by = prng.randint(120, W - 120), prng.randint(260, 420)
    d.ellipse([bx - 16, by - 16, bx + 16, by + 16], outline=(180, 40, 40), width=2)
    d.line([(bx - 11, by - 11), (bx + 11, by + 11)], fill=(180, 40, 40), width=3)
    d.line([(bx - 11, by + 11), (bx + 11, by - 11)], fill=(180, 40, 40), width=3)
    d.text((bx + 20, by - 8), "倒地位置", font=f_small, fill=(180, 40, 40))

    # 指纹与污渍（AI 零件）
    finger = pick(prng, parts["finger"])
    if finger:
        overlay(img, load_img(finger), prng.randint(60, 160), prng.randint(380, 450),
                96, 116, angle=prng.randint(-25, 25), strength=0.9)
    stain = pick(prng, parts["stain"])
    if stain:
        overlay(img, load_img(stain), prng.randint(200, W - 120), prng.randint(300, 440),
                prng.randint(110, 190), prng.randint(90, 150),
                angle=prng.randint(0, 359), strength=prng.randint(40, 80) / 100)

    # 文字与印章
    room = ROOMS[prng.randint(0, len(ROOMS) - 1)]
    d.rectangle([14, 14, W - 14, H - 14], outline=(70, 70, 70), width=2)
    d.text((30, 26), f"现场勘查图 · {room}", font=f_big, fill=(35, 35, 35))
    d.text((30, 58), f"案件编号：FA-{seed.hex()[:4].upper()}    "
                     f"制图时间：{hour:02d}:{minute:02d}",
           font=f_small, fill=(80, 80, 80))
    d.text((W - 150, H - 40), f"勘查：{OFFICERS[prng.randint(0, len(OFFICERS) - 1)]}",
           font=f_small, fill=(80, 80, 80))
    stamp(d, W - 90, H - 92, "档案专用", f_small, prng)

    if comment is None:   # 默认把"案发时间"写进 JPEG 注释段（答案在文件正文里，不在清单里）
        comment = f"案发时间：{hour:02d}:{minute:02d}".encode("utf-8")
    tint(img, prng)
    data = _dump(img, path, comment)
    return data, {"time": f"{hour:02d}:{minute:02d}", "room": room}


# ---------- 图二：证物登记卡 ----------
def make_card(seed, parts, path=None, comment=None, note=None):
    prng = PRNG(seed + b"\x02")
    f_big, f_small = fonts()
    W, H = 680, 480
    img = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(img)

    card = pick(prng, parts["card"]) or pick(prng, parts["paper"])
    if card:
        img.paste(load_img(card).resize((W, H)), (0, 0))
    else:
        aged_paper(img, d, W, H, prng)

    # 物证照片框：用线稿/指纹零件裁一块当"照片"
    px0, py0, px1, py1 = 44, 104, 330, 280
    d.rectangle([px0, py0, px1, py1], outline=(60, 60, 60), width=2)
    src = pick(prng, parts["plan"]) or pick(prng, parts["finger"]) or pick(prng, parts["stain"])
    if src:
        art = load_img(src)
        crop = center_crop(art, min(art.width, art.height), min(art.width, art.height))
        crop = crop.rotate(prng.randint(-8, 8), fillcolor=(255, 255, 255))
        img.paste(crop.resize((px1 - px0 - 6, py1 - py0 - 6)), (px0 + 3, py0 + 3))
    else:
        pr, pg, pb = prng.randint(40, 110), prng.randint(40, 110), prng.randint(40, 110)
        for y in range(py0 + 3, py1 - 2):
            for x in range(px0 + 3, px1 - 2):
                n = prng.randint(-14, 14)
                d.point((x, y), fill=(clamp(pr + n + (x - px0) // 8),
                                      clamp(pg + n + (y - py0) // 8), clamp(pb + n)))
    d.text((px0 + 6, py1 + 8), "物证照片（现场提取）", font=f_small, fill=(80, 80, 80))

    finger = pick(prng, parts["finger"])
    if finger:
        overlay(img, load_img(finger), W - 90, 170, 92, 112,
                angle=prng.randint(-20, 20), strength=0.85)
    stain = pick(prng, parts["stain"])
    if stain:
        overlay(img, load_img(stain), prng.randint(60, W - 120), prng.randint(320, 440),
                prng.randint(100, 170), prng.randint(80, 130),
                angle=prng.randint(0, 359), strength=prng.randint(35, 75) / 100)

    items = ["青瓷茶杯", "蓝羊毛围巾", "铜制裁纸刀", "未寄出的信", "停摆的挂钟"]
    places = ["书房地毯上", "门厅衣架上", "花房工具箱", "卧室抽屉内", "厨房灶台边"]
    notes = ["杯沿缠有一根蓝色羊毛纤维。", "围巾上沾着几点茶渍，尚未干透。",
             "刀柄上只有死者自己的指纹。", "信纸落款时间比挂钟早一小时。",
             "指针停在某一刻，发条却是上满的。"]
    i = prng.randint(0, len(items) - 1)

    fx, yy = 356, 110
    for k, v in (("证物名称", items[i]), ("发现位置", places[prng.randint(0, len(places) - 1)]),
                 ("提取人", OFFICERS[prng.randint(0, len(OFFICERS) - 1)]),
                 ("状态", ["完好", "有裂痕", "沾有污渍"][prng.randint(0, 2)])):
        d.text((fx, yy), f"{k}：", font=f_small, fill=(100, 100, 100))
        d.text((fx + 78, yy), v, font=f_small, fill=(30, 30, 30))
        yy += 34

    if note is None:          # 允许外部指定"关键一条"（由种子与凶手决定）
        note = notes[i]
    d.text((44, 308), "备注：", font=f_small, fill=(100, 100, 100))
    d.text((44, 330), note, font=f_small, fill=(150, 30, 30))
    d.line([(44, 404), (310, 404)], fill=(120, 120, 120), width=1)
    d.text((44, 410), "经办人签字", font=f_small, fill=(120, 120, 120))

    d.rectangle([14, 14, W - 14, H - 14], outline=(70, 70, 70), width=2)
    d.text((30, 26), "证物登记卡", font=f_big, fill=(35, 35, 35))
    d.text((30, 58), f"登记号：EV-{seed.hex()[4:8].upper()}", font=f_small, fill=(80, 80, 80))
    stamp(d, W - 94, H - 96, "已登记", f_small, prng)

    if comment is None:   # 默认把"备注（线索）"写进注释段
        comment = note.encode("utf-8")
    tint(img, prng)
    data = _dump(img, path, comment)
    return data, {"item": items[i], "note": note}


def digests(data):
    n = (len(data) + PIECE - 1) // PIECE
    return (hashlib.sha256(data).hexdigest(), n,
            [hashlib.sha256(data[i * PIECE:(i + 1) * PIECE]).hexdigest() for i in range(n)])


def main():
    seed = (bytes.fromhex(sys.argv[sys.argv.index("--seed") + 1])
            if "--seed" in sys.argv else os.urandom(32))
    parts = load_parts()
    used = {k: len(v) for k, v in parts.items()}
    print(f"seed_hash = {hashlib.sha256(seed).hexdigest()[:16]}...（种子不出进程）")
    print(f"零件：{used}" + ("" if sum(used.values()) else "  → 未找到零件，使用程序化兜底"))

    for name, fn in (("ev_plan", make_plan), ("ev_card", make_card)):
        data, info = fn(seed, parts, f"{name}.jpg")
        sha, pc, pieces = digests(data)
        print(f"  生成信息：{info}")
        print(f"\n[{name}.jpg] {len(data)} 字节  piece_count={pc}")
        assert len(data) <= 512 * 1024, "超过 512 KiB 上限（约定第 4 节）"
        print(f"  整文件 SHA256 = {sha[:32]}...")
        print(f"  块摘要[0]     = {pieces[0][:32]}...")


if __name__ == "__main__":
    main()
