"""碎片档案 C/S demo —— 客户端（侦探侧）。

用法：
    python case_client.py               # 连 127.0.0.1:38001
    python case_client.py --corrupt    # 注入一次坏块，演示摘要校验拒绝重传

流程：索取清单 -> 分块下载第一章两证物（逐块 SHA-256 校验）->
章间问答（本机解锁）-> 下载第二章证物 -> 三项择一（服务器裁决）。
"""
import argparse
import hashlib
import os
import re
import socket
import struct
import sys

from case_common import (ALIBIS, CMD_ANSWER, CMD_ERROR, CMD_MANIFEST, CMD_MANIFEST_REQ,
                         CMD_PIECE_DATA, CMD_PIECE_REQ, CONTRA, PIECE, SUSPECTS,
                         recv_frame, send_frame, set_trace)
from case_common import parse_manifest

OUT_DIR = "demo_out"


def ask(prompt):
    """读一行用户输入，剥掉首尾空白与不可见字符（BOM / 零宽空格）。"""
    try:
        return input(prompt).strip().lstrip("\ufeff\u200b").strip()
    except EOFError:
        return ""


PIECE_HDR = struct.Struct(">BHH")   # fid(1) piece_index(2) data_len(2) —— 注意是 5 字节，不是 4
MAX_RETRY = 5                       # 同一块最多补缺 5 次（对齐约定「5 次改走别的路」）


def download_file(sock, minfo, fid, corrupt=False):
    """按清单逐块拉取并校验，拼回整文件后核对整文件摘要。返回文件字节。"""
    pieces = {}
    total = len(minfo["pieces"])
    for idx in range(total):
        attempt = 0
        while True:
            if attempt >= MAX_RETRY:
                raise RuntimeError(f"file_{fid} 第 {idx} 块连续 {MAX_RETRY} 次未通过校验，放弃该来源")
            send_frame(sock, CMD_PIECE_REQ, idx + 1, struct.pack(">BH", fid, idx))
            frame = recv_frame(sock)
            if frame is None:
                raise ConnectionError("服务器关闭")
            cmd, _req, body = frame
            if cmd == CMD_ERROR:
                raise RuntimeError(f"服务器回 ERROR 0x{struct.unpack('>H', body)[0]:04X}")
            if cmd != CMD_PIECE_DATA:
                raise RuntimeError(f"意外指令 0x{cmd:02X}")

            pfid, pidx, dlen = PIECE_HDR.unpack_from(body, 0)
            if pfid != fid or pidx != idx:
                raise RuntimeError(f"块标识不符：想要 file_{fid} 第 {idx} 块，收到 file_{pfid} 第 {pidx} 块")
            piece = body[PIECE_HDR.size:PIECE_HDR.size + dlen]
            if len(piece) != dlen:
                raise RuntimeError(f"块长度不符：声明 {dlen} 实际 {len(piece)}")

            # --corrupt：把 file_1 第 0 块的首字节翻转一次，演示校验拒绝
            if corrupt and fid == 1 and idx == 0 and attempt == 0:
                piece = bytes([piece[0] ^ 0xFF]) + piece[1:]
                print("  [演示] 已注入一个坏块（首字节翻转）")
                corrupt = False

            if hashlib.sha256(piece).digest() == minfo["pieces"][pidx]:
                break
            attempt += 1
            print(f"  [file_{fid}] 第 {idx} 块摘要不符，拒绝并重新请求（第 {attempt} 次）")

        pieces[idx] = piece
        done = idx + 1
        bar = "#" * done + "." * (total - done)
        print(f"  [file_{fid}] [{bar}] {done}/{total} 块")

    data = b"".join(pieces[i] for i in range(total))
    if hashlib.sha256(data).digest() != minfo["sha"]:
        raise RuntimeError(f"file_{fid} 整文件摘要不符")
    return data


def save(fid, data, kind):
    os.makedirs(OUT_DIR, exist_ok=True)
    name = os.path.join(OUT_DIR, f"file_{fid}.jpg" if kind == 1 else f"file_{fid}.txt")
    with open(name, "wb") as f:
        f.write(data)
    return name


def jpeg_comment(data):
    """读出 JPEG 的 COM 注释段（答案就藏在文件正文里，不在清单里）。"""
    if not data.startswith(b"\xff\xd8"):
        return ""
    i = 2
    while i < len(data) - 4:
        if data[i] != 0xFF:
            i += 1
            continue
        marker = data[i + 1]
        if marker in (0xD8, 0x01) or 0xD0 <= marker <= 0xD7:
            i += 2
            continue
        if marker == 0xD9 or marker == 0xDA:      # EOI / SOS：后面是压缩数据
            break
        seg_len = int.from_bytes(data[i + 2:i + 4], "big")
        if marker == 0xFE:                        # COM 注释段
            return data[i + 4:i + 2 + seg_len].decode("utf-8", "ignore")
        i += 2 + seg_len
    return ""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=38001)
    ap.add_argument("--corrupt", action="store_true", help="注入一次坏块演示校验")
    ap.add_argument("--trace", action="store_true", help="打印每帧的字段结构")
    args = ap.parse_args()
    set_trace(args.trace)

    sock = socket.create_connection((args.host, args.port))
    print(f"已连接 {args.host}:{args.port}")

    # 1) 索取清单
    send_frame(sock, CMD_MANIFEST_REQ, 1)
    cmd, _req, body = recv_frame(sock)
    if cmd != CMD_MANIFEST:
        raise SystemExit(f"未取得清单（0x{cmd:02X}）")
    man = parse_manifest(body)

    print(f"局号 session_id = {man['session_id']:#010x}   "
          f"seed_hash = {man['seed_hash'].hex()[:16]}...（无种子）")
    by_id = {f["file_id"]: f for f in man["files"]}
    for f in man["files"]:
        kindtxt = "图" if f["kind"] == 1 else "文"
        print(f"  file_{f['file_id']} 第{f['chapter']}章[{kindtxt}] "
              f"{f['size']} 字节 {len(f['pieces'])} 块 摘要={f['sha'].hex()[:16]}...")

    # 2) 第一章：两份证物（两张图）
    print("\n== 第一章：取得现场证物 ==")
    data1 = download_file(sock, by_id[1], 1, corrupt=args.corrupt)
    data2 = download_file(sock, by_id[2], 2)
    p1 = save(1, data1, by_id[1]["kind"])
    p2 = save(2, data2, by_id[2]["kind"])
    print(f"  现场勘查图已存：{p1}")
    print(f"  证物登记卡已存：{p2}")
    print("  （用看图软件打开这两张图，线索就印在上面）")
    note2 = jpeg_comment(data2)
    if note2:
        print(f"  证物卡备注：{note2}")
    print("-" * 40)

    # 3) 章间问答：本机解锁。答案藏在 file_1 的 JPEG 注释段里（也在图上印着）
    m = re.search(r"(\d{1,2}:\d{2})", jpeg_comment(data1))
    answer = m.group(1) if m else ""
    for i in range(3):
        got = ask(f"章间问答（{2 - i} 次机会）：现场勘查图上的案发时间是几点几分？")
        if got == answer:
            print(">> 回答正确，第一章解锁，进入第二章。")
            break
        print(">> 不对，答案就印在勘查图的抬头（也在该文件的注释段里）。")
    else:
        print(">> 三次未中，按约定仅本机判断，继续进入第二章。")

    # 4) 第二章：第三份证物
    print("\n== 第二章：停摆的挂钟 ==")
    data3 = download_file(sock, by_id[3], 3)
    save(3, data3, by_id[3]["kind"])
    # 界面只显示哨兵行之前的内容；判据部分不展示，只供本机判定
    cut = data3.find("[判据]".encode("utf-8"))
    story3 = (data3[:cut] if cut >= 0 else data3).decode("utf-8", "ignore")
    hidden3 = (data3[cut:] if cut >= 0 else b"").decode("utf-8", "ignore")
    print(story3)
    print("-" * 40)

    # 5) 三项择一
    # 5a) 本机推理：证物卡的物证否掉哪套不在场证明 → 说那套话的人就是凶手
    deduced = ""
    if note2 in CONTRA:
        bad = ALIBIS[CONTRA.index(note2)]
        for line in story3.splitlines():
            if line.endswith(bad):
                deduced = line.split("：")[0].strip()
                break
    # 5b) 判据：哨兵行之后，界面不展示（约定要求本机判定，不靠服务器下发答案）
    truth_m = re.search(r"真凶：(\S+)", hidden3)
    truth = truth_m.group(1) if truth_m else ""
    print(f"（本机推理：物证「{note2}」否掉了 {deduced or '？'} 的说法）")
    if deduced and truth and deduced != truth:
        print(f">> 警告：推理结果（{deduced}）与判据（{truth}）不一致，案卷内容有问题。")
    print("三项择一：谁是用蓝围巾抹去杯沿指纹的真凶？")
    for i, n in enumerate(SUSPECTS, 1):
        print(f"  {i}. {n}")
    for i in range(3):
        got = ask(f"指认（{3 - i} 次机会，输入 1-3）：")
        if got in ("1", "2", "3"):
            picked = SUSPECTS[int(got) - 1]
            local_ok = (picked == truth)
            send_frame(sock, CMD_ANSWER, 2, bytes([int(got)]))
            cmd, _req, body = recv_frame(sock)
            server_ok = (cmd == 0xA1 and body and body[0] == 1)
            print(f">> 本机判定（file_3 判据）：{'正确' if local_ok else '不是这个人'}；"
                  f"服务器核对：{'正确' if server_ok else '错误'}")
            if local_ok and server_ok:
                print(">> 两处一致，案件告破。")
                break
            if local_ok != server_ok:
                print(">> 警告：本机与服务器判定不一致，请检查案卷是否被替换。")
        else:
            print("请输入 1、2 或 3。")
    else:
        print(f">> 三次指认均错，案件未破。真凶是 {truth}。")

    sock.close()
    print("\n（本 demo 为中心分发：全部证物均取自房主，对应实验 8 的 cs_host_up 场景）")


if __name__ == "__main__":
    main()
