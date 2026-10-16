"""一次性工具：把 image/ 里的素材压缩成可提交的小零件。

- 裁掉底部 9%（去掉生成平台水印）
- 中心裁成正方形，缩到 640x640（拼接时会被 resize 到 680x480，原图 1024 是浪费）
- 转 JPEG quality=82
输出到 parts_export/，体积从 24 MB 降到 1 MB 以内。
"""
import os

from PIL import Image

SRC = r"c:\Users\28764\Desktop\source\web_code\project\image"
DST = r"c:\Users\28764\Desktop\source\web_code\project\test_code\parts_export"

os.makedirs(DST, exist_ok=True)
total = 0
for fn in sorted(os.listdir(SRC)):
    if not fn.lower().endswith((".png", ".jpg", ".jpeg")):
        continue
    im = Image.open(os.path.join(SRC, fn)).convert("RGB")
    im = im.crop((0, 0, im.width, int(im.height * 0.91)))          # 去水印
    s = min(im.width, im.height)                                    # 中心裁方
    im = im.crop(((im.width - s) // 2, (im.height - s) // 2,
                  (im.width - s) // 2 + s, (im.height - s) // 2 + s))
    im = im.resize((640, 640))
    out = os.path.join(DST, os.path.splitext(fn)[0] + ".jpg")
    im.save(out, "JPEG", quality=82)
    kb = os.path.getsize(out) // 1024
    total += kb
    print(f"{fn}  ->  {os.path.basename(out)}  {kb} KB")
print(f"\n合计 {total} KB")
