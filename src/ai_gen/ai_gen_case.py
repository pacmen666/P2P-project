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
import hashlib
import json
import os
import sys
from io import BytesIO
from urllib import request as ureq

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import case_common as cc
import llm_case
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


# 类型 -> 兜底场景氛围（无 LLM 剧本时让文生图也尽量呼应所选类型）
GENRE_ROOM = {
    "民国租界公馆": "a Republican-era concession-era mansion parlor in 1930s Shanghai",
    "英伦庄园": "an English manor estate drawing room",
    "蒸汽时代的实验室": "a steampunk Victorian laboratory with brass apparatus",
    "偏远山间别墅": "a remote mountain villa on a foggy ridge",
    "南方旧宅梅雨季": "an old southern Chinese mansion in the rainy plum season",
    "邮轮头等舱": "a luxury ocean liner first-class cabin",
    "赛博朋克暗巷诊所": "a cyberpunk back-alley neon clinic",
    "雪山中的旅馆": "a snowbound mountain lodge hotel",
}


def gen_ai_images(seed, key, model, strict, p_portrait=None, p_scene=None, genre=""):
    if p_portrait is None or p_scene is None:   # 无剧本 prompt 时退回固定素材池（兜底）
        rng = PRNG(seed)
        hair, coat, feat = pick(rng, HAIR), pick(rng, COAT), pick(rng, FEAT)
        room = GENRE_ROOM.get(genre) or pick(rng, ROOMS)
        if p_portrait is None:
            p_portrait = (f"Detective noir character portrait of a suspect: {hair}, wearing {coat}, "
                          f"{feat}, head and shoulders, dramatic side lighting, dark background, "
                          f"cinematic, highly detailed. IMPORTANT: no readable text, no letters, no names, "
                          f"no numbers anywhere in the image")
        if p_scene is None:
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
    ap.add_argument("--base", default=None,
                    help="案卷输出根目录，默认 <仓库>/cases（与 tools/gen_case.py 一致）")
    ap.add_argument("--dotenv", default=None)
    ap.add_argument("--strict", action="store_true")
    ap.add_argument("--genre", default="", help="案件类型（固定风格；空则随机）")
    ap.add_argument("--preview", action="store_true", help="同时导出 file_1.jpg / file_2.jpg 便于肉眼查看")
    args = ap.parse_args()
    genre = (args.genre or "").strip()

    seed = bytes.fromhex(args.seed) if args.seed else os.urandom(32)
    if len(seed) < 16:
        sys.exit("种子需 >= 16 字节")

    load_dotenv(args.dotenv)
    key = os.environ.get("ARK_API_KEY")
    img_model = os.environ.get("ARK_MODEL")
    llm_model = os.environ.get("ARK_LLM_MODEL")

    seed_hash = hashlib.sha256(seed).digest()

    # 1) 优先走 LLM 剧本（需额外配置 ARK_LLM_MODEL，即 doubao 推理接入点）
    script = None
    if key and llm_model:
        script = llm_case.gen_case_json(seed, key, llm_model, args.strict, genre=genre)

    murderer_name = None
    f1_prog = f2_prog = None
    f3_text = None
    f1_prompt = f2_prompt = None

    if script:
        # LLM 出剧本：正文 + 文生图提示词；问答答案与真凶藏 file_3 哨兵行后
        murderer_name = script["murderer"]
        quiz_ans = (script.get("quiz_answer") or "").strip()
        f3_text = script["case_text"].encode("utf-8")
        if quiz_ans:
            f3_text += "[判据] 问答答案：".encode("utf-8") + quiz_ans.encode("utf-8") + b"\n"
        f3_text += ("[判据] 真凶：".encode("utf-8")
                    + murderer_name.encode("utf-8") + b"\n")
        f1_prompt, f2_prompt = script["file1_prompt"], script["file2_prompt"]
        # 程序化兜底图（文生图失败时用），顺便取 session_id
        case = cc.gen_case(seed)
        sid = case["session_id"]
        f1_prog, f2_prog = case["files"][1], case["files"][2]
    else:
        # 回退：完全程序化模板
        case = cc.gen_case(seed)
        sid = case["session_id"]
        f1_prog, f2_prog, f3_text = case["files"][1], case["files"][2], case["files"][3]
        murderer_name = cc.SUSPECTS[case["murderer"]]

    # 2) 文生图渲染 file_1 / file_2
    if key and img_model:
        ai1, ai2 = gen_ai_images(seed, key, img_model, args.strict,
                                 p_portrait=f1_prompt, p_scene=f2_prompt, genre=genre)
        f1 = ai1 if ai1 else f1_prog
        f2 = ai2 if ai2 else f2_prog
        mode = "llm+ai" if script else "ai"
    else:
        f1, f2 = f1_prog, f2_prog
        mode = "llm+template" if script else "template(fallback)"
        if not (key and img_model):
            sys.stderr.write("[warn] 未配置方舟 key，使用程序化图\n")

    files = [(1, 1, 1, f1), (2, 1, 1, f2), (3, 2, 2, f3_text)]
    manifest = cc.build_manifest(sid, seed_hash, files)

    base = args.base or os.path.normpath(
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "cases"))
    sdir = os.path.join(base, f"session_{sid:08x}")
    os.makedirs(sdir, exist_ok=True)
    # 原子落盘：先写 .tmp 再 os.replace，避免扫描/读取到生成中的残缺文件
    def atomic(name, data):
        tmp = os.path.join(sdir, name + ".tmp")
        with open(tmp, "wb") as fp:
            fp.write(data)
            fp.flush()
            os.fsync(fp.fileno())
        os.replace(tmp, os.path.join(sdir, name))

    atomic("file_1", f1)
    atomic("file_2", f2)
    atomic("file_3", f3_text)
    atomic("manifest.bin", manifest)

    if args.preview:
        for fid, data in ((1, f1), (2, f2)):
            with open(os.path.join(sdir, f"file_{fid}.jpg"), "wb") as fp:
                fp.write(data)

    print(f"session_{sid:08x}  mode={mode}  "
          f"file1={len(f1)}B file2={len(f2)}B file3={len(f3_text)}B manifest={len(manifest)}B")
    print(f"[host] 真凶（仅房主可见）：{murderer_name}  -> 见 file_3 末行 [判据]")


if __name__ == "__main__":
    main()
