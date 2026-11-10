#!/usr/bin/env python3
"""用方舟 chat 接口（doubao 系列）生成一桩案件的"剧本 JSON"，供 ai_gen_case 渲染成证据。

思路（与 Seedream 文生图并行，共用同一个 ARK_API_KEY）：
- 文生图负责氛围与证物画面（file_1/file_2），LLM 负责"案情、动机、时间线、供词、逻辑链"。
- LLM 一次性产出结构化 JSON：三嫌疑人固定为 赵枢/钱映/孙棠（保证三项择一 UI 与 load_session 兼容），
  真凶由 LLM 在三人中选定；答案只放 truth 字段，绝不写进 case_text。
- case_text 是"完整案卷正文"（对玩家可见），程序在末尾追加哨兵行 [判据] 真凶：NAME，前端在哨兵前截断。

红线：
- 种子只用于派生主题词与 session_id，不进文件、不进网络；LLM 输出本就不含种子。
- 答案严格藏哨兵行之后，case_text 需通过"不泄题"检查（出现『凶手/真凶』等词即视为泄题，重试或回退）。
- 任何一步失败都返回 None，由调用方回退到 case_common.gen_case 程序化模板。
"""
import json
import os
import re
import sys
import time
from urllib import request as ureq

from seed_image import PRNG

ARK_CHAT = "https://ark.cn-beijing.volces.com/api/v3/chat/completions"

# 三嫌疑人固定（与 case_common.SUSPECTS 对齐，保证 load_session / 前端三项择一不变）
SUSPECTS = ["赵枢", "钱映", "孙棠"]

# 由 seed 派生的"主题钩子"，喂给 LLM 作差异化引导（风格 × 现场 × 核心矛盾）
GENRES = ["民国租界公馆", "英伦庄园", "蒸汽时代的实验室", "偏远山间别墅", "南方旧宅梅雨季",
          "邮轮头等舱", "赛博朋克暗巷诊所", "雪山中的旅馆"]
SCENES = ["密室", "停电夜", "暴雨封路", "雪夜", "停电的钟楼", "家族晚宴"]
HOOKS = ["一件不可能存在的物证", "一段被篡改的时间", "两套互相矛盾的供词",
         "一把消失的凶器", "一封未寄出的信", "一扇从内侧反锁的门"]
VICTIMS = ["收藏家", "药剂师", "老管家", "女继承人", "退休探长", "钟表匠", "船长", "植物学家"]

SYSTEM_PROMPT = """你是一名悬疑推理剧的"案件卷宗"编剧。你要根据给定的主题钩子，创作一桩封闭空间内的谋杀案，并只输出一份严格的 JSON（不要输出任何解释或 Markdown 代码块标记以外的内容）。

硬性约束：
1. 嫌疑人固定为三人：赵枢、钱映、孙棠，三人必须各出现一次、名字一字不差。
2. 真凶必须是这三人之一，由你在三人中自行选定。
3. 必须有真正的推理空间：给出动机、机会、手段三要素；时间线、物证、供词能互相交叉印证；玩家可凭排除法锁定真凶。
4. 每人的不在场证明都要有一处破绽（flaw），但只有真凶的破绽是"决定性"的，另外两人的破绽可被其他物证/时间线合理解释掉。
5. 至少安排一条红鲱鱼线索，把怀疑引向错误的嫌疑人。
6. 答案（真凶身份与完整推理链）只能写在 truth 字段。case_text 中严禁出现"真凶/凶手/凶手是/真凶是/犯人是/就是凶手"等揭露性字眼，也严禁暗示最终答案；case_text 只提供可推理的事实、线索与人物表现。
7. case_text 要长且有氛围：800~1500 字中文，包含案情通报、现场描写、死者背景、三名人物的背景与动机、完整时间线、物证清单、三人各自的口供（口吻各不相同）、以及可以推理的线索与红鲱鱼。用档案体叙事，代入感强。
8. 设置一道章间问答题：玩家凭 case_text 与现场/证物画面能推出一个简短、唯一的事实答案。把"题干"自然写进 case_text，把简短标准答案写在 quiz_answer（只能是时间如"19:40"、1~6字短词或短语，不得是真凶姓名），case_text 正文不得直接出现该标准答案，要靠推理/读图得到。

输出 JSON 结构（所有字段必填，不要缺字段）：
{
  "title": "案件标题（4~12字，有氛围）",
  "genre": "风格（从给定钩子里的风格继承）",
  "quiz_question": "章间问答题干（也需融入 case_text）",
  "quiz_answer": "简短唯一标准答案（时间/短词，非真凶）",
  "victim": {"name": "死者姓名", "manner": "死因", "found": "发现地点与时间"},
  "suspects": [
    {"name": "赵枢", "relation": "与死者关系", "motive": "动机", "alibi": "不在场证明", "secret": "隐瞒的秘密", "flaw": "其说辞中可被反驳的破绽"}
  ],
  "timeline": ["19:00 ……", "……"],
  "evidence": [
    {"name": "物证名", "desc": "描述", "points_to": "指向或排除谁", "red_herring": false}
  ],
  "truth": {"murderer": "赵枢", "deduction": "完整推理链（200字以上，说明如何排除另两人）"},
  "file1_prompt": "英文文生图提示词：案发现场/现场勘查的整体氛围，cinematic，no readable text, no letters, no numbers",
  "file2_prompt": "英文文生图提示词：本案最关键的一件证物特写，cinematic，no readable text, no letters, no numbers",
  "case_text": "完整案卷正文（800~1500字，档案体，不含任何答案）"
}
"""


def _call_chat(model, key, messages, timeout=180, temperature=1.0):
    body = {
        "model": model,
        "messages": messages,
        "temperature": temperature,
        "response_format": {"type": "json_object"},
    }
    data = json.dumps(body, ensure_ascii=False).encode("utf-8")
    req = ureq.Request(ARK_CHAT, data=data, method="POST")
    req.add_header("Authorization", f"Bearer {key}")
    req.add_header("Content-Type", "application/json")
    with ureq.urlopen(req, timeout=timeout) as r:
        out = json.loads(r.read().decode("utf-8"))
    content = out.get("choices", [{}])[0].get("message", {}).get("content")
    if not content:
        raise RuntimeError("no content: " + json.dumps(out, ensure_ascii=False)[:300])
    return content


def _strip_code_fence(text):
    """LLM 可能把 JSON 包在 ```json ... ``` 里，剥掉外壳。"""
    text = text.strip()
    m = re.search(r"```(?:json)?\s*(\{.*\})\s*```", text, re.S)
    if m:
        return m.group(1).strip()
    # 兜底：截取第一个 { 到最后一个 }
    a, b = text.find("{"), text.rfind("}")
    if a >= 0 and b > a:
        return text[a:b + 1]
    return text


def _validate(obj):
    """校验剧本 JSON；不合法抛异常。返回规范化后的 dict。"""
    sus = obj.get("suspects")
    if not isinstance(sus, list) or len(sus) < 3:
        raise ValueError("suspects 不足 3 人")
    names = [s.get("name") for s in sus]
    if set(names) != set(SUSPECTS):
        raise ValueError(f"嫌疑人必须且只能是 {SUSPECTS}，实为 {names}")
    murderer = obj.get("truth", {}).get("murderer")
    if murderer not in SUSPECTS:
        raise ValueError(f"真凶 {murderer} 不在 {SUSPECTS}")
    case_text = (obj.get("case_text") or "").strip()
    if len(case_text) < 300:
        raise ValueError(f"case_text 过短：{len(case_text)} 字")
    if not obj.get("file1_prompt") or not obj.get("file2_prompt"):
        raise ValueError("缺少 file1_prompt / file2_prompt")
    quiz_answer = (obj.get("quiz_answer") or "").strip()
    if not quiz_answer or len(quiz_answer) > 20:
        raise ValueError("quiz_answer 缺失或过长")
    if quiz_answer in SUSPECTS or quiz_answer == murderer:
        raise ValueError("quiz_answer 不得是真凶姓名")
    if _leaks_answer(case_text, murderer):
        raise ValueError("case_text 疑似泄题")
    return obj


def _leaks_answer(text, murderer):
    """轻量防泄题：正文同时出现凶手名 + 揭露性字眼，视为泄题。"""
    reveal = ("真凶", "真凶是", "凶手是", "犯人是", "就是凶手", "凶手就是")
    for w in reveal:
        if w in text and murderer in text:
            return True
    return False


def _theme(seed):
    prng = PRNG(seed)
    return {
        "genre": GENRES[prng.randint(0, len(GENRES) - 1)],
        "scene": SCENES[prng.randint(0, len(SCENES) - 1)],
        "hook": HOOKS[prng.randint(0, len(HOOKS) - 1)],
        "victim": VICTIMS[prng.randint(0, len(VICTIMS) - 1)],
    }


def gen_case_json(seed, key, model, strict=False, tries=3, genre=""):
    """生成案件剧本 dict；任何失败返回 None（调用方回退程序化模板）。

    genre：可选案件类型；非空则固定风格，其余主题元素仍由种子随机。
    返回：
      {"title","murderer","case_text","file1_prompt","file2_prompt"}
    """
    t = _theme(seed)
    if genre:
        t["genre"] = genre
    user = (f"主题钩子：风格「{t['genre']}」，场景「{t['scene']}」，核心矛盾「{t['hook']}」，"
            f"死者身份「{t['victim']}」。请据此创作一桩完整案件。")
    last = None
    for i in range(tries):
        try:
            content = _call_chat(model, key,
                                 [{"role": "system", "content": SYSTEM_PROMPT},
                                  {"role": "user", "content": user}])
            obj = json.loads(_strip_code_fence(content))
            obj = _validate(obj)
            murderer = obj["truth"]["murderer"]
            return {
                "title": obj.get("title", "").strip(),
                "murderer": murderer,
                "case_text": obj["case_text"].strip(),
                "quiz_question": (obj.get("quiz_question") or "").strip(),
                "quiz_answer": obj["quiz_answer"].strip(),
                "file1_prompt": obj["file1_prompt"].strip(),
                "file2_prompt": obj["file2_prompt"].strip(),
            }
        except Exception as e:
            last = e
            if strict:
                raise
            sys.stderr.write(f"[warn] LLM 剧本第 {i + 1}/{tries} 次失败：{e}\n")
            if i < tries - 1:
                time.sleep(1 + i)
    if last:
        sys.stderr.write(f"[warn] LLM 剧本最终失败：{last}\n")
    return None


if __name__ == "__main__":
    # 单独调试：python llm_case.py <seed_hex> [--dotenv PATH]
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("seed", nargs="?", default=None)
    ap.add_argument("--dotenv", default=None)
    ap.add_argument("--strict", action="store_true")
    args = ap.parse_args()

    # 复用 ai_gen_case 的 load_dotenv（避免重复实现）
    here = os.path.dirname(os.path.abspath(__file__))
    sys.path.insert(0, here)
    from ai_gen_case import load_dotenv
    load_dotenv(args.dotenv)
    key = os.environ.get("ARK_API_KEY")
    model = os.environ.get("ARK_LLM_MODEL")
    if not (key and model):
        sys.exit("需在 .env 配置 ARK_API_KEY 与 ARK_LLM_MODEL（LLM 推理接入点 endpoint id）")

    seed = bytes.fromhex(args.seed) if args.seed else os.urandom(32)
    r = gen_case_json(seed, key, model, args.strict)
    if not r:
        sys.exit(1)
    print(f"标题：{r['title']}")
    print(f"真凶：{r['murderer']}")
    print(f"正文 {len(r['case_text'])} 字\n----\n{r['case_text']}")
    print("\n---- 文生图提示词 ----")
    print("file1:", r["file1_prompt"])
    print("file2:", r["file2_prompt"])
