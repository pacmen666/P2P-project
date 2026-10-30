"""AI 生成素材去背景（白幕 / 绿幕抠图），输出带 alpha 的 PNG。

背景：Seedream 等文生图接口只输出 RGB（不吐真透明），prompt 写
"transparent background" 往往得到棋盘格伪透明，反而更脏。
正确姿势：让 AI 生成「纯色底 + 主体」，本地再抠掉纯色。

三种档位（按依赖从轻到重）：
  white  边缘 flood-fill 去白（推荐，零重依赖；主体内部白不误删）
  chroma 纯色幕抠（绿幕/白幕通用，按颜色距离）
  rembg  U2-Net 自动抠图（效果最好，需 pip install rembg，首次下模型）

用法：
  python remove_bg.py in.png -o out.png --mode white [--thresh 240]
  python remove_bg.py in.png -o out.png --mode chroma --key 00ff00 [--tol 90]
  python remove_bg.py in.png -o out.png --mode rembg
"""
import argparse
import collections
import os

from PIL import Image, ImageFilter

MODE_CHOICES = ("white", "chroma", "rembg")


def hex_to_rgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def _flood_from_edges(im, keep_condition):
    """从四边 flood-fill，把满足 keep_condition 的连通背景像素 alpha 置 0。

    keep_condition(pixel: (r,g,b,a)) -> bool，True 表示该像素属于背景（要抠掉）。
    只处理与图像边缘连通的区域，主体内部同色像素不会被误删。
    """
    w, h = im.size
    px = im.load()
    visited = bytearray(w * h)

    def is_bg(x, y):
        return not visited[y * w + x] and keep_condition(px[x, y])

    q = collections.deque()
    for x in range(w):
        if is_bg(x, 0):
            q.append((x, 0))
        if is_bg(x, h - 1):
            q.append((x, h - 1))
    for y in range(h):
        if is_bg(0, y):
            q.append((0, y))
        if is_bg(w - 1, y):
            q.append((w - 1, y))

    while q:
        x, y = q.popleft()
        if visited[y * w + x]:
            continue
        visited[y * w + x] = 1
        r, g, b, a = px[x, y]
        px[x, y] = (r, g, b, 0)
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < w and 0 <= ny < h and is_bg(nx, ny):
                q.append((nx, ny))
    return im


def remove_white(im, thresh=240, feather=1.5):
    """边缘 flood-fill 去白。thresh：R/G/B 均 >= 该值视为背景白。"""
    im = im.convert("RGBA")
    _flood_from_edges(im, lambda p: p[0] >= thresh and p[1] >= thresh and p[2] >= thresh)
    if feather > 0:
        # 只羽化 alpha 通道，软化锯齿边缘
        a = im.getchannel("A").filter(ImageFilter.GaussianBlur(feather))
        im.putalpha(a)
    return im


def remove_chroma(im, key=(0, 255, 0), tol=90):
    """按颜色距离抠纯色幕（绿幕/白幕通用）。key 为目标幕色，tol 为欧氏距离阈值。"""
    im = im.convert("RGBA")
    w, h = im.size
    px = im.load()
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            d = ((r - key[0]) ** 2 + (g - key[1]) ** 2 + (b - key[2]) ** 2) ** 0.5
            if d <= tol:
                px[x, y] = (r, g, b, 0)
    return im


def remove_rembg(im):
    try:
        from rembg import remove
    except ImportError as e:
        raise SystemExit("未安装 rembg：pip install rembg onnxruntime（首次运行会自动下载模型）") from e
    out = remove(im)
    return out.convert("RGBA")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("input")
    ap.add_argument("-o", "--out", default=None)
    ap.add_argument("--mode", choices=MODE_CHOICES, default="white")
    ap.add_argument("--thresh", type=int, default=240, help="white 模式：>= 该值的 RGB 视为背景")
    ap.add_argument("--key", default="00ff00", help="chroma 模式：幕色 hex，默认绿幕")
    ap.add_argument("--tol", type=int, default=90, help="chroma 模式：颜色距离阈值")
    ap.add_argument("--no-feather", action="store_true", help="white 模式关闭羽化")
    args = ap.parse_args()

    im = Image.open(args.input)

    if args.mode == "white":
        out = remove_white(im, args.thresh, 0 if args.no_feather else 1.5)
    elif args.mode == "chroma":
        out = remove_chroma(im, hex_to_rgb(args.key), args.tol)
    else:
        out = remove_rembg(im)

    out_path = args.out or (os.path.splitext(args.input)[0] + "_cut.png")
    out.save(out_path, "PNG")
    print(f"已生成透明 PNG：{out_path}  ({out.width}x{out.height})")


if __name__ == "__main__":
    main()
