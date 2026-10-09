"""碎片档案 C/S demo —— 一键基准脚本（只传文件，不答题）。

设计目的：
  - 进程内启动一个服务端线程（监听 127.0.0.1 临时端口），与客户端走真实 TCP 环回。
  - 客户端“无交互”地把三份证物（两图一文）全部下载下来，不做章间问答 / 三项择一。
  - 用“计量套接字”逐字节统计真实网络用量（线上字节），并拆解协议开销。
  - 跑若干轮（默认 5 轮，每轮随机种子），输出汇总表并画出统计图。

用法：
    python run_bench.py            # 跑 5 轮，出图
    python run_bench.py -n 10     # 跑 10 轮
    python run_bench.py --seed <hex>   # 固定种子只跑 1 轮（复现）
    python run_bench.py --no-chart # 只出表不出图

输出：
    demo_out/bench_stats.csv      汇总数据
    demo_out/bench_report.png     统计图
"""
import argparse
import csv
import hashlib
import os
import socket
import struct
import threading
import time

import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

# Windows 上让图里的中文正常显示（方框 -> 微软雅黑）；其他平台回退到 DejaVu
plt.rcParams["font.family"] = ["Microsoft YaHei", "SimHei", "DejaVu Sans"]
plt.rcParams["axes.unicode_minus"] = False

from case_common import (CMD_ERROR, CMD_MANIFEST, CMD_MANIFEST_REQ, CMD_PIECE_DATA,
                         CMD_PIECE_REQ, HDR, PIECE, gen_case, parse_manifest,
                         recv_frame, send_frame)
from case_server import handle_client

OUT_DIR = "demo_out"
PIECE_HDR = struct.Struct(">BHH")   # file_id(1) piece_index(2) data_len(2) = 5 字节


class MeteredSocket:
    """包一层真实 socket，逐字节累加收发量，得到“真实的网络用量”。"""

    def __init__(self, sock, counter):
        self.sock = sock
        self.c = counter  # {"tx": 0, "rx": 0}

    def sendall(self, data):
        self.c["tx"] += len(data)
        return self.sock.sendall(data)

    def recv(self, n):
        chunk = self.sock.recv(n)
        if chunk:
            self.c["rx"] += len(chunk)
        return chunk

    def close(self):
        return self.sock.close()

    def setsockopt(self, *a, **k):
        return self.sock.setsockopt(*a, **k)


def serve_one(case, ready, port_holder, counter):
    """服务端线程：只接受一次连接，用计量套接字包起来交给 handle_client。"""
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", 0))          # 临时端口，避免和手动起的 demo 抢 38001
    srv.listen(1)
    port_holder.append(srv.getsockname()[1])
    ready.set()
    conn, addr = srv.accept()
    srv.close()
    mconn = MeteredSocket(conn, counter)
    try:
        handle_client(mconn, addr, case)
    finally:
        mconn.close()


def bench_pull(meter, frames):
    """不答题，只把三份证物全下载下来并逐块校验。返回 (per_file, pieces, elapsed)。"""
    t0 = time.perf_counter()

    # 1) 索取清单
    send_frame(meter, CMD_MANIFEST_REQ, 1)
    frames["tx"] += 1
    cmd, _, body = recv_frame(meter)
    frames["rx"] += 1
    if cmd != CMD_MANIFEST:
        raise RuntimeError("未取得清单")
    man = parse_manifest(body)
    by_id = {f["file_id"]: f for f in man["files"]}

    # 2) 逐文件、逐块下载并校验（与 case_client.download_file 同一套校验逻辑）
    per_file = {}
    total_pieces = 0
    for f in man["files"]:
        fid = f["file_id"]
        n = len(f["pieces"])
        got = b""
        for idx in range(n):
            send_frame(meter, CMD_PIECE_REQ, idx + 1, struct.pack(">BH", fid, idx))
            frames["tx"] += 1
            frame = recv_frame(meter)
            frames["rx"] += 1
            if frame is None:
                raise ConnectionError("服务器关闭")
            cmd2, _, pbody = frame
            if cmd2 == CMD_ERROR:
                raise RuntimeError("服务器回 ERROR")
            if cmd2 != CMD_PIECE_DATA:
                raise RuntimeError(f"意外指令 0x{cmd2:02X}")
            pfid, pidx, dlen = PIECE_HDR.unpack_from(pbody, 0)
            piece = pbody[PIECE_HDR.size: PIECE_HDR.size + dlen]
            if hashlib.sha256(piece).digest() != f["pieces"][pidx]:
                raise RuntimeError(f"file_{fid} 第 {idx} 块摘要不符")
            got += piece
        if hashlib.sha256(got).digest() != f["sha"]:
            raise RuntimeError(f"file_{fid} 整文件摘要不符")
        per_file[fid] = len(got)
        total_pieces += n

    return per_file, total_pieces, time.perf_counter() - t0


def run_round(seed):
    """跑一轮完整的“清单 + 全文件下载”，返回各项指标（字典）。"""
    case = gen_case(seed)

    srv_counter = {"tx": 0, "rx": 0}
    cli_counter = {"tx": 0, "rx": 0}
    frames = {"tx": 0, "rx": 0}

    ready = threading.Event()
    port_holder = []
    t = threading.Thread(target=serve_one, args=(case, ready, port_holder, srv_counter),
                         daemon=True)
    t.start()
    ready.wait()
    port = port_holder[0]

    s = socket.create_connection(("127.0.0.1", port))
    meter = MeteredSocket(s, cli_counter)
    per_file, pieces, elapsed = bench_pull(meter, frames)
    s.close()
    t.join(timeout=5)

    # ---- 指标结算 ----
    app_bytes = sum(per_file.values())          # 应用层证物字节（真正的文件内容）
    wire_rx = cli_counter["rx"]                  # 客户端收到的真实网络字节
    wire_tx = cli_counter["tx"]                  # 客户端发出的真实网络字节
    rx_frames = frames["rx"]                     # 客户端收帧数（1 清单 + N 数据块）
    tx_frames = frames["tx"]                     # 客户端发帧数（1 请求 + N 块请求）

    manifest_body = len(case["manifest"])        # 清单正文（不含文件数据，只含摘要/元数据）
    frame_hdr = 8 * rx_frames                   # 8 字节帧头总量
    piece_hdr = 5 * pieces                       # 块头(5B)总量
    overhead = wire_rx - app_bytes               # 协议开销（头 + 清单）

    # 一致性自检：帧头 + 清单元数据 + 块头 + 文件数据 应等于线上总字节
    assert abs((frame_hdr + manifest_body + piece_hdr + app_bytes) - wire_rx) <= 8, \
        "开销拆解与线上字节对不上"

    return {
        "app_bytes": app_bytes, "wire_rx": wire_rx, "wire_tx": wire_tx,
        "rx_frames": rx_frames, "tx_frames": tx_frames, "pieces": pieces,
        "manifest_body": manifest_body, "frame_hdr": frame_hdr,
        "piece_hdr": piece_hdr, "overhead": overhead, "elapsed": elapsed,
        "per_file": per_file,
    }


def fmt_kb(b):
    return f"{b / 1024:.2f}"


def main():
    ap = argparse.ArgumentParser(description="碎片档案 一键基准（只传文件，不答题）")
    ap.add_argument("-n", type=int, default=5, help="轮数（每轮随机种子），默认 5")
    ap.add_argument("--no-chart", action="store_true", help="只出表不出图")
    ap.add_argument("--seed", help="固定种子 hex（>=16 字节），仅跑 1 轮复现")
    args = ap.parse_args()
    os.makedirs(OUT_DIR, exist_ok=True)

    if args.seed:
        seed = bytes.fromhex(args.seed)
        if len(seed) < 16:
            raise SystemExit("种子需 >= 16 字节")
        rounds = [run_round(seed)]
    else:
        rounds = [run_round(os.urandom(32)) for _ in range(args.n)]

    n = len(rounds)
    fids = sorted(rounds[0]["per_file"].keys())
    csv_path = os.path.join(OUT_DIR, "bench_stats.csv")
    with open(csv_path, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["round", "app_bytes", "wire_rx", "wire_tx", "overhead",
                    "rx_frames", "tx_frames", "pieces", "manifest_body",
                    "frame_hdr", "piece_hdr", "elapsed_ms"] + [f"file_{i}_bytes" for i in fids])
        for i, r in enumerate(rounds, 1):
            w.writerow([i, r["app_bytes"], r["wire_rx"], r["wire_tx"], r["overhead"],
                        r["rx_frames"], r["tx_frames"], r["pieces"], r["manifest_body"],
                        r["frame_hdr"], r["piece_hdr"], f"{r['elapsed'] * 1000:.2f}"]
                       + [r["per_file"][i] for i in fids])

    # ---- 汇总（取均值）----
    avg = lambda k: sum(r[k] for r in rounds) / n
    a_app = avg("app_bytes")
    a_wire = avg("wire_rx")
    a_tx = avg("wire_tx")
    a_ovh = avg("overhead")
    a_frames = avg("rx_frames")
    a_pieces = avg("pieces")
    a_man = avg("manifest_body")
    a_fh = avg("frame_hdr")
    a_ph = avg("piece_hdr")
    a_el = avg("elapsed")
    avg_per = {i: sum(r["per_file"][i] for r in rounds) / n for i in fids}
    thru = a_wire / a_el / 1024 if a_el else 0     # KB/s（线上吞吐）
    thru_app = a_app / a_el / 1024 if a_el else 0  # KB/s（应用层吞吐）

    # ---- 控制台汇总表 ----
    print(f"\n=== 碎片档案 传输基准（{n} 轮，只传文件不答题）===")
    print(f"{'指标':<22}{'均值':>14}{'  说明'}")
    print(f"{'应用层证物字节':<20}{fmt_kb(a_app)+' KB':>14}   两图一文真实内容")
    print(f"{'线上接收字节':<20}{fmt_kb(a_wire)+' KB':>14}   真实网络用量（含头/清单）")
    print(f"{'线上发送字节':<20}{fmt_kb(a_tx)+' KB':>14}   清单请求+块请求")
    print(f"{'协议开销':<20}{fmt_kb(a_ovh)+' KB':>14}   = 线上 - 应用层")
    print(f"{'  其中 8字节帧头':<20}{fmt_kb(a_fh)+' KB':>14}   {a_frames:.0f} 帧")
    print(f"{'  其中 5字节块头':<20}{fmt_kb(a_ph)+' KB':>14}   {a_pieces:.0f} 块")
    print(f"{'  其中清单元数据':<20}{fmt_kb(a_man)+' KB':>14}   摘要/局号/种子哈希")
    print(f"{'传输耗时':<20}{a_el*1000:>12.2f} ms   单轮清单+全文件")
    print(f"{'线上吞吐':<20}{thru:>12.2f} KB/s")
    print(f"{'应用层吞吐':<20}{thru_app:>12.2f} KB/s")
    for i in fids:
        print(f"{f'  file_{i} 体积':<20}{fmt_kb(avg_per[i])+' KB':>14}")
    print(f"\n明细已写入：{csv_path}")

    if args.no_chart:
        return

    # ---- 统计图 ----
    png_path = os.path.join(OUT_DIR, "bench_report.png")
    fig, ax = plt.subplots(2, 2, figsize=(12, 9))
    fig.suptitle("碎片档案 · 传输基准（只传文件，不答题）", fontsize=14, fontweight="bold")

    # (0,0) 各轮线上总字节
    xs = list(range(1, n + 1))
    ax[0, 0].bar(xs, [r["wire_rx"] / 1024 for r in rounds], color="#4C72B0")
    ax[0, 0].axhline(a_wire / 1024, color="red", ls="--", lw=1, label=f"均值 {fmt_kb(a_wire)} KB")
    ax[0, 0].set_title("各轮次 · 线上接收总字节")
    ax[0, 0].set_xlabel("轮次"); ax[0, 0].set_ylabel("KB")
    ax[0, 0].legend(fontsize=8)

    # (0,1) 协议开销构成（饼图：应用数据 vs 各类开销）
    labels = ["文件数据", "8字节帧头", "5字节块头", "清单元数据"]
    vals = [a_app, a_fh, a_ph, a_man]
    cols = ["#55A868", "#C44E52", "#8172B3", "#CCB974"]
    ax[0, 1].pie(vals, labels=[f"{l}\n{fmt_kb(v)}KB" for l, v in zip(labels, vals)],
                 colors=cols, autopct="%1.1f%%", startangle=90,
                 wedgeprops=dict(width=0.45))
    ax[0, 1].set_title("单轮线上字节构成（均值）")

    # (1,0) 三个文件应用层体积
    ax[1, 0].bar([f"file_{i}" for i in fids], [avg_per[i] / 1024 for i in fids],
                 color=["#4C72B0", "#DD8452", "#55A868"])
    ax[1, 0].set_title("各文件应用层体积（均值）")
    ax[1, 0].set_ylabel("KB")

    # (1,1) 吞吐与耗时
    bars = ax[1, 1].bar(["线上吞吐", "应用层吞吐"], [thru, thru_app], color=["#4C72B0", "#55A868"])
    ax[1, 1].set_title("吞吐与耗时")
    ax[1, 1].set_ylabel("KB/s")
    for b, v in zip(bars, [thru, thru_app]):
        ax[1, 1].text(b.get_x() + b.get_width()/2, v, f"{v:.1f}", ha="center", va="bottom", fontsize=9)
    ax[1, 1].text(0.5, -0.18, f"单轮传输耗时 {a_el*1000:.2f} ms", transform=ax[1, 1].transAxes,
                  ha="center", fontsize=9, color="#555")

    fig.tight_layout(rect=[0, 0, 1, 0.96])
    fig.savefig(png_path, dpi=120)
    print(f"统计图已保存：{png_path}")


if __name__ == "__main__":
    main()
