"""档案扫描件风格的种子出图（房主侧案卷用图）。

与 seed_image.py 的区别：那张是"随机几何"，看不出内容；
这张是**有信息的证物**——现场勘查平面图 + 证物登记卡，
带做旧纸张、中文标注、红色印章，所有内容由种子决定。

设计要点：
- 种子只留内存，不写文件/文件名/界面（AGENT.md 红线）。
- HMAC-SHA256(seed, 计数器) 作确定性密钥流：同种子必得同图，不同种子必不同。
- 图里承载线索（倒地位置、发现位置、备注），而不只是"好看的噪声"。

运行：
    python doc_image.py                      随机种子
    python doc_image.py --seed <hex>         固定种子复现

移植到 C 的提示：
- 纸色/噪点/做旧/矩形/线条/圆环印章：纯像素与几何，C 里直接写。
- 中文文字：C 里要么内嵌点阵字库，要么图上只用数字与英文编号、
  把中文线索放进文本文件（file_2 / file_3）。
"""
import hashlib
import os
import sys

from PIL import Image, ImageDraw, ImageFont

from seed_image import PRNG

PIECE = 16384
FONT_PATH = "C:/Windows/Fonts/msyh.ttc"

ROOMS = ["书房", "卧室", "花房", "厨房", "门厅", "阁楼"]
FURNITURE = ["书桌", "书架", "躺椅", "茶几", "木柜", "屏风", "落地钟"]
OFFICERS = ["警员 陈", "警员 彭", "警员 王", "警员 刘"]


def clamp(v):
    return 0 if v < 0 else (255 if v > 255 else v)


def fonts():
    return (ImageFont.truetype(FONT_PATH, 22), ImageFont.truetype(FONT_PATH, 15))


# ---------- 做旧纸张 ----------
def aged_paper(img, draw, w, h, prng):
    base = (prng.randint(230, 246), prng.randint(222, 240), prng.randint(202, 224))
    draw.rectangle([0, 0, w, h], fill=base)
    px = img.load()
    for _ in range(prng.randint(3000, 7000)):          # 噪点
        x, y = prng.randint(0, w - 1), prng.randint(0, h - 1)
        v = prng.randint(-20, 14)
        r, g, b = px[x, y]
        px[x, y] = (clamp(r + v), clamp(g + v), clamp(b + v))
    for _ in range(prng.randint(2, 5)):                # 茶渍/水痕
        cx, cy = prng.randint(0, w), prng.randint(0, h)
        rx, ry = prng.randint(30, 90), prng.randint(20, 60)
        c = prng.randint(150, 200)
        draw.ellipse([cx - rx, cy - ry, cx + rx, cy + ry],
                     outline=(c, c - 15, c - 40), width=prng.randint(1, 3))
    for _ in range(prng.randint(1, 3)):                # 折痕
        y = prng.randint(0, h)
        draw.line([(0, y), (w, y)], fill=(190, 180, 160), width=1)
    return base


# ---------- 红色印章 ----------
def stamp(draw, cx, cy, text, f_small, prng):
    r = 42
    red = (prng.randint(170, 200), prng.randint(30, 60), prng.randint(30, 55))
    draw.ellipse([cx - r, cy - r, cx + r, cy + r], outline=red, width=3)
    draw.ellipse([cx - r + 6, cy - r + 6, cx + r - 6, cy + r - 6], outline=red, width=1)
    draw.text((cx - 20, cy - 10), text, font=f_small, fill=red)
    draw.line([(cx - r, cy), (cx + r, cy)], fill=red, width=1)


# ---------- 图一：现场勘查平面图 ----------
def floor_plan(seed, path):
    prng = PRNG(seed)
    f_big, f_small = fonts()
    W, H = 660, 470
    img = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(img)
    aged_paper(img, d, W, H, prng)

    case_no = f"FA-{seed.hex()[:4].upper()}"
    hour = prng.randint(20, 23)
    minute = prng.randint(10, 59)
    room = prng.randint(0, len(ROOMS) - 1)

    d.rectangle([14, 14, W - 14, H - 14], outline=(70, 70, 70), width=2)
    d.text((30, 26), f"现场勘查图 · {ROOMS[room]}", font=f_big, fill=(40, 40, 40))
    d.text((30, 58), f"案件编号：{case_no}    制图时间：{hour:02d}:{minute:02d}",
           font=f_small, fill=(80, 80, 80))

    # 平面图主体
    x0, y0, x1, y1 = 46, 96, W - 46, H - 66
    d.rectangle([x0, y0, x1, y1], outline=(50, 50, 50), width=3)

    # 内部隔墙（带门洞）
    if prng.randint(0, 1):
        wx = prng.randint(x0 + 120, x1 - 120)
        gap_y = prng.randint(y0 + 60, y1 - 90)
        d.line([(wx, y0), (wx, gap_y)], fill=(60, 60, 60), width=3)
        d.line([(wx, gap_y + 46), (wx, y1)], fill=(60, 60, 60), width=3)
    else:
        wy = prng.randint(y0 + 60, y1 - 90)
        gap_x = prng.randint(x0 + 60, x1 - 140)
        d.line([(x0, wy), (gap_x, wy)], fill=(60, 60, 60), width=3)
        d.line([(gap_x + 46, wy), (x1, wy)], fill=(60, 60, 60), width=3)

    # 窗（双线）
    side = prng.randint(0, 3)
    if side == 0:
        d.line([(x0 + 60, y0), (x0 + 160, y0)], fill=(90, 120, 160), width=4)
    elif side == 1:
        d.line([(x1, y0 + 60), (x1, y0 + 160)], fill=(90, 120, 160), width=4)
    elif side == 2:
        d.line([(x0 + 60, y1), (x0 + 160, y1)], fill=(90, 120, 160), width=4)
    else:
        d.line([(x0, y0 + 60), (x0, y0 + 160)], fill=(90, 120, 160), width=4)

    # 家具
    used = []
    for _ in range(prng.randint(3, 6)):
        fw = prng.randint(48, 110)
        fh = prng.randint(34, 70)
        fx = prng.randint(x0 + 12, x1 - fw - 12)
        fy = prng.randint(y0 + 12, y1 - fh - 12)
        d.rectangle([fx, fy, fx + fw, fy + fh], outline=(80, 80, 80), width=2)
        name = FURNITURE[prng.randint(0, len(FURNITURE) - 1)]
        d.text((fx + 5, fy + 5), name, font=f_small, fill=(60, 60, 60))
        used.append((fx, fy, fw, fh))

    # 倒地位置（关键线索：× + 圈）
    bx = prng.randint(x0 + 40, x1 - 40)
    by = prng.randint(y0 + 40, y1 - 40)
    d.ellipse([bx - 16, by - 16, bx + 16, by + 16], outline=(180, 40, 40), width=2)
    d.line([(bx - 11, by - 11), (bx + 11, by + 11)], fill=(180, 40, 40), width=3)
    d.line([(bx - 11, by + 11), (bx + 11, by - 11)], fill=(180, 40, 40), width=3)
    d.text((bx + 20, by - 8), "倒地位置", font=f_small, fill=(180, 40, 40))

    # 指北针 + 落款
    d.text((x1 - 60, y0 + 8), "↑ N", font=f_small, fill=(60, 60, 60))
    d.text((x0, y1 + 14), f"勘查：{OFFICERS[prng.randint(0, len(OFFICERS) - 1)]}",
           font=f_small, fill=(80, 80, 80))
    stamp(d, W - 92, H - 84, "档案专用", f_small, prng)

    img.save(path)
    with open(path, "rb") as f:
        return f.read()


# ---------- 图二：证物登记卡 ----------
def evidence_card(seed, path):
    prng = PRNG(seed + b"\x02")
    f_big, f_small = fonts()
    W, H = 660, 470
    img = Image.new("RGB", (W, H), (255, 255, 255))
    d = ImageDraw.Draw(img)
    aged_paper(img, d, W, H, prng)

    item = ["青瓷茶杯", "蓝羊毛围巾", "铜制裁纸刀", "未寄出的信", "停摆的挂钟"]
    place = ["书房地毯上", "门厅衣架上", "花房工具箱", "卧室抽屉内", "厨房灶台边"]
    idx_i = prng.randint(0, len(item) - 1)
    idx_p = prng.randint(0, len(place) - 1)

    d.rectangle([14, 14, W - 14, H - 14], outline=(70, 70, 70), width=2)
    d.text((30, 26), "证物登记卡", font=f_big, fill=(40, 40, 40))
    d.text((30, 58), f"登记号：EV-{seed.hex()[4:8].upper()}", font=f_small, fill=(80, 80, 80))

    # 证物照片框（内部为程序化图案，代表实物照片）
    px0, py0, px1, py1 = 40, 96, 320, 266
    d.rectangle([px0, py0, px1, py1], outline=(60, 60, 60), width=2)
    pr = prng.randint(30, 110)
    pg = prng.randint(30, 110)
    pb = prng.randint(30, 110)
    for y in range(py0 + 2, py1 - 1):
        for x in range(px0 + 2, px1 - 1):
            n = prng.randint(-12, 12)
            d.point((x, y), fill=(clamp(pr + n + (x - px0) // 8),
                                  clamp(pg + n + (y - py0) // 8),
                                  clamp(pb + n)))
    for _ in range(prng.randint(1, 3)):
        cx = prng.randint(px0 + 30, px1 - 30)
        cy = prng.randint(py0 + 30, py1 - 30)
        rr = prng.randint(16, 42)
        d.ellipse([cx - rr, cy - rr, cx + rr, cy + rr],
                  outline=(prng.randint(0, 255), prng.randint(0, 255), prng.randint(0, 255)),
                  width=2)
    d.text((px0 + 6, py1 + 6), "物证照片（现场提取）", font=f_small, fill=(80, 80, 80))

    # 登记字段
    fx = 350
    fields = [
        ("证物名称", item[idx_i]),
        ("发现位置", place[idx_p]),
        ("提取人", OFFICERS[prng.randint(0, len(OFFICERS) - 1)]),
        ("状态", ["完好", "有裂痕", "沾有污渍"][prng.randint(0, 2)]),
    ]
    yy = 100
    for k, v in fields:
        d.text((fx, yy), f"{k}：", font=f_small, fill=(100, 100, 100))
        d.text((fx + 78, yy), v, font=f_small, fill=(30, 30, 30))
        yy += 34

    # 备注 = 线索
    note = [
        "杯沿缠有一根蓝色羊毛纤维。",
        "围巾上沾着几点茶渍，尚未干透。",
        "刀柄上只有死者自己的指纹。",
        "信纸落款时间比挂钟早一小时。",
        "指针停在某一刻，发条却是上满的。",
    ]
    d.text((40, 300), "备注：", font=f_small, fill=(100, 100, 100))
    d.text((40, 322), note[idx_i], font=f_small, fill=(150, 30, 30))

    # 签名与印章
    d.line([(40, 392), (300, 392)], fill=(120, 120, 120), width=1)
    d.text((40, 398), "经办人签字", font=f_small, fill=(120, 120, 120))
    stamp(d, W - 96, H - 88, "已登记", f_small, prng)

    img.save(path)
    with open(path, "rb") as f:
        return f.read()


def digests(data):
    n = (len(data) + PIECE - 1) // PIECE
    return (hashlib.sha256(data).hexdigest(), n,
            [hashlib.sha256(data[i * PIECE:(i + 1) * PIECE]).hexdigest() for i in range(n)])


def main():
    seed = (bytes.fromhex(sys.argv[sys.argv.index("--seed") + 1])
            if "--seed" in sys.argv else os.urandom(32))
    print(f"seed_hash = {hashlib.sha256(seed).hexdigest()[:16]}...（种子不出进程）")

    for name, fn in (("doc_plan", floor_plan), ("doc_card", evidence_card)):
        path = f"{name}.png"
        data = fn(seed, path)
        sha, pc, pieces = digests(data)
        print(f"\n[{path}] {len(data)} 字节  piece_count={pc}")
        print(f"  整文件 SHA256 = {sha[:32]}...")
        print(f"  块摘要[0]     = {pieces[0][:32]}...")


if __name__ == "__main__":
    main()
