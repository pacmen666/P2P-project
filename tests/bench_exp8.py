#!/usr/bin/env python3
"""一键实验 8：固定一份案卷，中心分发(C/S) vs 玩家互传(P2P) 上传字节对照 + 统计生图。

用法：
  python3 tests/bench_exp8.py                    # 随机生成一份案卷，跑两轮并生图
  python3 tests/bench_exp8.py --session session_xxx   # 用固定案卷（如 AI 生图局）
  python3 tests/bench_exp8.py --seconds 16 --out logs/exp8

口径（与 src/transfer/transfer.c 的 tr_summary 一致）：
  房主上传字节    = peer 1 的 total_up（tcp+udp+relay 上行证物字节）
  其他节点上传字节 = peer 2/3 的 total_up 之和
  源节点上传减少比例 = 1 - (P2P 房主上传 / C/S 房主上传)；可为 0 或负，据实报告
  中继字节(实验9)  = relay_up，单独统计，不并入实验 8 口径
  完成时间        = 下载端 elapsed（开始下载到三文件收齐），房主为 -1
  校验结果        = hash_reject（校验拒绝块数，正常为 0）
"""
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
import plot_stats  # noqa: E402


def text(path):
    p = Path(path)
    return p.read_text(errors="replace") if p.exists() else ""


def wait_for(pred, label, timeout=15):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if pred():
            return
        time.sleep(0.04)
    raise AssertionError("timeout: " + label)


class Round:
    def __init__(self, session, cs, seconds):
        self.base = Path(tempfile.mkdtemp(prefix="fragment-exp8-"))
        (self.base / "logs").mkdir(parents=True, exist_ok=True)
        self.ps = {}
        self.handles = []
        self.session = session
        self.cs = cs
        self.seconds = seconds

    def start(self, name, cmd):
        f = open(self.base / (name + ".log"), "w")
        self.handles.append(f)
        p = subprocess.Popen(cmd, cwd=self.base, stdin=subprocess.PIPE,
                             stdout=f, stderr=subprocess.STDOUT, text=True)
        self.ps[name] = p
        return p

    def log(self, name):
        return text(self.base / (name + ".log"))

    def run(self):
        self.start("room", [str(ROOT / "room_server")])
        wait_for(lambda: "UDP 监听" in self.log("room"), "room ready")

        # 房主先 join 保证 peer=1；再依次起 A、B（peer=2、3）
        self.start("host", [str(ROOT / "client_transfer"), "--host", "--session",
                            str(self.session), "--room", "127.0.0.1", "--nick", "host",
                            "--run-seconds", str(self.seconds + 3)])
        wait_for(lambda: "JOINED" in self.log("host"), "host joined")
        for name in ("A", "B"):
            cmd = [str(ROOT / "client_transfer"), "--room", "127.0.0.1", "--nick", name,
                   "--auto", "--data-dir", str(self.base / ("cache_" + name)),
                   "--run-seconds", str(self.seconds)]
            if self.cs:
                cmd.append("--cs")
            self.start(name, cmd)
            wait_for(lambda: "JOINED" in self.log(name), name + " joined")

        # 只等三个 client 自然走到 --run-seconds 到点并打印 TRANSFER_STATS 后退出；
        # room_server 不会自行退出，交给 close() 终止。
        deadline = time.monotonic() + self.seconds + 20
        for name in ("host", "A", "B"):
            self.ps[name].wait(timeout=max(1, deadline - time.monotonic()))

    def stats(self):
        return {name: plot_stats.parse_stats(self.log(name))
                for name in ("host", "A", "B")}

    def close(self):
        for p in self.ps.values():
            if p.poll() is None:
                p.terminate()
        for p in self.ps.values():
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
        for f in self.handles:
            f.close()
        shutil.rmtree(self.base, ignore_errors=True)


def merge(round_stats):
    """合并三端各自视角的 TRANSFER_STATS 为一张 peer 表。"""
    merged = {}
    for name, stats in round_stats.items():
        for pid, d in stats.items():
            merged[pid] = d
    return merged


def main():
    ap = __import__("argparse").ArgumentParser()
    ap.add_argument("--session", help="固定案卷目录（默认随机生成一份）")
    ap.add_argument("--seconds", type=int, default=16, help="每轮运行秒数")
    ap.add_argument("--out", default="logs/exp8", help="输出目录")
    ap.add_argument("--keep", action="store_true", help="保留临时进程日志")
    args = ap.parse_args()

    out = ROOT / args.out
    out.mkdir(parents=True, exist_ok=True)

    # 固定案卷：给定目录，否则随机生成一份（两轮共用同一份，保证“同一案卷”）
    if args.session:
        session = Path(args.session).resolve()
        assert session.is_dir(), f"session 目录不存在: {session}"
    else:
        gen_dir = Path(tempfile.mkdtemp(prefix="fragment-exp8-gen-"))
        subprocess.run(["python3", str(ROOT / "tools/gen_case.py")],
                       cwd=gen_dir, check=True, capture_output=True)
        session = next(gen_dir.glob("session_*"))
    print(f"案卷: {session}")

    rounds = {}
    for label, cs in (("C/S", True), ("P2P", False)):
        r = Round(session, cs, args.seconds)
        r.run()
        merged = merge(r.stats())
        s = plot_stats.summarize(merged)
        rounds[label] = {"label": label, "host_up": s["host_up"],
                         "peer_up": s["peer_up"], "relay_bytes": s["relay_bytes"],
                         "elapsed": s["elapsed"], "hash_reject": s["hash_reject"],
                         "peers": merged}
        if args.keep:
            keep = out / label
            keep.mkdir(parents=True, exist_ok=True)
            for name in ("host", "A", "B"):
                shutil.copy(r.base / (name + ".log"), keep / (name + ".log"))
        r.close()
        print(f"[{label}] host_up={s['host_up']:.0f} peer_up={s['peer_up']:.0f} "
              f"relay={s['relay_bytes']:.0f} elapsed={s['elapsed']:.3f} "
              f"hash_reject={s['hash_reject']:.0f}")

    cs_h = rounds["C/S"]["host_up"]
    p2p_h = rounds["P2P"]["host_up"]
    ratio = None
    if cs_h > 0:
        ratio = 1.0 - p2p_h / cs_h
    print("\n=== 实验 8 结论 ===")
    print(f"中心分发  房主上传 {cs_h:,.0f} 字节；其他节点上传 {rounds['C/S']['peer_up']:,.0f} 字节")
    print(f"玩家互传  房主上传 {p2p_h:,.0f} 字节；其他节点上传 {rounds['P2P']['peer_up']:,.0f} 字节")
    if ratio is not None:
        print(f"源节点上传减少比例 = 1 - (互传房主上传 / 中心房主上传) = {ratio:+.2%}"
              + ("（为负即互传反而更多，据实报告）" if ratio < 0 else ""))

    # 生图
    png = out / "exp8.png"
    plot_stats.render_compare(
        [{"label": g["label"], "host_up": g["host_up"], "peer_up": g["peer_up"],
          "relay_bytes": g["relay_bytes"]} for g in rounds.values()],
        str(png), "Experiment 8: C/S vs P2P (same case, same 3 peers)")
    for label in ("C/S", "P2P"):
        plot_stats.render_round(plot_stats.summarize(rounds[label]["peers"]),
                                str(out / f"round_{label.replace('/', '_')}.png"),
                                f"Experiment 8 {label}: per-peer upload")
    with open(out / "exp8.json", "w") as f:
        json.dump({k: {kk: (vv if kk != "peers" else {str(p): d for p, d in vv.items()})
                       for kk, vv in v.items()} for k, v in rounds.items()}, f, indent=2)
    print(f"\n已生成: {png} 及 round_*.png、exp8.json")


if __name__ == "__main__":
    main()
