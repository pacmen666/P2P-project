"""碎片档案 C/S demo —— 公共部分：帧格式、案件生成、清单编解码。

帧格式仿 v1 约定 8 字节头：magic(2) ver(1) cmd(1) req_id(2) body_len(2)，大端。
案件由种子当场生成：种子只留服务端进程内存；对外只有 SHA-256 摘要（MANIFEST）。
"""
import hashlib
import struct

from seed_image import PRNG, generate_pixels, png_bytes
from compose_evidence import load_parts, make_card, make_plan

MAGIC = b"FA"
VER = 1
HDR = struct.Struct(">2sBBHH")
PIECE = 16384  # 逻辑分块 16 KiB（约定第 4 节）

# 指令码：0x07/0x0A/0x0B/0x13/0x16 对齐 v1 约定第 3 节；0xA0/0xA1 为 demo 专用
CMD_MANIFEST_REQ = 0x16   # 客户端索取清单
CMD_MANIFEST = 0x07      # 清单（无种子，含 seed_hash）
CMD_PIECE_REQ = 0x0A     # 要一块：file_id(1) piece_index(2)
CMD_PIECE_DATA = 0x0B    # 给一块：file_id(1) piece_index(2) data_len(2) data
CMD_ERROR = 0x13         # err(2)
CMD_ANSWER = 0xA0        # demo 专用：三项择一 choice(1)
CMD_RESULT = 0xA1        # demo 专用：correct(1)

ERR_BAD_CMD = 0x0001
ERR_NO_PIECE = 0x0002

SUSPECTS = ["赵枢", "钱映", "孙棠"]

# 三套不在场证明，以及各自被哪条物证否掉（陈与刘约定的"推理接口"）
ALIBIS = ["「晚饭后我一直在厨房煮茶，一步都没离开过。」",
          "「我在自己房间写信，墨迹还没干。」",
          "「我在花房修剪枝条，剪刀没离过手。」"]
CONTRA = ["厨房茶炉的保温灯是关着的，炉体早已冰凉。",
          "信纸上的墨迹早已干透，压痕也是旧的。",
          "剪刀刃口干干净净，没有一点植物汁液。"]


# ---------- 字段结构追踪（--trace 打开）----------
TRACE = False
PIECE_DATA_HDR = struct.Struct(">BHH")   # file_id(1) piece_index(2) data_len(2) = 5 字节

CMD_NAME = {
    CMD_MANIFEST_REQ: "MANIFEST_REQ", CMD_MANIFEST: "MANIFEST",
    CMD_PIECE_REQ: "PIECE_REQ", CMD_PIECE_DATA: "PIECE_DATA",
    CMD_ERROR: "ERROR", CMD_ANSWER: "ANSWER", CMD_RESULT: "RESULT",
}


def set_trace(on):
    global TRACE
    TRACE = bool(on)


def explain_frame(direction, cmd, req_id, body):
    """把一帧的字段结构按偏移逐项打印出来。"""
    name = CMD_NAME.get(cmd, f"0x{cmd:02X}")
    print(f"\n{direction} {name}(0x{cmd:02X})   req_id={req_id}   body_len={len(body)}")
    print("   ┌─ 8 字节头 ──────────────────────────────────────")
    print(f"   │ 0-1  magic     = b'FA'")
    print(f"   │ 2    ver       = {VER}")
    print(f"   │ 3    cmd       = 0x{cmd:02X}  ({name})")
    print(f"   │ 4-5  req_id    = {req_id}")
    print(f"   │ 6-7  body_len  = {len(body)}")
    print(f"   └─ 正文（{len(body)} 字节）────────────────────────")

    if cmd == CMD_MANIFEST:
        m = parse_manifest(body)
        print(f"      0    4   session_id  = {m['session_id']:#010x}   (局号，随机，不是种子)")
        print(f"      4    32  seed_hash   = {m['seed_hash'].hex()[:16]}...  (SHA-256(种子)，不是种子)")
        print(f"      36   1   file_n      = {len(m['files'])}")
        off = 37
        for f in m["files"]:
            kind = "图" if f["kind"] == 1 else "文本"
            print(f"      {off:>3}  41  文件项 file_id={f['file_id']} "
                  f"chapter={f['chapter']} kind={f['kind']}({kind})")
            print(f"              +3   4  file_size    = {f['size']}")
            print(f"              +7   32 整文件SHA256 = {f['sha'].hex()[:16]}...")
            print(f"              +39  2  piece_count  = {len(f['pieces'])}")
            for i, p in enumerate(f["pieces"]):
                print(f"              +{41 + i * 32:<3} 32 块{i} SHA256   = {p.hex()[:16]}...")
            off += 41 + 32 * len(f["pieces"])

    elif cmd == CMD_MANIFEST_REQ:
        print("      （空正文：只要一份清单）")

    elif cmd == CMD_PIECE_REQ:
        fid, idx = struct.unpack(">BH", body)
        print(f"      0    1   file_id     = {fid}")
        print(f"      1    2   piece_index = {idx}")

    elif cmd == CMD_PIECE_DATA:
        fid, idx, dlen = PIECE_DATA_HDR.unpack_from(body, 0)
        data = body[PIECE_DATA_HDR.size:PIECE_DATA_HDR.size + dlen]
        print(f"      0    1   file_id     = {fid}      ← 注意：块头是 5 字节，不是 4")
        print(f"      1    2   piece_index = {idx}")
        print(f"      3    2   data_len    = {dlen}")
        print(f"      5    {dlen:<4} 块数据      = {data[:8].hex()}...（共 {dlen} 字节）")

    elif cmd == CMD_ANSWER and body:
        print(f"      0    1   choice      = {body[0]}   (三项择一)")

    elif cmd == CMD_RESULT and body:
        print(f"      0    1   correct     = {body[0]}   (1=正确)")

    elif cmd == CMD_ERROR and len(body) >= 2:
        err = struct.unpack(">H", body)[0]
        print(f"      0    2   err         = 0x{err:04X}")


# ---------- 帧收发（读状态机：先凑满 8 字节头，再按体长凑正文）----------
def send_frame(sock, cmd, req_id, body=b""):
    sock.sendall(HDR.pack(MAGIC, VER, cmd, req_id, len(body)) + body)
    if TRACE:
        explain_frame("  >>> 发送", cmd, req_id, body)


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            return None  # 对端关闭
        buf += chunk
    return buf


def recv_frame(sock):
    """返回 (cmd, req_id, body)；对端关闭返回 None；帧非法抛异常。"""
    hdr = recv_exact(sock, HDR.size)
    if hdr is None:
        return None
    magic, ver, cmd, req_id, blen = HDR.unpack(hdr)
    if magic != MAGIC or ver != VER:
        raise ValueError("帧头不符")
    body = recv_exact(sock, blen) if blen else b""
    if body is None:
        raise ConnectionError("正文未收满对端即关闭")
    if TRACE:
        explain_frame("  <<< 接收", cmd, req_id, body)
    return cmd, req_id, body


# ---------- 摘要与清单（布局仿约定 5.4）----------
def piece_digests(data):
    n = (len(data) + PIECE - 1) // PIECE
    return [hashlib.sha256(data[i * PIECE:(i + 1) * PIECE]).digest() for i in range(n)]


def build_manifest(session_id, seed_hash, files):
    """files: [(file_id, chapter, kind, data), ...] -> MANIFEST 正文字节。"""
    body = struct.pack(">I32sB", session_id, seed_hash, len(files))
    for fid, chapter, kind, data in files:
        shas = piece_digests(data)
        body += struct.pack(">BBBI32sH", fid, chapter, kind, len(data),
                            hashlib.sha256(data).digest(), len(shas))
        body += b"".join(shas)
    return body


def parse_manifest(body):
    session_id, seed_hash, file_n = struct.unpack_from(">I32sB", body, 0)
    off = 37
    files = []
    for _ in range(file_n):
        fid, chapter, kind, fsize, fsha, pcount = struct.unpack_from(">BBBI32sH", body, off)
        off += 41
        pieces = [body[off + i * 32: off + (i + 1) * 32] for i in range(pcount)]
        off += pcount * 32
        files.append({"file_id": fid, "chapter": chapter, "kind": kind,
                     "size": fsize, "sha": fsha, "pieces": pieces})
    return {"session_id": session_id, "seed_hash": seed_hash, "files": files}


# ---------- 案件生成（种子不出服务端进程）----------
_PARTS = None


def _parts():
    """零件只加载一次（AI/手绘素材，运行时零依赖）。"""
    global _PARTS
    if _PARTS is None:
        _PARTS = load_parts()
    return _PARTS


def gen_case(seed):
    """由种子生成一局：两图一文 + 真凶。返回 dict（含凶手索引，但绝不含种子）。

    三份证物（对齐方案"两图一文"与约定 file_id→章节 的映射）：
      file_1  现场勘查图（第一章，图）——章间问答答案写进 JPEG 注释段，并画在图上
      file_2  证物登记卡（第一章，图）——备注即线索
      file_3  案卷正文  （第二章，文）——末行写真凶，供本机判定
    """
    prng = PRNG(seed)
    m = prng.randint(0, 2)                      # 真凶索引
    session_id = prng.randint(0, 0xFFFFFFFF)     # 局号（随机，不是种子）

    murderer = SUSPECTS[m]
    others = [n for n in SUSPECTS if n != murderer]

    # 三套不在场证明随机分给三人（凶手不再固定说"在厨房"），
    # 证物卡上的物证恰好否掉凶手那一套——玩家必须交叉比对才推得出。
    order = [0, 1, 2]
    for i in (2, 1):                       # 洗牌
        j = prng.randint(0, i)
        order[i], order[j] = order[j], order[i]
    mi = order[0]                          # 凶手所用的那套不在场证明
    speakers = [murderer, others[0], others[1]]
    lines = [f"{speakers[k]}：{ALIBIS[order[k]]}" for k in range(3)]
    note = CONTRA[mi]

    # file_1 / file_2：由零件 + 种子合成的证物图
    plan, pinfo = make_plan(seed, _parts())
    card, cinfo = make_card(seed, _parts(), note=note)

    # file_3：第二章案卷正文。可展示部分不含答案；
    #         判据放在哨兵行之后，界面不显示，只供本机判定。
    story = (
        "《第二章：停摆的挂钟》\n"
        f"现场勘查图记录的事发时刻为 {pinfo['time']}，与书房挂钟停摆的那一刻相同。\n"
        f"证物登记卡上的关键一条：{note}\n"
        "三人的说法如下：\n"
        + "\n".join(lines) + "\n"
        "其中有一人的说法被上面的物证否掉了。被物证否掉的那个人，就是真凶。\n"
    )
    text3 = story.encode("utf-8") + "[判据] 真凶：".encode("utf-8") + murderer.encode("utf-8") + b"\n"

    files = [(1, 1, 1, plan), (2, 1, 1, card), (3, 2, 2, text3)]
    return {
        "session_id": session_id,
        "seed_hash": hashlib.sha256(seed).digest(),
        "files": {fid: data for fid, _, _, data in files},
        "manifest": build_manifest(session_id, hashlib.sha256(seed).digest(), files),
        "murderer": m,
    }
