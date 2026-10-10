#!/usr/bin/env python3
"""碎片档案 · 轻量案卷生成器（陈提供，供 transfer 联调）。

不依赖 Pillow：证物内容用种子的确定性字节流填充。网络实验只关心字节与
SHA-256，不关心图长什么样。产出 session_<id>/file_<id> + manifest.bin，
manifest.bin 严格对齐 v1 约定 §5.4（= 线上 MANIFEST 正文），与
test_code/case_common.py 的 build_manifest 输出字节一致。

用法：
    python3 tools/gen_case.py [seed_hex] [base_dir]
无参数则随机生成种子（>= 16 字节）。
"""
import hashlib
import os
import struct
import sys

PIECE = 16384


def piece_digests(data):
    n = (len(data) + PIECE - 1) // PIECE
    return [hashlib.sha256(data[i * PIECE:(i + 1) * PIECE]).digest() for i in range(n)]


def build_manifest(session_id, seed_hash, files):
    body = struct.pack(">I32sB", session_id, seed_hash, len(files))
    for fid, chapter, kind, data in files:
        shas = piece_digests(data)
        body += struct.pack(">BBBI32sH", fid, chapter, kind, len(data),
                            hashlib.sha256(data).digest(), len(shas))
        body += b"".join(shas)
    return body


def gen_bytes(seed, fid, size):
    """用种子派生确定性填充，保证同种子同内容。"""
    out, i = b"", 0
    while len(out) < size:
        out += hashlib.sha256(seed + bytes([fid]) + i.to_bytes(2, "big")).digest()
        i += 1
    return out[:size]


def main():
    if len(sys.argv) > 1:
        seed = bytes.fromhex(sys.argv[1])
    else:
        seed = os.urandom(32)
    base = sys.argv[2] if len(sys.argv) > 2 else "."
    if len(seed) < 16:
        sys.exit("种子需 >= 16 字节")

    sid = int.from_bytes(hashlib.sha256(b"sid" + seed).digest()[:4], "big")
    files = [
        (1, 1, 1, gen_bytes(seed, 1, 96 * 1024)),   # 现场勘查图（图）
        (2, 1, 1, gen_bytes(seed, 2, 71 * 1024)),   # 证物登记卡（图）
        (3, 2, 2, gen_bytes(seed, 3, 560)),         # 案卷正文（文本）
    ]
    sdir = os.path.join(base, f"session_{sid:08x}")
    os.makedirs(sdir, exist_ok=True)
    for fid, _, _, data in files:
        with open(os.path.join(sdir, f"file_{fid}"), "wb") as f:
            f.write(data)
    manifest = build_manifest(sid, hashlib.sha256(seed).digest(), files)
    with open(os.path.join(sdir, "manifest.bin"), "wb") as f:
        f.write(manifest)
    print(f"session_{sid:08x}  files={[len(d) for _, _, _, d in files]} "
          f"manifest={len(manifest)}B")


if __name__ == "__main__":
    main()
