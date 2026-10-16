#!/usr/bin/env python3
"""test_code 内 AI 生图实验：方舟 Seedream 渲染 file_1/2，复用 case_common 的谜题与清单。

设计（档位 c 零泄漏）：
- 种子 -> case_common.gen_case(seed) 决定谜题（凶手/不在场证明/线索），只留 host 内存。
- AI 只渲染画面风格：file_1=嫌疑人肖像、file_2=案发现场；图内 "no readable text"，不出真名。
- file_3 案卷正文沿用 gen_case 文本（含终局 [判据]，本机判定）。
- 图压到 <= 512 KiB；manifest 用 case_common.build_manifest（§5.4）。

运行：
    python ai_gen_case.py [seed_hex] [--dotenv PATH] [--base DIR] [--strict]
未传 seed 随机；AI 不可用时（无 key / 调用失败）自动降级回 case_common 程序化图，保证能跑。
"""
import argparse
import base64
import json
import os
import sys
from io import BytesIO
from urllib import request as ureq

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import case_common as cc
from seed_image import PRNG

ARK_API = "https://ark.cn-beijing.volces.com/api/v3/images/generations"
MAX_IMG = 512 * 1024

# 视觉素材池：由 seed 派生，不含真名；图内无文字（避免 AI 乱写名字/穿帮）
HAIR = ["slicked black hair", "curly auburn hair", "silver combed hair", "short blonde hair", "long dark hair"]
COAT = ["a tweed coat", "a blood-red evening gown", "a navy officer uniform", "a worn leather jacket", "a velvet green robe"]
FEAT = ["a thin scar above the left brow", "round wire-frame glasses", "a faded tattoo on the neck", "a gold pocket watch chain", "a limp and a walking cane"]
ROOMS = ["a Victorian mansion study", "a dim bedroom with a four-poster bed", "a humid greenhouse", "a kitchen with a cold stove", "a grand library reading room"]


def load_dotenv(path):
    cands = []
    if path:
        cands.append(path)
    cands.append(os.path.join(os.getcwd(), ".env"))
    cands.append(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".env"))
    for c in cands:
        if c and os.path.exists(c):
            for line in open(c, encoding="utf-8"):
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, v = line.split("=", 1)
                os.environ.setdefault(k.strip(), v.strip().strip('"').strip("'"))
            return c
    return None


def compress_image(raw):
    from PIL import Image
    im = Image.open(BytesIO(raw)).convert("RGB")
    if max(im.size) > 1024:
        ratio = 1024 / max(im.size)
        im = im.resize((int(im.size[0] * ratio), int(im.size[1] * ratio)))
    buf = BytesIO()
    im.save(buf, "PNG", optimize=True)
    if len(buf.getvalue()) <= MAX_IMG:
        return buf.getvalue()
    q = 85
    while q >= 30:
        buf = BytesIO()
        im.save(buf, "JPEG", quality=q, optimize=True)
        if len(buf.getvalue()) <= MAX_IMG:
            return buf.getvalue()
        q -= 10
    return buf.getvalue()


def call_ark(model, key, prompt, size=1024, timeout=120):
    body = {"model": model, "prompt": prompt, "size": f"{size}x{size}",
            "response_format": "b64_json", "n": 1}
    data = json.dumps(body).encode("utf-8")
    req = ureq.Request(ARK_API, data=data, method="POST")
    req.add_header("Authorization", f"Bearer {key}")
    req.add_header("Content-Type", "application/json")
    with ureq.urlopen(req, timeout=timeout) as r:
        out = json.loads(r.read().decode("utf-8"))
    b64 = out.get("data", [{}])[0].get("b64_json")
    if not b64:
        raise RuntimeError("no b64_json: " + json.dumps(out, ensure_ascii=False)[:200])
    return base64.b64decode(b64)


def pick(rng, lst):
    return lst[rng.randint(0, len(lst) - 1)]


def gen_ai_images(seed, key, model, strict):
    rng = PRNG(seed)
    hair, coat, feat, room = pick(rng, HAIR), pick(rng, COAT), pick(rng, FEAT), pick(rng, ROOMS)
    p_portrait = (f"Detective noir character portrait of a suspect: {hair}, wearing {coat}, "
                  f"{feat}, head and shoulders, dramatic side lighting, dark background, "
                  f"cinematic, highly detailed. IMPORTANT: no readable text, no letters, no names, "
                  f"no numbers anywhere in the image")
    p_scene = (f"Detective noir crime scene: {room}, dimly lit, cinematic lighting, high detail, "
               f"film grain, moody atmosphere. IMPORTANT: no readable text, no letters, no names, "
               f"no numbers anywhere in the image")
    portrait = scene = None
    try:
        portrait = compress_image(call_ark(model, key, p_portrait))
    except Exception as e:
        if strict:
            raise
        sys.stderr.write(f"[warn] file_1 AI 失败，降级程序化图：{e}\n")
    try:
        scene = compress_image(call_ark(model, key, p_scene))
    except Exception as e:
        if strict:
            raise
        sys.stderr.write(f"[warn] file_2 AI 失败，降级程序化图：{e}\n")
    return portrait, scene


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("seed", nargs="?", default=None)
    ap.add_argument("--base", default=".")
    ap.add_argument("--dotenv", default=None)
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--preview", action="store_true", help="同时导出 file_1.jpg / file_2.jpg 便于肉眼查看")
    args = ap.parse_args()

    seed = bytes.fromhex(args.seed) if args.seed else os.urandom(32)
    if len(seed) < 16:
        sys.exit("种子需 >= 16 字节")

    case = cc.gen_case(seed)            # 谜题 + 程序化图（兜底）+ 文本（file_3 含 [判据]）
    sid = case["session_id"]
    f1_prog, f2_prog, f3_text = case["files"][1], case["files"][2], case["files"][3]

    load_dotenv(args.dotenv)
    key, model = os.environ.get("ARK_API_KEY"), os.environ.get("ARK_MODEL")
    if key and model:
        ai1, ai2 = gen_ai_images(seed, key, model, args.strict)
        f1 = ai1 if ai1 else f1_prog
        f2 = ai2 if ai2 else f2_prog
        mode = "ai"
    else:
        f1, f2, mode = f1_prog, f2_prog, "template(fallback)"
        sys.stderr.write("[warn] 未配置方舟 key，使用程序化图\n")

    files = [(1, 1, 1, f1), (2, 1, 1, f2), (3, 2, 2, f3_text)]
    manifest = cc.build_manifest(sid, case["seed_hash"], files)

    sdir = os.path.join(args.base, f"session_{sid:08x}")
    os.makedirs(sdir, exist_ok=True)
    for fid, data in ((1, f1), (2, f2), (3, f3_text)):
        with open(os.path.join(sdir, f"file_{fid}"), "wb") as fp:
            fp.write(data)
    with open(os.path.join(sdir, "manifest.bin"), "wb") as fp:
        fp.write(manifest)

    if args.preview:
        for fid, data in ((1, f1), (2, f2)):
            with open(os.path.join(sdir, f"file_{fid}.jpg"), "wb") as fp:
                fp.write(data)

    print(f"session_{sid:08x}  mode={mode}  "
          f"file1={len(f1)}B file2={len(f2)}B file3={len(f3_text)}B manifest={len(manifest)}B")
    print(f"[host] 真凶（仅房主可见）：{cc.SUSPECTS[case['murderer']]}  -> 见 file_3 末行 [判据]")


if __name__ == "__main__":
    main()
