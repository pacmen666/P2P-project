#!/usr/bin/env python3
"""碎片档案 · manifest 哈希一致性校验（陈提供，供 transfer 联调）。

独立从磁盘重算每个文件的整文件 SHA-256 与每个逻辑分块的 SHA-256，
与 manifest.bin（v1 §5.4）里记录的摘要逐一比对。transfer 收块后正是
按 manifest.pieces[].hash 校验，所以这一致性是传送层正确性的前提。

用法：
    python3 tests/check_manifest.py <session_dir>
退出码：0=PASS，1=FAIL（便于 CI）。
"""
import hashlib
import struct
import sys
import os

PIECE = 16384


def main():
    if len(sys.argv) < 2:
        print("用法: python3 tests/check_manifest.py <session_dir>")
        return 2
    sd = sys.argv[1]
    mb = open(os.path.join(sd, "manifest.bin"), "rb").read()
    sid, sh, fn = struct.unpack_from(">I32sB", mb, 0)
    print("session_id=%08x file_n=%d" % (sid, fn))
    off = 37
    ok = True
    for _ in range(fn):
        fid, ch, kind, fsz, fsha, pc = struct.unpack_from(">BBBI32sH", mb, off)
        off += 41
        data = open(os.path.join(sd, "file_%d" % fid), "rb").read()
        if hashlib.sha256(data).digest() != fsha:
            ok = False
            print("  FILE HASH MISMATCH fid=%d" % fid)
        n = (len(data) + PIECE - 1) // PIECE
        if n != pc:
            ok = False
            print("  piece_count mismatch fid=%d: %d != %d" % (fid, n, pc))
        for i in range(pc):
            h = mb[off:off + 32]
            off += 32
            exp = hashlib.sha256(data[i * PIECE:(i + 1) * PIECE]).digest()
            if h != exp:
                ok = False
                print("  PIECE HASH MISMATCH fid=%d piece=%d" % (fid, i))
    print("HASH-CONSISTENCY:", "PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
