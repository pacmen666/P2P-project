"""由随机种子程序化生成"证物图"（纯标准库，不依赖 PIL）。

设计目标（对齐 Fragment_Archive 约定）：
- 种子 >= 16 字节密码学随机，只留进程内存，不写文件名/文件内容/日志/界面。
- 同一种子 -> 同一张图；不同种子 -> 不同图（演示实验 0：仅持摘要无法本地重放）。
- 用 HMAC-SHA256(seed, 计数器) 作确定性密钥流，保证可复现且无模偏。
- 顺手算整文件 SHA-256 与 16 KiB 逻辑分块摘要（约定第 4、5.4 节）。

运行：
    python seed_image.py                 # 用随机种子生成 seed_img_1.png / seed_img_2.png
    python seed_image.py --seed <hex>    # 用给定种子复现（仅用于实验，真实产品用随机种子）
"""
import os
import sys
import zlib
import struct
import hmac
import hashlib

PIECE = 16384  # 逻辑分块 16 KiB（约定第 4 节）


# ---------- 确定性 PRNG：HMAC 计数器模式密钥流 ----------
class PRNG:
    def __init__(self, seed):
        self.seed = seed
        self._buf = b""
        self._ctr = 0

    def _refill(self, n):
        while len(self._buf) < n:
            blk = hmac.new(self.seed, struct.pack(">Q", self._ctr), hashlib.sha256).digest()
            self._buf += blk
            self._ctr += 1

    def randbytes(self, n):
        self._refill(n)
        out = self._buf[:n]
        self._buf = self._buf[n:]
        return out

    def randint(self, a, b):  # 闭区间 [a, b]，拒绝采样避免模偏
        span = b - a + 1
        limit = (1 << 32) - ((1 << 32) % span)
        while True:
            x = int.from_bytes(self.randbytes(4), "big")
            if x < limit:
                return a + x % span


def clamp(v):
    return 0 if v < 0 else (255 if v > 255 else v)


# ---------- 程序化出图（背景渐变 + 种子形状 + 噪点）----------
def generate_pixels(seed, w, h):
    prng = PRNG(seed)
    base_r = prng.randint(20, 140)
    base_g = prng.randint(20, 140)
    base_b = prng.randint(20, 140)

    px = bytearray(w * h * 3)

    def setpx(x, y, r, g, b):
        o = (y * w + x) * 3
        px[o:o + 3] = bytes([clamp(r), clamp(g), clamp(b)])

    # 背景：横向轻微渐变
    for y in range(h):
        for x in range(w):
            setpx(x, y,
                  base_r + (x // 6) - 20,
                  base_g + (y // 6) - 20,
                  base_b + ((x + y) // 10) - 20)

    # 种子形状：几个填充椭圆
    for _ in range(prng.randint(3, 8)):
        cx = prng.randint(0, w - 1)
        cy = prng.randint(0, h - 1)
        rx = prng.randint(6, w // 3)
        ry = prng.randint(6, h // 3)
        cr = prng.randint(0, 255)
        cg = prng.randint(0, 255)
        cb = prng.randint(0, 255)
        for yy in range(max(0, cy - ry), min(h, cy + ry)):
            for xx in range(max(0, cx - rx), min(w, cx + rx)):
                if ((xx - cx) ** 2) / (rx * rx) + ((yy - cy) ** 2) / (ry * ry) <= 1.0:
                    setpx(xx, yy, cr, cg, cb)

    # 种子噪点：少量高对比像素（像"污渍/标记"）
    for _ in range(prng.randint(60, 500)):
        x = prng.randint(0, w - 1)
        y = prng.randint(0, h - 1)
        setpx(x, y, prng.randint(0, 255), prng.randint(0, 255), prng.randint(0, 255))

    return px


# ---------- 纯标准库写 PNG ----------
def png_bytes(w, h, px):
    """把像素阵列编码成 PNG 字节串（不落盘）。"""
    raw = bytearray()
    for y in range(h):
        raw.append(0)  # 过滤器类型 0
        raw += px[y * w * 3:(y + 1) * w * 3]

    def chunk(typ, data):
        c = typ + data
        return struct.pack(">I", len(data)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    sig = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)  # 8位/像素 真彩色
    idat = zlib.compress(bytes(raw), 6)
    return sig + chunk(b"IHDR", ihdr) + chunk(b"IDAT", idat) + chunk(b"IEND", b"")


def make_png(path, w, h, px):
    data = png_bytes(w, h, px)
    with open(path, "wb") as f:
        f.write(data)
    return data


def digests(data):
    file_sha = hashlib.sha256(data).hexdigest()
    piece_count = (len(data) + PIECE - 1) // PIECE
    piece_hashes = [hashlib.sha256(data[i * PIECE:(i + 1) * PIECE]).hexdigest()
                    for i in range(piece_count)]
    return file_sha, piece_count, piece_hashes


def main():
    if "--seed" in sys.argv:
        i = sys.argv.index("--seed")
        seed = bytes.fromhex(sys.argv[i + 1])
        assert len(seed) >= 16, "种子需 >= 16 字节"
    else:
        seed = os.urandom(32)  # 真实产品：仅内存，不落盘

    seed_hash = hashlib.sha256(seed).hexdigest()
    print(f"seed_hash = {seed_hash}   (这是 SHA-256(种子)，可进 MANIFEST；种子本身不出行)")

    # 同一局两份图证物（file_id 1、2），都用同一 seed 但加盐区分
    for file_id in (1, 2):
        # 放大到 256~360，使 PNG 跨多个 16 KiB 逻辑分块，真正验证切块摘要
        w = PRNG(seed + bytes([file_id])).randint(256, 360)
        h = PRNG(seed + bytes([file_id])).randint(256, 360)
        px = generate_pixels(seed + bytes([file_id]), w, h)
        data = make_png(f"seed_img_{file_id}.png", w, h, px)
        file_sha, piece_count, piece_hashes = digests(data)
        print(f"\n[file_{file_id}] {w}x{h}  png={len(data)} 字节  piece_count={piece_count}")
        print(f"  整文件SHA256 = {file_sha}")
        print(f"  分块SHA256[0] = {piece_hashes[0]}")
        print(f"  分块SHA256[-1]= {piece_hashes[-1]}")
        assert len(data) <= 512 * 1024, "单文件必须 <= 512 KiB（约定第 4 节）"


if __name__ == "__main__":
    main()
