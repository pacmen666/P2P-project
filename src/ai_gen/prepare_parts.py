"""把原始高清素材压缩成可提交的小零件（去掉硬编码路径，可在任意机器运行）。

- 裁掉底部 9%（去掉生成平台水印）
- 中心裁成正方形，缩到 640x640（拼接时会被 resize 到 680x480，原图 1024 是浪费）
- 转 JPEG quality=82
默认源目录 <仓库>/../image（仓库外的本地原始素材，不入库）；
默认输出   <仓库>/assets/parts。均可通过命令行覆盖：

    python prepare_parts.py [SRC_DIR] [--dst DST_DIR]
"""
import argparse
import os

from PIL import Image

_HERE = os.path.dirname(os.path.abspath(__file__))
_REPO = os.path.abspath(os.path.join(_HERE, "..", ".."))
DEFAULT_SRC = os.path.abspath(os.path.join(_REPO, "..", "image"))
DEFAULT_DST = os.path.join(_REPO, "assets", "parts")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src", nargs="?", default=DEFAULT_SRC, help="原始素材目录")
    ap.add_argument("--dst", default=DEFAULT_DST, help="零件输出目录")
    args = ap.parse_args()

    if not os.path.isdir(args.src):
        raise SystemExit(f"素材目录不存在：{args.src}")
    os.makedirs(args.dst, exist_ok=True)
    total = 0
    for fn in sorted(os.listdir(args.src)):
        if not fn.lower().endswith((".png", ".jpg", ".jpeg")):
            continue
        im = Image.open(os.path.join(args.src, fn)).convert("RGB")
        im = im.crop((0, 0, im.width, int(im.height * 0.91)))          # 去水印
        s = min(im.width, im.height)                                    # 中心裁方
        im = im.crop(((im.width - s) // 2, (im.height - s) // 2,
                      (im.width - s) // 2 + s, (im.height - s) // 2 + s))
        im = im.resize((640, 640))
        out = os.path.join(args.dst, os.path.splitext(fn)[0] + ".jpg")
        im.save(out, "JPEG", quality=82)
        kb = os.path.getsize(out) // 1024
        total += kb
        print(f"{fn}  ->  {os.path.basename(out)}  {kb} KB")
    print(f"\n合计 {total} KB")


if __name__ == "__main__":
    main()
