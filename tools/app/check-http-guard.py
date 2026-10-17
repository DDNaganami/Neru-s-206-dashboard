#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check-http-guard.py -- 「这个本地服务不会被**别的网页**当跳板」的可执行判据

    <真 python> tools/app/check-http-guard.py     # 退出码 0 = 全过；1 = 有判据红

----------------------------------------------------------------------------
为什么要有它（2026-10-16 的补漏单）
----------------------------------------------------------------------------
三条真洞，都在这一个 server.py 里：

  ① **POST 没有来源校验**：任意网页都能 `no-cors` 往 `/api/quit`、`/api/upload`、
     `/api/flash` 发请求 ⇒ **能把东西写进板子** ✗。
     判据：`Host` 必须是 `127.0.0.1[:port]` / `localhost[:port]`；带了 `Origin`
     就必须等于自己这个来源（跨源 ⇒ 403）；★ **不带 `Origin` 的请求要放行**
     （curl / python 脚本 / 命令行用法都不带它，一刀切会把它们全打回去 ✗）；
     POST 另要求 `Content-Type` 合理（否则 400）。
  ② **目录判断可绕过**：`startswith(REPO/SCRATCH)` 不是目录关系 ⇒ 兄弟目录
     `…-dashboard2\\`、junction 都能绕；而且**仓库里任何文件**都能被刷进分区 ✗。
     判据：`realpath` + `commonpath`，并且只有 `SCRATCH\\app-upload` 下 ≤ 32 MB 的
     `*.bin` / `*.json` 能刷（别的路径、别的后缀 ⇒ 400；`*.json` 是给「刷存下来的
     theme.json」留的，它同样只能待在 app-upload 里）。
  ③ **未匹配的 GET 直通** `SimpleHTTPRequestHandler` ⇒ 整个仓库（含 `.git`、docs）
     连同**目录列表**都发出去 ✗；`/api/board-status?host=` 还能把服务端当跳板 ✗。
     判据：目录 ⇒ 404/403；白名单静态资源照发；`host=8.8.8.8` ⇒ 400。
  ④ 顺带钉住：`run()` 的 `cwd` 默认值**不在 import 时绑定**（否则打包后 `--repo`
     对构建/烧写/预览全部失效，可能在**错目录**里构建 ✗）。

★ 只用标准库。
★ 全程**不刷板、不碰串口**：`server.run()` 被换成"只记账不执行"的假函数，
  端口/路径校验都发生在拼命令**之前**，所以连假函数都不该被调到。
★ 不往真机的 scratch 里写东西：`server.SCRATCH` 换成临时目录，跑完删掉。
★ 临时服务只听 127.0.0.1 的一个临时端口（端口 0 = 让系统挑一个空的）。
"""

import importlib.util
import json
import os
import re
import shutil
import socket
import sys
import tempfile
import threading
import urllib.parse
from http.server import ThreadingHTTPServer

APP_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(APP_DIR))
SERVER_PY = os.path.join(APP_DIR, "server.py")

try:
    sys.stdout.reconfigure(errors="replace")     # GBK 控制台别把脚本打崩
except Exception:
    pass

PASS = 0
FAIL = 0


def ok(cond, what):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("  ok  " + what)
    else:
        FAIL += 1
        print("  BAD " + what)
    return bool(cond)


def section(t):
    print("\n== " + t)


# ---------------------------------------------------------------------------
# 把 server.py 当模块加载（**不**跑 main()：它只在 __main__ 里跑）
# ---------------------------------------------------------------------------
spec = importlib.util.spec_from_file_location("dsh_app_server_guard", SERVER_PY)
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)

REAL_RUN = server.run                 # ⑧ 那一节要拿回真实现来验
CALLS = []


def fake_run(cmd, timeout=900, cwd=None):
    """只记账，不执行 —— 这就是"不碰串口、不跑 esptool"的那一步。"""
    CALLS.append(list(cmd))
    return True, "stub: 没有真的跑（这是判据，不是刷写）"


server.run = fake_run
server.Handler.log_message = lambda self, fmt, *args: None      # 别把请求日志刷进来

TMP = tempfile.mkdtemp(prefix="dsh-http-guard-")
server.SCRATCH = TMP                  # 上传只落在临时目录（不碰真机那个 scratch）
UP = server.upload_dir()              # = <TMP>\app-upload

srv = ThreadingHTTPServer(("127.0.0.1", 0), server.Handler)
PORT = srv.server_address[1]
HOST = "127.0.0.1:%d" % PORT
SELF_ORIGIN = "http://" + HOST
threading.Thread(target=srv.serve_forever, daemon=True).start()
print("临时服务：http://%s（只监听 127.0.0.1）；上传目录：%s" % (HOST, UP))


def request(method, path, headers=None, body=b"", timeout=15):
    """手写一个 HTTP 请求（要**精确**控制 Host / Origin / Content-Type 这几颗头）。

    返回 (status, 响应头文本, 响应体字节)；连不上就 (0, "", b"")。
    ★ 为什么不用 urllib：它会替我们补 Host，`data` 还会自动补一个 Content-Type ——
      而这几条判据测的正是"这几颗头是谁写的"。
    """
    hdrs = {"Host": HOST, "Connection": "close"}
    hdrs.update(headers or {})
    if body:
        hdrs.setdefault("Content-Length", str(len(body)))
    lines = ["%s %s HTTP/1.1" % (method, path)]
    lines += ["%s: %s" % (k, v) for k, v in hdrs.items()]
    raw = ("\r\n".join(lines) + "\r\n\r\n").encode("utf-8") + body
    chunks = []
    try:
        s = socket.create_connection(("127.0.0.1", PORT), timeout)
    except OSError:
        return 0, "", b""
    try:
        s.sendall(raw)
        while True:
            try:
                b = s.recv(65536)
            except (socket.timeout, OSError):    # 被拒时对端可能直接 reset
                break
            if not b:
                break
            chunks.append(b)
    finally:
        try:
            s.close()
        except OSError:
            pass
    data = b"".join(chunks)
    head, _, rest = data.partition(b"\r\n\r\n")
    text = head.decode("iso-8859-1", "replace")
    m = re.match(r"HTTP/\d\.\d\s+(\d{3})", text)
    return (int(m.group(1)) if m else 0), text, rest


def jbody(raw):
    """响应体当 JSON 读（不是 JSON 就空字典）。"""
    try:
        return json.loads((raw or b"").decode("utf-8", "replace") or "{}")
    except Exception:
        return {}


UP_BIN = ""          # ② 那一次上传落盘的真实路径（⑥ 的正常刷写用它）
SIBLING = ""         # ⑥ 那个"兄弟目录"探针
SIBLING_CREATED = False

try:
    # -----------------------------------------------------------------------
    section("① 带**伪造 Origin** 的 POST ⇒ 403（「任意网页都能往板子里写」那条路）")
    for path in ("/api/upload", "/api/flash", "/api/quit"):
        code, _, body = request("POST", path,
                                {"Origin": "http://evil.example",
                                 "Content-Type": "application/octet-stream"}, b"")
        ok(code == 403, "%s 从 evil.example 发过来 ⇒ %d（要 403）" % (path, code))
        j = jbody(body)
        ok(j.get("ok") is False and "Origin" in str(j.get("error")),
           "%s 的拒绝理由说清了是 Origin 不对：%r" % (path, str(j.get("error"))[:60]))
    ok(len(CALLS) == 0, "被 403 的这几下 esptool 一次都没被拼出来（没碰板子）")

    # -----------------------------------------------------------------------
    section("② **不带** Origin 的 POST ⇒ 放行（curl / python 脚本 / 命令行都是这种）")
    probe = b"\x00\x01\x02\x03" * 4
    code, _, body = request("POST", "/api/upload",
                            {"Content-Type": "application/octet-stream",
                             "X-Filename": "guard-probe.bin"}, probe)
    j = jbody(body)
    UP_BIN = j.get("path") or ""
    ok(code == 200 and j.get("ok") is True,
       "不带 Origin 的 /api/upload ⇒ %d，ok=%r（要 200/True）" % (code, j.get("ok")))
    ok(bool(UP_BIN) and os.path.abspath(os.path.dirname(UP_BIN)) == os.path.abspath(UP),
       "落在 app-upload 下：%r" % (UP_BIN,))
    ok(os.path.isfile(UP_BIN) and os.path.getsize(UP_BIN) == len(probe),
       "文件真写出来了，字节数 %d" % (len(probe),))

    code, _, body = request("POST", "/api/upload",
                            {"Origin": SELF_ORIGIN,
                             "Content-Type": "application/octet-stream",
                             "X-Filename": "guard-probe-same-origin.bin"}, probe)
    ok(code == 200 and jbody(body).get("ok") is True,
       "★ 同源页面（Origin = %s）⇒ %d（要 200：自己人照常干活）"
       % (SELF_ORIGIN, code))

    code, _, _ = request("POST", "/api/upload",
                         {"Origin": "http://127.0.0.1:1",      # 别的端口 ⇒ 别的来源
                          "Content-Type": "application/octet-stream",
                          "X-Filename": "no.bin"}, b"")
    ok(code == 403, "Origin 换了端口（http://127.0.0.1:1）⇒ %d（要 403）" % code)

    # ★ 后缀白名单是 `.bin` / `.json` 两种（`.json` = 「刷存下来的 theme.json」，要留着）
    code, _, body = request("POST", "/api/upload",
                            {"Content-Type": "application/octet-stream",
                             "X-Filename": "theme.json"}, b'{"v":1}')
    ok(code == 200 and jbody(body).get("ok") is True,
       "★ /api/upload 收 theme.json ⇒ %d（要 200：刷存下来的 theme.json 这条路要留着）" % code)
    code, _, body = request("POST", "/api/upload",
                            {"Content-Type": "application/octet-stream",
                             "X-Filename": "notes.txt"}, b"hi")
    ok(code == 400, "/api/upload 收 notes.txt ⇒ %d（要 400：后缀不在白名单）" % code)

    # -----------------------------------------------------------------------
    section("③ POST 的 Content-Type 不合理 ⇒ 400（浏览器「不用预检」的那几种进不来）")
    for ct in ("text/plain", "application/x-www-form-urlencoded", "multipart/form-data"):
        code, _, body = request("POST", "/api/flash", {"Content-Type": ct})
        ok(code == 400, "Content-Type: %s ⇒ %d（要 400）" % (ct, code))
    code, _, body = request("POST", "/api/flash", {"Content-Type": "application/json"},
                            json.dumps({"port": "COM8", "kind": "assets",
                                        "path": ""}).encode("utf-8"))
    j = jbody(body)
    ok(code == 400 and not j.get("error") and "只允许刷" in str(j.get("out") or ""),
       "★ application/json 这一种**不是**被 Content-Type 挡下的（回的是 do_flash 的"
       "文件白名单那一句：%r）" % (str(j.get("out") or "")[:40],))

    # -----------------------------------------------------------------------
    section("④ `Host` 不是 loopback ⇒ 403（DNS rebinding：攻击者域名解析到 127.0.0.1）")
    code, _, _ = request("GET", "/api/health", {"Host": "evil.example:8787"})
    ok(code == 403, "GET /api/health 带 Host: evil.example:8787 ⇒ %d（要 403）" % code)
    code, _, _ = request("POST", "/api/upload",
                         {"Host": "evil.example:8787",
                          "Content-Type": "application/octet-stream"}, b"")
    ok(code == 403, "POST 带 Host: evil.example:8787 ⇒ %d（要 403）" % code)
    code, _, _ = request("GET", "/api/health", {"Host": "192.168.1.9:%d" % PORT})
    ok(code == 403, "Host 换成局域网 IP（192.168.1.9）⇒ %d（要 403：只认 loopback）" % code)
    code, _, body = request("GET", "/api/health", {"Host": "localhost:%d" % PORT})
    ok(code == 200 and jbody(body).get("ok") is True,
       "Host: localhost:%d ⇒ %d（localhost 也要放行）" % (PORT, code))
    code, _, body = request("GET", "/api/health")
    ok(code == 200 and jbody(body).get("ok") is True,
       "Host: %s ⇒ %d（127.0.0.1 照常）" % (HOST, code))

    # -----------------------------------------------------------------------
    section("⑤ 目录列表被关，仓库里别的东西也不发（未匹配的 GET 不再直通）")
    for p in ("/tools/app/", "/tools/theme-editor/", "/tools/", "/preview/",
              "/preview/frames/", "/tools/app/build/", "/.git/config", "/.git/HEAD",
              "/README.md", "/platformio.ini", "/src/", "/docs/", "/partitions.csv"):
        code, _, body = request("GET", p)
        ok(code in (404, 403), "GET %s ⇒ %d（要 404/403：没有列表、也不发仓库文件）"
           % (p, code))
    code, _, body = request("GET", "/tools/app/")
    ok(b"index of" not in body.lower() and b"<title>directory" not in body.lower(),
       "GET /tools/app/ 的响应体里没有目录列表（不是一份 index）")

    section("⑤b 白名单里的静态资源照发（没把界面弄坏）")
    for p in ("/tools/app/index.html", "/tools/app/app.js", "/tools/app/app.css",
              "/tools/theme-editor/index.html", "/tools/theme-editor/face-stages.js",
              "/tools/app/README.md"):
        code, head, body = request("GET", p)
        ok(code == 200 and len(body) > 0,
           "GET %s ⇒ %d（要 200，且有内容 %d 字节）" % (p, code, len(body)))
    if os.path.isfile(os.path.join(REPO, "tools", "web", "index.html")):
        code, _, body = request("GET", "/tools/web/index.html")
        ok(code == 200 and len(body) > 0, "GET /tools/web/index.html ⇒ %d（导航页照发）" % code)
    if os.path.isfile(os.path.join(REPO, "preview", "preview.html")):
        code, _, body = request("GET", "/preview/preview.html")
        ok(code == 200 and len(body) > 0, "GET /preview/preview.html ⇒ %d（看帧页照发）" % code)
    frames_dir = os.path.join(REPO, "preview", "frames")
    frame = None
    if os.path.isdir(frames_dir):
        names = sorted(n for n in os.listdir(frames_dir) if n.lower().endswith(".bmp"))
        frame = names[0] if names else None
    if frame:
        code, head, body = request("GET", "/preview/frames/" + frame)
        ok(code == 200 and len(body) > 0,
           "GET /preview/frames/%s ⇒ %d（看帧页的帧照发）" % (frame, code))
        ok("image/bmp" in head.lower(), "那个帧的 Content-Type 是 image/bmp")
        code, _, _ = request("GET", "/preview/frames/../../server.py")
        ok(code in (404, 400), "GET /preview/frames/../../server.py ⇒ %d（要 404：跳不出去）" % code)

    # -----------------------------------------------------------------------
    section("⑥ `/api/board-status?host=` 只收私有 IPv4，其余 400（别把服务端当跳板）")
    for h in ("8.8.8.8", "example.com", "172.32.0.1", "100.64.0.1", "0.0.0.0",
              "::1", "1.2.3.4:5678", "127.1"):
        code, _, body = request("GET", "/api/board-status?host=" + urllib.parse.quote(h))
        ok(code == 400, "host=%s ⇒ %d（要 400）" % (h, code))
    for h in ("127.0.0.1", "192.168.4.1", "10.1.2.3", "172.16.0.1", "172.31.255.254"):
        ok(server.is_private_v4(h) is True, "is_private_v4(%r) ⇒ True（私有网段要收）" % h)
    for h in ("8.8.8.8", "172.32.0.1", "example.com", ""):
        ok(server.is_private_v4(h) is False, "is_private_v4(%r) ⇒ False（一律不认）" % h)
    code, _, body = request("POST", "/api/ota-verify", {"Content-Type": "application/json"},
                            json.dumps({"host": "8.8.8.8"}).encode("utf-8"))
    ok(code == 400, "POST /api/ota-verify {host: 8.8.8.8} ⇒ %d（要 400：同一个跳板口子）" % code)

    # -----------------------------------------------------------------------
    section("⑦ 能刷的只有 `SCRATCH\\app-upload` 下的 *.bin / *.json（兄弟目录也绕不过去）")

    def flash_post(path, kind="assets"):
        code, _, body = request("POST", "/api/flash", {"Content-Type": "application/json"},
                                json.dumps({"port": "COM8", "kind": kind,
                                            "path": path}).encode("utf-8"))
        return code, jbody(body)

    # ★ 复刻那个绕过：兄弟目录 `…-dashboard-guard-probe\` 的路径字符串以 REPO 开头
    SIBLING = REPO + "-guard-probe"
    if not os.path.isdir(SIBLING):
        os.makedirs(SIBLING)
        SIBLING_CREATED = True
    evil = os.path.join(SIBLING, "evil.bin")
    with open(evil, "wb") as fh:
        fh.write(b"\x00" * 64)
    ok(evil.startswith(REPO),
       "（这正是一个 startswith 判法会放过的路径：%s）" % evil)
    del CALLS[:]
    code, j = flash_post(evil)
    ok(code == 400 and j.get("ok") is False,
       "兄弟目录里的 evil.bin ⇒ %d（要 400：realpath+commonpath 判的是目录关系）" % code)
    ok(len(CALLS) == 0, "被拒时 esptool 一次都没被拼出来（没碰板子）")

    code, j = flash_post(os.path.join(REPO, "README.md"))
    ok(code == 400, "仓库里的 README.md ⇒ %d（要 400：仓库文件不许被刷进分区）" % code)

    outside = os.path.join(TMP, "outside.bin")          # scratch 里，但不在 app-upload 下
    with open(outside, "wb") as fh:
        fh.write(b"\x00" * 64)
    code, j = flash_post(outside)
    ok(code == 400, "scratch 根下（不在 app-upload 里）的 .bin ⇒ %d（要 400）" % code)

    # ★ 别的目录里的 .json / .txt：**目录**那一道先拦（白名单是"目录 + 后缀"两条一起）
    outside_json = os.path.join(TMP, "theme.json")
    with open(outside_json, "wb") as fh:
        fh.write(b"{}")
    del CALLS[:]
    code, j = flash_post(outside_json, kind="theme")
    ok(code == 400, "别的目录（scratch 根下）的 theme.json ⇒ %d（要 400：.json 也要在 app-upload 里）"
       % code)
    outside_txt = os.path.join(TMP, "notes.txt")
    with open(outside_txt, "wb") as fh:
        fh.write(b"hi")
    code, j = flash_post(outside_txt)
    ok(code == 400, "别的目录里的 .txt ⇒ %d（要 400）" % code)

    txt_in_up = os.path.join(UP, "notes.txt")
    with open(txt_in_up, "wb") as fh:
        fh.write(b"hi")
    code, j = flash_post(txt_in_up)
    ok(code == 400, "app-upload 里的 .txt ⇒ %d（要 400：后缀也不在白名单）" % code)
    ok(len(CALLS) == 0, "上面这几下也一次都没拼出 esptool 命令（没碰板子）")

    # ★ app-upload 里的 .json 是**允许**的（「刷存下来的 theme.json」，kind=theme）
    json_in_up = os.path.join(UP, "theme.json")
    with open(json_in_up, "wb") as fh:
        fh.write(b'{"v":1}')
    del CALLS[:]
    code, j = flash_post(json_in_up, kind="theme")
    ok(code == 200 and j.get("ok") is True,
       "★ app-upload 下的 theme.json ⇒ %d，ok=%r（要 200/True：这条能力要留着）"
       % (code, j.get("ok")))
    ok(j.get("offset") == "0x210000", "theme.json 刷的还是 0x210000：%r" % (j.get("offset"),))
    ok(len(CALLS) == 1 and CALLS[0][CALLS[0].index("write_flash") + 1] == "0x210000",
       "命令里是 write_flash 0x210000（theme 分区）")

    dotdot = os.path.join(UP, "..", "outside.bin")      # 长得像"在 app-upload 下"的写法
    code, j = flash_post(dotdot)
    ok(code == 400, "app-upload/../outside.bin ⇒ %d（要 400：解完真身就不在里面）" % code)

    big = os.path.join(UP, "too-big.bin")
    with open(big, "wb") as fh:
        fh.truncate(server.FLASH_MAX_BYTES + 1)         # 稀疏文件：不真占 32 MB
    code, j = flash_post(big)
    ok(code == 400, "app-upload 下 %d MB+1 的 .bin ⇒ %d（要 400：超过上限）"
       % (server.FLASH_MAX_BYTES // (1024 * 1024), code))
    os.remove(big)

    ok(server.flash_file_ok(os.path.join(UP, "真的没有这个文件.bin")) is None,
       "不存在的文件 ⇒ flash_file_ok 返回 None（判据在拼命令之前）")
    ok(server.flash_file_ok(evil) is None, "兄弟目录里那个文件 ⇒ flash_file_ok 也是 None")

    # ★ 正常那一次还是要好好工作（run 是假的 ⇒ 只拼命令，不碰板子）
    del CALLS[:]
    code, j = flash_post(UP_BIN)
    ok(code == 200 and j.get("ok") is True,
       "★ app-upload 下的正常 .bin ⇒ %d，ok=%r（要 200/True：能力没被收掉）"
       % (code, j.get("ok")))
    ok(j.get("offset") == "0x210000", "偏移仍然是 0x210000：%r" % (j.get("offset"),))
    cmd = CALLS[0] if CALLS else []
    ok(len(CALLS) == 1 and "write_flash" in cmd and
       cmd[cmd.index("write_flash") + 1] == "0x210000",
       "命令里是 write_flash 0x210000（拼了 %d 条）" % len(CALLS))
    ok(bool(cmd) and os.path.abspath(cmd[-1]) == os.path.abspath(UP_BIN),
       "命令最后一个是刚上传的那个文件（解完真身 = 它自己）")

    # -----------------------------------------------------------------------
    section("⑧ `--repo` 对构建/烧写/预览真生效：run() 的 cwd 是**当场**取 REPO")
    src = open(SERVER_PY, encoding="utf-8").read()
    ok("def run(cmd, timeout=900, cwd=None):" in src,
       "签名是 `def run(cmd, timeout=900, cwd=None)`（默认值不是 REPO）")
    ok(re.search(r"^\s+cwd = cwd or REPO$", src, re.M) is not None,
       "函数体里有 `cwd = cwd or REPO`（用的时候再取）")
    ok(re.search(r"(?m)^def run\([^)]*cwd=REPO", src) is None,
       "★ 没有 `def run(..., cwd=REPO)` 那种 import 时就绑死的默认参数（只看第 0 列的 def）")
    ok("PYLIBS = os.path.join(REPO" not in src and "def pylibs()" in src,
       "PYLIBS 也是**函数取值**（同一类病：模块级常量绑死 REPO）")

    class FakeSub(object):
        """把 subprocess 换成记账的：只为看 run() 传给子进程的 cwd，不真跑任何东西。"""
        PIPE = server.subprocess.PIPE
        STDOUT = server.subprocess.STDOUT

        def __init__(self):
            self.seen = []

        def run(self, cmd, **kw):
            self.seen.append(kw.get("cwd"))

            class R(object):
                returncode = 0
                stdout = b"stub"
            return R()

    real_sub, real_repo = server.subprocess, server.REPO
    fake_sub = FakeSub()
    fake_repo = os.path.join(TMP, "fake-repo")
    os.makedirs(fake_repo, exist_ok=True)
    try:
        server.run = REAL_RUN                 # 拿回真实现（前面几节用的是假 run）
        server.subprocess = fake_sub
        server.REPO = fake_repo               # 模拟 main() 里 `--repo` 之后的样子
        okd, _out = server.run(["echo", "x"])
        ok(okd is True and fake_sub.seen == [fake_repo],
           "改了 REPO 之后 run() 的 cwd 跟着变：%r（要 [%r]）" % (fake_sub.seen, fake_repo))
        ok(bool(fake_sub.seen) and fake_sub.seen[0] != real_repo,
           "★ 不再用 import 时那个旧 REPO（%r）" % (real_repo,))
    finally:
        server.subprocess = real_sub
        server.REPO = real_repo
        server.run = fake_run

    section("⑨ 这个服务只听 127.0.0.1（原来那条边界没动）")
    ok(re.search(r"ThreadingHTTPServer\(\(\"127\.0\.0\.1\", a\.port\), Handler\)", src)
       is not None, "main() 里仍然是 ThreadingHTTPServer((\"127.0.0.1\", a.port), Handler)")
    ok(server.ALLOWED_FLASH == {"theme": 0x210000, "assets": 0x210000, "image": 0x254000},
       "偏移白名单没被这次改动碰过：%r" % (server.ALLOWED_FLASH,))
finally:
    try:
        srv.shutdown()
        srv.server_close()
    except Exception:
        pass
    if SIBLING:
        if SIBLING_CREATED:
            shutil.rmtree(SIBLING, ignore_errors=True)
        else:
            try:
                os.remove(os.path.join(SIBLING, "evil.bin"))
            except OSError:
                pass
    shutil.rmtree(TMP, ignore_errors=True)

print("\n" + "=" * 56)
if FAIL == 0:
    print("全部通过:%d 项判据（没碰串口、没跑 esptool、没往真 scratch 里写东西）" % PASS)
else:
    print("失败 %d 项 / 共 %d 项" % (FAIL, PASS + FAIL))
print("=" * 56)
sys.exit(0 if FAIL == 0 else 1)
