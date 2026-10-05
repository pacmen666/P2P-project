"""碎片档案 C/S demo —— 服务器（房主侧）。

用法：
    python case_server.py                     # 随机种子开一局
    python case_server.py --seed <hex>       # 固定种子复现（仅实验用）

只监听 127.0.0.1，端口默认 38001。种子只留进程内存，不打印、不落盘。
统计：只计 PIECE_DATA 数据字节（对应实验 8 的 cs_host_up 口径）。
"""
import argparse
import socket
import threading

from case_common import (CMD_ANSWER, CMD_ERROR, CMD_MANIFEST, CMD_MANIFEST_REQ,
                         CMD_PIECE_DATA, CMD_PIECE_REQ, ERR_BAD_CMD, ERR_NO_PIECE,
                         PIECE, gen_case, recv_frame, send_frame, set_trace)
import os
import struct

stats_lock = threading.Lock()
stats = {"up": 0, "served": 0}  # 应用层证物字节（实验 8 口径）


def handle_client(conn, addr, case):
    global stats
    print(f"[连接] {addr}")
    try:
        while True:
            frame = recv_frame(conn)
            if frame is None:
                break
            cmd, req_id, body = frame

            if cmd == CMD_MANIFEST_REQ:
                send_frame(conn, CMD_MANIFEST, req_id, case["manifest"])

            elif cmd == CMD_PIECE_REQ:
                if len(body) != 3:
                    send_frame(conn, CMD_ERROR, req_id, struct.pack(">H", ERR_BAD_CMD))
                    continue
                fid, idx = struct.unpack(">BH", body)
                data = case["files"].get(fid)
                piece = data[idx * PIECE:(idx + 1) * PIECE] if data else b""
                if not piece:
                    send_frame(conn, CMD_ERROR, req_id, struct.pack(">H", ERR_NO_PIECE))
                    continue
                payload = struct.pack(">BHH", fid, idx, len(piece)) + piece
                send_frame(conn, CMD_PIECE_DATA, req_id, payload)
                with stats_lock:
                    stats["up"] += len(piece)
                    stats["served"] += 1

            elif cmd == CMD_ANSWER:
                choice = body[0]
                ok = 1 if choice - 1 == case["murderer"] else 0
                send_frame(conn, 0xA1, req_id, bytes([ok]))
                verdict = "正确" if ok else "错误"
                print(f"[裁决] {addr} 指认 {choice} 号 -> {verdict}")

            else:
                send_frame(conn, CMD_ERROR, req_id, struct.pack(">H", ERR_BAD_CMD))
    except (ConnectionError, ValueError) as e:
        print(f"[断开] {addr}: {e}")
    finally:
        conn.close()
        print(f"[离开] {addr}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=38001)
    ap.add_argument("--seed", help="固定种子（hex，>=16 字节），仅实验用")
    ap.add_argument("--trace", action="store_true", help="打印每帧的字段结构")
    args = ap.parse_args()
    set_trace(args.trace)

    seed = bytes.fromhex(args.seed) if args.seed else os.urandom(32)
    if len(seed) < 16:
        raise SystemExit("种子需 >= 16 字节")

    case = gen_case(seed)
    print(f"本局已生成：session_id = {case['session_id']:#010x}，"
          f"seed_hash = {case['seed_hash'].hex()[:16]}...（种子不出进程）")
    for fid, data in case["files"].items():
        print(f"  file_{fid}: {len(data)} 字节, {case['files'][fid] and (len(data)+PIECE-1)//PIECE} 块")

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.host, args.port))
    srv.listen(8)
    print(f"房间服务监听 {args.host}:{args.port}，Ctrl+C 结束")

    try:
        while True:
            conn, addr = srv.accept()
            threading.Thread(target=handle_client, args=(conn, addr, case), daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        srv.close()
        with stats_lock:
            print(f"\n[统计] 证物数据字节累计上行 {stats['up']} 字节，共送出 {stats['served']} 块（cs_host_up 口径）")


if __name__ == "__main__":
    main()
