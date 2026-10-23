#!/usr/bin/env python3
"""实验 8/9 统计生图：解析 TRANSFER_STATS 日志 -> PNG 柱状图 + JSON。

口径（对应 src/transfer/transfer.c 的 tr_summary 输出）：
  tcp_up / udp_up / relay_up   各路径上行证物字节（应用层，不含协议头）
  total_up                     = tcp_up+udp_up+relay_up（实验 8：本节点上传总量）
  relay_up                     中继转发证物字节（实验 9：单独统计，不与实验 8 混列）
  elapsed                      完成时间（下载端：开始下载到三文件收齐；房主无下载为 -1）
  hash_reject                  校验拒绝块数（实验 8 的“校验结果”）
  from_1..from_4               从各 peer 收到的证物字节

用法：
  python3 tests/plot_stats.py --logs logs/host_run.log logs/detective1.log --out stats.png
  python3 tests/plot_stats.py --dir logs --out stats.png [--json stats.json]
也可作为模块： from plot_stats import parse_stats, summarize, render_round, render_compare
"""
import argparse
import glob
import json
import os
import re
import sys

STATS_RE = re.compile(r"TRANSFER_STATS\s+(.*)")
_HOST_ID = 1  # 房主先 join，恒为 peer 1（见 bench_exp8.py 启动顺序）


def parse_stats(text):
    """从一段日志文本里取出每个 peer 的最后一次 TRANSFER_STATS。

    返回 {peer_id(int): {field(str): float}}，无数据则为 {}。
    """
    out = {}
    for m in STATS_RE.finditer(text):
        d = {}
        for kv in m.group(1).split():
            if "=" in kv:
                k, v = kv.split("=", 1)
                try:
                    d[k] = float(v)
                except ValueError:
                    d[k] = v
        peer = int(d.pop("peer", 0))
        if 1 <= peer <= 4:
            out[peer] = d
    return out


def summarize(stats):
    """把各 peer 的原始字段归并成实验 8/9 口径。

    返回 dict：host_up(房主上传字节) / peer_up(其他节点上传字节) /
              relay_bytes(中继证物字节, 实验9) / elapsed_max(完成时间, 取下载端最大) /
              hash_reject(校验拒绝总数) / peers(原始按 peer 分列)。
    """
    host_up = stats.get(_HOST_ID, {}).get("total_up", 0.0)
    peer_up = 0.0
    relay_bytes = 0.0
    hash_reject = 0.0
    elapsed_max = -1.0
    for pid, d in stats.items():
        relay_bytes += d.get("relay_up", 0.0)
        hash_reject += d.get("hash_reject", 0.0)
        if pid != _HOST_ID:
            peer_up += d.get("total_up", 0.0)
        el = d.get("elapsed", -1.0)
        if el > elapsed_max:
            elapsed_max = el
    return {
        "host_up": host_up,
        "peer_up": peer_up,
        "relay_bytes": relay_bytes,
        "elapsed": elapsed_max,
        "hash_reject": hash_reject,
        "peers": stats,
    }


def _fig(rows, cols):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, axes = plt.subplots(rows, cols, figsize=(cols * 6.0, rows * 4.2),
                             squeeze=False)
    return plt, fig, axes


def render_round(summary, out_png, title="Transfer stats"):
    """画单轮（一次采集）的统计图：各 peer 上传构成 + 房主/其他节点总量。"""
    stats = summary["peers"]
    ids = sorted(stats)
    tcp = [stats[i].get("tcp_up", 0.0) for i in ids]
    udp = [stats[i].get("udp_up", 0.0) for i in ids]
    relay = [stats[i].get("relay_up", 0.0) for i in ids]
    total = [stats[i].get("total_up", 0.0) for i in ids]

    try:
        plt, fig, axes = _fig(1, 2)
        ax1, ax2 = axes[0][0], axes[0][1]
        x = range(len(ids))
        labels = [f"peer {i}" + (" (host)" if i == _HOST_ID else "") for i in ids]
        ax1.bar(x, tcp, label="tcp_up", color="#4C8CBF")
        ax1.bar(x, udp, bottom=tcp, label="udp_up", color="#7FB3D5")
        ax1.bar(x, relay, bottom=[t + u for t, u in zip(tcp, udp)],
                label="relay_up (exp9)", color="#E67E22")
        ax1.set_xticks(list(x))
        ax1.set_xticklabels(labels)
        ax1.set_ylabel("evidence bytes")
        ax1.set_title("Per-peer upload by path")
        ax1.legend()

        host = summary["host_up"]
        peer = summary["peer_up"]
        ax2.bar(["host upload", "other peers upload"], [host, peer],
                color=["#2E86C1", "#58D68D"])
        ax2.set_ylabel("evidence bytes")
        ax2.set_title("Experiment 8: host vs other peers (total_up)")
        for i, v in enumerate([host, peer]):
            ax2.text(i, v, f"{v:,.0f}", ha="center", va="bottom")

        plt.suptitle(title)
        plt.tight_layout(rect=(0, 0, 1, 0.95))
        plt.savefig(out_png, dpi=120)
        plt.close()
        return True
    except Exception as e:  # 无 matplotlib 或后端失败 -> 降级 CSV
        csv = out_png.rsplit(".", 1)[0] + ".csv"
        with open(csv, "w") as f:
            f.write("peer,tcp_up,udp_up,relay_up,total_up\n")
            for i in ids:
                f.write(f"{i},{stats[i].get('tcp_up',0)},{stats[i].get('udp_up',0)},"
                        f"{stats[i].get('relay_up',0)},{stats[i].get('total_up',0)}\n")
        print(f"matplotlib 不可用({e})，已降级写出 {csv}", file=sys.stderr)
        return False


def render_compare(groups, out_png, title="Experiment 8: C/S vs P2P"):
    """画两组对照（如中心分发 vs 玩家互传）的房主/其他节点上传柱状图。

    groups: [{"label": str, "host_up": float, "peer_up": float, "relay_bytes": float}, ...]
    """
    try:
        plt, fig, axes = _fig(1, 1)
        ax = axes[0][0]
        n = len(groups)
        x = range(n)
        hosts = [g["host_up"] for g in groups]
        peers = [g["peer_up"] for g in groups]
        relays = [g["relay_bytes"] for g in groups]
        labels = [g["label"] for g in groups]
        w = 0.28
        ax.bar([i - w for i in x], hosts, w, label="host upload (exp8)", color="#2E86C1")
        ax.bar(x, peers, w, label="other peers upload (exp8)", color="#58D68D")
        ax.bar([i + w for i in x], relays, w, label="relay bytes (exp9)", color="#E67E22")
        ax.set_xticks(list(x))
        ax.set_xticklabels(labels)
        ax.set_ylabel("evidence bytes")
        ax.set_title(title)
        ax.legend()
        for i in x:
            for v, xx in ((hosts[i], i - w), (peers[i], i), (relays[i], i + w)):
                ax.text(xx, v, f"{v:,.0f}", ha="center", va="bottom", fontsize=8)
        plt.tight_layout()
        plt.savefig(out_png, dpi=120)
        plt.close()
        return True
    except Exception as e:
        print(f"matplotlib 不可用({e})，仅输出 JSON/文本。", file=sys.stderr)
        return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--logs", nargs="*", help="指定日志文件（可多个）")
    ap.add_argument("--dir", help="扫描该目录下所有 *.log")
    ap.add_argument("--out", default="stats.png")
    ap.add_argument("--json", help="额外写出归并后的 JSON")
    ap.add_argument("--title", default="Transfer stats")
    args = ap.parse_args()

    files = list(args.logs or [])
    if args.dir:
        files += sorted(glob.glob(os.path.join(args.dir, "*.log")))

    merged = {}
    for fp in files:
        try:
            with open(fp, encoding="utf-8", errors="replace") as f:
                for pid, d in parse_stats(f.read()).items():
                    merged[pid] = d  # 后者覆盖，保留最后一次
        except OSError as e:
            print(f"skip {fp}: {e}", file=sys.stderr)

    if not merged:
        print("未找到任何 TRANSFER_STATS 行。", file=sys.stderr)
        return 1

    s = summarize(merged)
    print(f"host_up={s['host_up']:.0f} peer_up={s['peer_up']:.0f} "
          f"relay={s['relay_bytes']:.0f} elapsed={s['elapsed']:.3f} "
          f"hash_reject={s['hash_reject']:.0f}")
    if args.json:
        with open(args.json, "w") as f:
            json.dump({k: (v if k != "peers" else {str(p): d for p, d in v.items()})
                       for k, v in s.items()}, f, indent=2)
    render_round(s, args.out, args.title)
    return 0


if __name__ == "__main__":
    sys.exit(main())
