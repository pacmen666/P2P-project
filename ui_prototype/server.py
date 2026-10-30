#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""碎片档案 · Web 案情板服务器（零依赖，Python 标准库）。

架构（网络连接复用 C 核心，本文件不重写任何网络协议）：
- 静态文件服务（根 = 项目根，前端 /ui_prototype、状态 /logs、证物 /ui_prototype/session_*）
- 案件库 API：扫描 session_* 目录，读 manifest.bin + file_3，抽取标题/摘要/真凶（本地功能）
- 现场生成新案：后台线程 subprocess 调 ai_gen_case.py（本地功能）
- 命令桥：spawn ./client_transfer，把浏览器操作翻译成 client 的 stdin 命令，
  再轮询 client 每次重绘落盘的 ui_state.json（UI_JSON_PATH）回推给前端。
  —— 进房/传输/择一的网络传输全部由 C 核心 P2P（room_server + relay_server + transfer）完成。

运行（需在 WSL/Linux 先 `make transfer` 产出 room_server / relay_server / client_transfer）：
    python ui_prototype/server.py          # 默认 8080
访问：http://localhost:8080/ui_prototype/index.html
"""
import http.server
import json
import os
import re
import socket
import struct
import subprocess
import sys
import threading
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)                      # Fragment_Archive/
PYGEN = os.path.join(ROOT, "tests", "py_AI_gen")  # 案件生成目录
SUSPECTS = ["赵枢", "钱映", "孙棠"]
LOCK = threading.Lock()

# 案件可落盘的位置：生成目录 + 前端数据目录
CASE_ROOTS = [
    PYGEN,
    os.path.join(ROOT, "ui_prototype"),
]

# C 核心二进制（make transfer 产物）
CLIENT_BIN = os.path.join(ROOT, "client_transfer")
ROOM_BIN = os.path.join(ROOT, "room_server")
RELAY_BIN = os.path.join(ROOT, "relay_server")
DEVNULL = subprocess.DEVNULL


# ---------------------------------------------------------------------------
# 案件库（本地功能，不涉及网络）
# ---------------------------------------------------------------------------
def _read_manifest(sdir):
    p = os.path.join(sdir, "manifest.bin")
    if not os.path.isfile(p):
        return None
    b = open(p, "rb").read()
    if len(b) < 37:
        return None
    sid, seed_hash, fn = struct.unpack_from(">I32sB", b, 0)
    off = 37
    files = []
    for _ in range(fn):
        if off + 41 > len(b):
            break
        fid, ch, kind, fsize, _fsha, pc = struct.unpack_from(">BBBI32sH", b, off)
        off += 41 + pc * 32
        files.append({"file_id": fid, "chapter": ch, "kind": kind, "size": fsize})
    return {"session_id": sid, "seed_hash": seed_hash.hex(), "files": files}


def _read_text3(sdir):
    p = os.path.join(sdir, "file_3")
    if not os.path.isfile(p):
        return ""
    return open(p, "rb").read().decode("utf-8", "ignore")


def _title_from(body):
    first = body.split("\n", 1)[0].strip()
    m = re.search(r"案由[:：]\s*([^\s，,。；;【]+)", first)
    if m:
        return m.group(1).strip()
    m = re.search(r"编号[:：]\s*([^\s，,。；;【]+)", first)
    if m:
        return m.group(1).strip()
    m = re.search(r"【([^】]+)】", first)
    if m:
        t = m.group(1).strip()
        for junk in ("案件卷宗", "案卷正文", "案卷"):
            t = t.replace(junk, "")
        t = t.strip(" ：:，,。")
        if t:
            return t
    return first[:16] or "未命名案件"


def _extract_case(sdir):
    m = _read_manifest(sdir)
    if not m:
        return None
    txt = _read_text3(sdir)
    body = txt.split("[判据]")[0].strip()
    murderer = None
    i = txt.find("[判据] 真凶：")
    if i >= 0:
        name = txt[i + len("[判据] 真凶："):].split("\n", 1)[0].strip()
        if name in SUSPECTS:
            murderer = name
    return {
        "sid": "%08x" % m["session_id"],
        "title": _title_from(body),
        "summary": body[:160],
        "files": m["files"],
        "murderer": murderer,
        "seed_hash": m["seed_hash"],
    }


def _find_session_dir(sid):
    for base in CASE_ROOTS:
        d = os.path.join(base, "session_" + sid)
        if os.path.isdir(d) and os.path.isfile(os.path.join(d, "manifest.bin")):
            return d
    return None


def list_cases():
    seen = {}
    for base in CASE_ROOTS:
        if not os.path.isdir(base):
            continue
        try:
            names = sorted(os.listdir(base))
        except OSError:
            continue
        for name in names:
            if not name.startswith("session_"):
                continue
            sdir = os.path.join(base, name)
            c = _extract_case(sdir)
            if not c or c["sid"] in seen:
                continue
            c["mtime"] = os.path.getmtime(os.path.join(sdir, "manifest.bin"))
            seen[c["sid"]] = c
    out = sorted(seen.values(), key=lambda c: c["mtime"], reverse=True)
    return out


def _latest_sid():
    cs = list_cases()
    return cs[0]["sid"] if cs else None


# ---------------------------------------------------------------------------
# 现场生成新案（本地功能，后台线程）
# ---------------------------------------------------------------------------
GEN_BUSY = {"busy": False}
GEN_LAST = {"error": ""}


def _generate_async():
    if GEN_BUSY["busy"]:
        return False
    GEN_BUSY["busy"] = True
    GEN_LAST["error"] = ""

    def work():
        try:
            subprocess.run(
                [sys.executable, "ai_gen_case.py", "--base", PYGEN,
                 "--dotenv", os.path.join(PYGEN, ".env")],
                cwd=PYGEN, capture_output=True, text=True, timeout=600,
            )
        except Exception as e:
            GEN_LAST["error"] = str(e)[:200]
        finally:
            GEN_BUSY["busy"] = False

    threading.Thread(target=work, daemon=True).start()
    return True


# ---------------------------------------------------------------------------
# C 核心服务自动拉起（room_server / relay_server 常驻，进程级单例）
# ---------------------------------------------------------------------------
SERVICES = []


def _port_open(port):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(0.3)
    try:
        s.connect(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def _is_elf(p):
    try:
        with open(p, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def _is_runnable(p):
    """能否真正执行：Windows 下 ELF（WSL 产物）不可直接运行。"""
    if not os.path.isfile(p):
        return False
    if os.name == "nt" and _is_elf(p):
        return False
    return True


def ensure_services():
    """确保 room_server / relay_server 在跑（未监听且二进制存在则拉起）。"""
    started = []
    for name, port, binpath in (
        ("room_server", 37401, ROOM_BIN),
        ("relay_server", 37405, RELAY_BIN),
    ):
        if _is_runnable(binpath) and not _port_open(port):
            try:
                p = subprocess.Popen([binpath], cwd=ROOT,
                                     stdout=DEVNULL, stderr=DEVNULL)
                SERVICES.append(p)
                started.append(name)
            except OSError:
                pass
    return started


# ---------------------------------------------------------------------------
# 命令桥：一个 Bridge = 一个 client_transfer 进程 = 一个玩家
# ---------------------------------------------------------------------------
BRIDGES = {}  # id -> Bridge


class Bridge:
    def __init__(self, nick, role, host, session_dir, mode):
        self.id = uuid.uuid4().hex[:10]
        self.nick = (nick or "detective")[:16]
        self.role = role              # "host" | "player"
        self.host = host or "localhost"
        self.session_dir = session_dir
        self.mode = mode              # "" | tcp | udp | relay
        self.proc = None
        self.state_path = os.path.join(ROOT, "logs", "ui_state_%s.json" % self.id)
        self.log_lines = []
        self._mu = threading.Lock()
        self.started = False

    def start(self):
        argv = [CLIENT_BIN, "--nick", self.nick,
                "--data-dir", os.path.join(ROOT, "ui_prototype")]
        if self.mode:
            argv += ["--mode", self.mode]
        if self.role == "host":
            argv.append("--host")
            if self.session_dir:
                argv += ["--session", self.session_dir]
        env = dict(os.environ)
        env["UI_JSON_PATH"] = self.state_path

        self.proc = subprocess.Popen(
            argv, cwd=ROOT, stdin=subprocess.PIPE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, bufsize=1, env=env,
        )
        self.started = True
        threading.Thread(target=self._pump_stdout, daemon=True).start()
        # 进房意图明确，直接翻译成 join 命令
        join_cmd = "join %s %s%s" % (self.nick, self.host,
                                     " host" if self.role == "host" else "")
        self.command(join_cmd)
        return self.id

    def _pump_stdout(self):
        for line in self.proc.stdout:
            line = line.rstrip("\n")
            if not line:
                continue
            with self._mu:
                self.log_lines.append(line)
                self.log_lines = self.log_lines[-200:]

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def command(self, cmd):
        cmd = (cmd or "").strip()
        if not cmd:
            return False, "空命令"
        if not self.running():
            return False, "client_transfer 已退出"
        try:
            self.proc.stdin.write(cmd + "\n")
            self.proc.stdin.flush()
            return True, ""
        except (OSError, ValueError) as e:
            return False, str(e)

    def state(self):
        if not os.path.isfile(self.state_path):
            return None
        try:
            with open(self.state_path, "r", encoding="utf-8") as f:
                return json.load(f)
        except (OSError, ValueError):
            return None

    def log(self):
        with self._mu:
            return list(self.log_lines[-100:])

    def quit(self):
        if self.running():
            try:
                self.proc.stdin.write("quit\n")
                self.proc.stdin.flush()
            except OSError:
                pass
            try:
                self.proc.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.proc.terminate()
        self.started = False


def bridge_capability():
    return {
        "available": _is_runnable(CLIENT_BIN),
        "client": os.path.isfile(CLIENT_BIN),
        "room_server": os.path.isfile(ROOM_BIN),
        "relay_server": os.path.isfile(RELAY_BIN),
        "services_running": _port_open(37401),
        "wsl_hint": os.name == "nt" and _is_elf(CLIENT_BIN),
    }


def shutdown_bridges():
    with LOCK:
        for b in list(BRIDGES.values()):
            b.quit()
        BRIDGES.clear()
        for p in SERVICES:
            try:
                p.terminate()
            except OSError:
                pass
        SERVICES.clear()


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------
class GameHandler(http.server.BaseHTTPRequestHandler):
    server_version = "FragmentArchive/1.0"

    def _send(self, code, obj=None, ctype="application/json", raw=None):
        self.send_response(code)
        self.send_header("Cache-Control", "no-store")
        if raw is not None:
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)
        else:
            data = json.dumps(obj, ensure_ascii=False).encode("utf-8")
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    def _json_body(self):
        n = int(self.headers.get("Content-Length", 0) or 0)
        if n <= 0:
            return {}
        try:
            return json.loads(self.rfile.read(n).decode("utf-8"))
        except Exception:
            return {}

    def log_message(self, fmt, *args):
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    # --- 路由 ---
    def do_GET(self):
        path = self.path.split("?", 1)[0].rstrip("/") or "/"
        if path == "/api/cases":
            self._send(200, {"cases": list_cases()})
        elif path == "/api/suspects":
            self._send(200, {"suspects": SUSPECTS})
        elif path == "/api/generate":
            self._send(200, {"busy": GEN_BUSY["busy"], "error": GEN_LAST["error"]})
        elif path == "/api/bridge/capability":
            self._send(200, bridge_capability())
        elif path.startswith("/api/cases/"):
            sid = path.rsplit("/", 1)[1]
            sdir = _find_session_dir(sid)
            if not sdir:
                self._send(404, {"error": "not found"})
            else:
                self._send(200, {"case": _extract_case(sdir)})
        elif path.startswith("/api/bridge/") and path.endswith("/state"):
            bid = path.split("/")[3]
            b = BRIDGES.get(bid)
            st = b.state() if b else None
            self._send(200, {"bridge_id": bid, "running": bool(b and b.running()),
                             "state": st}) if b else self._send(404, {"error": "bridge not found"})
        elif path.startswith("/api/bridge/") and path.endswith("/log"):
            bid = path.split("/")[3]
            b = BRIDGES.get(bid)
            self._send(200, {"lines": b.log()}) if b else self._send(404, {"error": "bridge not found"})
        elif path.startswith("/api/evidence/"):
            parts = path.split("/")
            if len(parts) == 5:
                sid, fid = parts[3], parts[4]
                sdir = _find_session_dir(sid)
                fpath = os.path.join(sdir, "file_" + fid) if sdir else ""
                if not fpath or not os.path.isfile(fpath):
                    self._send(404, {"error": "not found"})
                    return
                data = open(fpath, "rb").read()
                ctype = "image/jpeg" if fid in ("1", "2") else "text/plain; charset=utf-8"
                self._send(200, raw=data, ctype=ctype)
            else:
                self._send(400, {"error": "bad path"})
        else:
            self._serve_static()

    def do_POST(self):
        path = self.path.split("?", 1)[0].rstrip("/")
        body = self._json_body()
        if path == "/api/generate":
            ok = _generate_async()
            self._send(200 if ok else 409,
                       {"ok": ok, "error": GEN_LAST["error"]})
        elif path == "/api/bridge/start":
            self._bridge_start(body)
        elif path.startswith("/api/bridge/") and path.endswith("/command"):
            self._bridge_command(path.split("/")[3], body)
        elif path.startswith("/api/bridge/") and path.endswith("/quit"):
            self._bridge_quit(path.split("/")[3])
        else:
            self._send(404, {"error": "not found"})

    # --- 命令桥动作 ---
    def _bridge_start(self, b):
        if not _is_runnable(CLIENT_BIN):
            cap = bridge_capability()
            hint = ("检测到 WSL 编译的 ELF 产物，Windows 无法直接执行。请在 WSL/Linux 下运行 `python ui_prototype/server.py`。"
                    if cap["wsl_hint"] else
                    "未找到 client_transfer。请先在 WSL/Linux 下运行 `make transfer`。")
            self._send(409, {"error": hint, "capability": cap})
            return
        role = "host" if b.get("role") == "host" else "player"
        session_dir = None
        if role == "host":
            sid = (b.get("session") or "").strip()
            if sid:
                session_dir = _find_session_dir(sid)
                if not session_dir:
                    self._send(400, {"error": "案件不存在，请重新选择"})
                    return
        mode = b.get("mode") or ""
        if mode not in ("", "tcp", "udp", "relay"):
            mode = ""
        ensure_services()
        br = Bridge((b.get("nick") or "").strip(), role,
                    (b.get("host") or "").strip(), session_dir, mode)
        try:
            br.start()
        except (OSError, ValueError) as e:
            self._send(500, {"error": "启动 client_transfer 失败：%s" % e})
            return
        with LOCK:
            BRIDGES[br.id] = br
        self._send(200, {"bridge_id": br.id, "role": role,
                         "state_path": br.state_path,
                         "join_sent": True})

    def _bridge_command(self, bid, b):
        br = BRIDGES.get(bid)
        if not br:
            self._send(404, {"error": "bridge not found"})
            return
        ok, err = br.command(b.get("cmd"))
        self._send(200 if ok else 409, {"ok": ok, "error": err})

    def _bridge_quit(self, bid):
        br = BRIDGES.get(bid)
        if br:
            br.quit()
            with LOCK:
                BRIDGES.pop(bid, None)
        self._send(200, {"ok": True})

    # --- 静态文件 ---
    def _serve_static(self):
        path = self.path.split("?", 1)[0]
        if path == "/":
            path = "/ui_prototype/index.html"
        rel = path.lstrip("/")
        full = os.path.normpath(os.path.join(ROOT, rel))
        if not full.startswith(ROOT) or not os.path.isfile(full):
            self._send(404, {"error": "not found"})
            return
        ext = os.path.splitext(full)[1].lower()
        ctype = {
            ".html": "text/html; charset=utf-8",
            ".css": "text/css; charset=utf-8",
            ".js": "application/javascript; charset=utf-8",
            ".json": "application/json; charset=utf-8",
            ".png": "image/png",
            ".jpg": "image/jpeg",
            ".jpeg": "image/jpeg",
            ".svg": "image/svg+xml",
        }.get(ext, "application/octet-stream")
        data = open(full, "rb").read()
        self._send(200, raw=data, ctype=ctype)


def main():
    port = int(os.environ.get("PORT", "8080"))
    httpd = http.server.ThreadingHTTPServer(("", port), GameHandler)
    print("碎片档案 · Web 案情板  http://localhost:%d/ui_prototype/index.html" % port)
    print("案件库：已扫描 %d 案" % len(list_cases()))
    cap = bridge_capability()
    print("C 核心命令桥：client_transfer=%s room_server=%s relay_server=%s（需在 WSL/Linux `make transfer`）"
          % (cap["client"], cap["room_server"], cap["relay_server"]))
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        shutdown_bridges()
        httpd.server_close()


if __name__ == "__main__":
    main()
