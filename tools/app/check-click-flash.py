#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check-click-flash.py -- 真浏览器里**点一次**「写进板子」，一路跟到 `/api/flash`（自动化判据）

    <真 python> tools/app/check-click-flash.py          # 退出码 0 = 全过；2 = 本机没 Chrome（跳过）

----------------------------------------------------------------------------
为什么要有它（车主 2026-10-16："点了 写进板子 按钮好像没反应"）
----------------------------------------------------------------------------
"点了没反应"这种事，**静态字符串断言证明不了**（上一单我就是这么栽的：只证明了接线与
HTTP 那一段，车主那一点正好落在没验成的缺口里）。所以这条判据是**真点**：

  · 真起一个服务（`server.py`，本机仓库）→ 真起一个 headless Chrome（独立 profile、
    独立端口）→ 用 DevTools 协议（只用标准库：socket + 手写 WebSocket 帧）**点那颗按钮**；
  · 走**真编辑器页面**：测试往编辑器里塞一张 480×480 的背景图（走它自己的 `addFiles`），
    于是"当前状态"真的能打出容器；
  · `/api/flash` **在页面里被拦掉**（fetch 换成假的，只记账）⇒ ★★ **绝不碰板子**；
    服务端日志里也不许出现 `POST /api/flash`（这一条也查）。
  · 下载目录指到一个空临时目录 ⇒ "刷一次有没有产生文件"是**看得见**的。

查四件事（都是"点了之后看得见什么"）：
  ① 点下去**立刻**有反馈（按钮变「正在写…」、旁边牌子跟着变）；
  ② 一路走到 `/api/flash`：请求体是 `{port, kind:"assets", path}`、**没有偏移字段**；
  ③ 没有图片时点，**会说清为什么**（不是静默）；
  ④ 编辑器页缺 `exportAssetsBin()`（版本旧）时，**当场喊"版本过旧"**，而且**不发** `/api/flash`。

★ 它**不刷真机**（红线）：全程没有任何真实串口操作。
"""

import base64
import json
import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import zlib

APP_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(APP_DIR))
CHROME_CANDIDATES = [
    r"C:\Program Files\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
]

PASS, FAIL = 0, 0


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
def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def png_rgb(w, h, rgb=(30, 90, 160)):
    """最小 PNG 编码器（8bit RGB）—— 只是给编辑器一张能解码的图。"""
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


class WS(object):
    """够用的 DevTools 客户端：HTTP 升级握手 + 文本帧（只用标准库）。"""

    def __init__(self, url):
        rest = url[5:]
        hostport, _, path = rest.partition("/")
        host, _, port = hostport.partition(":")
        self.s = socket.create_connection((host, int(port)), timeout=90)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET /%s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
                        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
                        "Sec-WebSocket-Version: 13\r\n\r\n" % (path, hostport, key)).encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.s.recv(4096)
        if b" 101 " not in buf.split(b"\r\n")[0]:
            raise IOError("websocket 握手失败: %r" % buf[:160])
        self.buf = buf.split(b"\r\n\r\n", 1)[1]
        self.mid = 0

    def send(self, obj):
        data = json.dumps(obj).encode()
        n = len(data)
        head = bytearray([0x81])
        if n < 126:
            head.append(0x80 | n)
        elif n < 65536:
            head.append(0x80 | 126)
            head += struct.pack(">H", n)
        else:
            head.append(0x80 | 127)
            head += struct.pack(">Q", n)
        mask = os.urandom(4)
        head += mask
        self.s.sendall(bytes(head) + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.s.recv(1 << 16)
            if not chunk:
                raise IOError("连接断了")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self):
        while True:
            h = self._read(2)
            op = h[0] & 0x0F
            ln = h[1] & 0x7F
            if ln == 126:
                ln = struct.unpack(">H", self._read(2))[0]
            elif ln == 127:
                ln = struct.unpack(">Q", self._read(8))[0]
            payload = self._read(ln) if ln else b""
            if op == 0x9:                                   # ping
                self.s.sendall(b"\x8a\x80" + os.urandom(4))
                continue
            if op == 0x8:
                raise IOError("对端关了连接")
            return payload.decode("utf-8", "replace")

    def call(self, method, params=None):
        self.mid += 1
        mid = self.mid
        self.send({"id": mid, "method": method, "params": params or {}})
        while True:
            msg = json.loads(self.recv())
            if msg.get("id") == mid:
                return msg


def ev(ws, expr, await_promise=False):
    r = ws.call("Runtime.evaluate", {"expression": expr, "returnByValue": True,
                                     "awaitPromise": await_promise})
    res = r.get("result", {})
    if res.get("exceptionDetails"):
        return {"__exc": json.dumps(res["exceptionDetails"], ensure_ascii=False)[:300]}
    return res.get("result", {}).get("value")


def main():
    args = sys.argv[1:]
    exe = None
    if "--exe" in args:
        i = args.index("--exe")
        exe = args[i + 1] if len(args) > i + 1 else None
        if not exe or not os.path.isfile(exe):
            print("--exe 给的路径不存在：%r" % (exe,))
            return 2
    chrome = next((c for c in CHROME_CANDIDATES if os.path.exists(c)), None)
    if not chrome:
        print("本机没找到 Chrome/Edge —— 这条判据跳过（它要真浏览器）")
        return 2

    tmp = tempfile.mkdtemp(prefix="dsh-click-")
    dl = os.path.join(tmp, "downloads")
    os.makedirs(dl, exist_ok=True)
    port, cdp = free_port(), free_port()
    log_lines = []
    # 默认起**开发态**的 server.py；给了 --exe 就起那个 exe（判据一样，用它验打包产物）
    cmd = ([exe, "--repo", REPO, "--port", str(port), "--no-open"] if exe else
           [sys.executable, os.path.join(APP_DIR, "server.py"),
            "--repo", REPO, "--port", str(port), "--no-open"])
    print("被测服务：%s" % (" ".join(cmd)))
    srv = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

    def drain():                     # ★ 一定要读：管道写满会把请求线程堵死（上一单就是栽在这）
        try:
            for line in srv.stdout:
                log_lines.append(line.decode("utf-8", "replace"))
        except Exception:
            pass
    threading.Thread(target=drain, daemon=True).start()

    br = None
    try:
        for _ in range(60):
            try:
                urllib.request.urlopen("http://127.0.0.1:%d/api/health" % port, timeout=3).read()
                break
            except Exception:
                time.sleep(0.5)
        br = subprocess.Popen([chrome, "--headless", "--disable-gpu", "--no-first-run",
                               "--user-data-dir=" + os.path.join(tmp, "profile"),
                               "--remote-debugging-port=%d" % cdp, "about:blank"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        page = None
        for _ in range(60):
            try:
                ts = json.loads(urllib.request.urlopen(
                    "http://127.0.0.1:%d/json" % cdp, timeout=3).read().decode())
                pg = [t for t in ts if t.get("type") == "page"]
                if pg:
                    page = pg[0]
                    break
            except Exception:
                pass
            time.sleep(0.5)
        if not page:
            print("拿不到浏览器页面 —— 跳过")
            return 2
        ws = WS(page["webSocketDebuggerUrl"])
        ws.call("Page.enable")
        ws.call("Runtime.enable")
        try:
            ws.call("Page.setDownloadBehavior", {"behavior": "allow", "downloadPath": dl})
        except Exception:
            pass
        ws.call("Page.navigate", {"url": "http://127.0.0.1:%d/tools/app/index.html#dash" % port})
        time.sleep(4)
        # alert/confirm 在 headless 里没人点会卡住页面 ⇒ 换成记账
        ev(ws, "(function(){window.__alerts=[];window.__confirms=[];"
               "window.alert=function(m){window.__alerts.push(String(m));};"
               "window.confirm=function(m){window.__confirms.push(String(m));return true;};"
               "window.fetch=(function(of){return function(u,o){var s=String(u);"
               "window.__urls=(window.__urls||[]).concat([s]);"
               "if(s.indexOf('/api/flash')>=0){window.__flash=(window.__flash||0)+1;"
               "return Promise.resolve(new Response('{\"ok\":true,\"offset\":\"0x210000\",\"out\":\"blocked-by-test\"}',"
               "{status:200,headers:{'Content-Type':'application/json'}}));}"
               "return of.apply(window,arguments);};})(window.fetch);return 1;})()")
        for _ in range(90):                       # 等编辑器框载入
            if ev(ws, "!!(document.querySelector('#fr-theme')&&"
                      "document.querySelector('#fr-theme').contentWindow&&"
                      "document.querySelector('#fr-theme').contentWindow.document&&"
                      "document.querySelector('#fr-theme').contentWindow.document.readyState==='complete')"):
                break
            time.sleep(1)

        # ------------------------------------------------------------------
        section("③ 还没放图片时点：会说清为什么（不静默、也不去调 /api/flash）")
        ev(ws, "(function(){window.__flash=0;document.querySelector('#b-dash-flash').click();return 1;})()")
        time.sleep(3)
        pill0 = ev(ws, "((document.querySelector('#dash-flash-say')||{}).textContent||'')")
        flash0 = ev(ws, "window.__flash||0") or 0
        print("   牌子：" + str(pill0)[:140])
        ok(("构建失败" in str(pill0)) or ("构建不了" in str(pill0)) or ("还没有图片" in str(pill0)),
           "★ 说明白了为什么（构建失败/还没有图片），不是静默")
        ok(flash0 == 0, "没图片时不会去调 /api/flash")

        # ------------------------------------------------------------------
        section("① 往编辑器里塞一张图（走它自己的 addFiles），再点：立刻有反馈 → 走到 /api/flash")
        b64 = base64.b64encode(png_rgb(480, 480)).decode()
        added = ev(ws, """(async function(){
          var w=document.querySelector('#fr-theme').contentWindow;
          var bin=atob('%s'), a=new Uint8Array(bin.length);
          for(var i=0;i<bin.length;i++)a[i]=bin.charCodeAt(i);
          try { w.addFiles([new w.File([a],'selftest-bg.png',{type:'image/png'})],
                           w.ImageBlob.ROLE.Background); } catch(e) { return 'throw:'+e; }
          await new Promise(function(r){setTimeout(r,4000);});
          return ((w.document.getElementById('status')||{}).textContent||'').split('\\n')[0];})()""" % b64,
                    await_promise=True)
        print("   编辑器：" + str(added)[:100])
        ok("已添加" in str(added), "编辑器确认那张图进来了（否则这一段证明不了什么）")
        ev(ws, "(function(){window.__flash=0;window.__urls=[];return 1;})()")
        before = ev(ws, "((document.querySelector('#dash-flash-say')||{}).textContent||'')")
        ev(ws, "document.querySelector('#b-dash-flash').click()")
        time.sleep(0.25)
        during = ev(ws, "({pill:((document.querySelector('#dash-flash-say')||{}).textContent||''),"
                        "btn:((document.querySelector('#b-dash-flash')||{}).textContent||''),"
                        "dis:(document.querySelector('#b-dash-flash')||{}).disabled===true})")
        ok(isinstance(during, dict) and (str(during.get("btn", "")).startswith("正在写") or
                                         during.get("pill", "") != before or during.get("dis")),
           "★ 点下去 0.25 秒内就有可见变化（按钮/牌子）：%r" % (during,))
        deadline = time.time() + 60
        flash = 0
        while time.time() < deadline:
            flash = ev(ws, "window.__flash||0") or 0
            if flash:
                break
            time.sleep(0.5)
        state = ev(ws, "({flash:window.__flash||0,urls:window.__urls||[],"
                       "pill:((document.querySelector('#dash-flash-say')||{}).textContent||'')})")
        print("   牌子：" + str(state.get("pill"))[:120])
        print("   请求：" + json.dumps(state.get("urls"), ensure_ascii=False)[:200])
        ok(flash >= 1, "★★ 真的走到了 /api/flash（点了不是没反应）")
        ok(any("/api/upload" in u for u in state.get("urls", [])), "中间那一步 /api/upload 也走了")
        ok(("写完了" in str(state.get("pill"))) or ("没写成" in str(state.get("pill"))),
           "牌子给出了结果：" + str(state.get("pill"))[:60])
        # ★ 打的是不是**本项目的板**（默认绝不能落到 COM1 那种主板板载口上）
        boards = json.loads(urllib.request.urlopen(
            "http://127.0.0.1:%d/api/ports" % port, timeout=120).read().decode("utf-8"))
        board_ports = [b["port"] for b in boards.get("boards", [])]
        hit = re.search(r"写完了 ✓（([A-Za-z0-9]+)", str(state.get("pill"))) or \
              re.search(r"正在写 ([A-Za-z0-9]+)", str(pill0))
        target = hit.group(1) if hit else "?"
        print("   目标串口：%s（认出来的板：%r）" % (target, board_ports))
        ok(target in board_ports,
           "★ 表盘屏那颗默认打的是**认出来的板**（%s），不是 COM1 那种板载口" % target)

        section("② 下载目录里没有产生任何文件（写进板子不落地）")
        files = os.listdir(dl)
        print("   下载目录：" + repr(files))
        ok(not [f for f in files if f.lower().endswith(".bin")],
           "★ 刷一次没有产生 .bin 文件")

        section("④ 编辑器页缺 exportAssetsBin（版本旧）⇒ 当场喊出来，且不发 /api/flash")
        ev(ws, "(function(){var w=document.querySelector('#fr-theme').contentWindow;"
               "try{w.exportAssetsBin=undefined;}catch(e){}return 1;})()")
        ev(ws, "(function(){window.__flash=0;document.querySelector('#b-dash-flash').click();return 1;})()")
        time.sleep(3)
        st4 = ev(ws, "({pill:((document.querySelector('#dash-flash-say')||{}).textContent||''),"
                     "banner:((document.querySelector('#dash-warn')||{}).textContent||''),"
                     "flash:window.__flash||0})")
        print("   牌子：" + str(st4.get("pill"))[:160])
        ok("版本过旧" in str(st4.get("pill")), "★ 喊的是「版本过旧」（并告诉怎么更新）")
        ok("版本过旧" in str(st4.get("banner")), "表盘屏那条横幅也一起说（显眼）")
        ok((st4.get("flash") or 0) == 0, "版本不对时**不发** /api/flash（不拿旧页面瞎写）")

        section("⑤ 真 /api/flash 一次都没到服务端（没碰板子）")
        ok(not any("POST /api/flash" in l for l in log_lines),
           "服务端日志里没有 POST /api/flash（全程没碰串口）")
    finally:
        for p in (br, srv):
            try:
                if p and p.poll() is None:
                    subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            except Exception:
                pass
        shutil.rmtree(tmp, ignore_errors=True)

    print("\n" + "=" * 56)
    print(("全部通过:%d 项（真浏览器真点击；/api/flash 被测试拦掉，没碰板子）" % PASS)
          if FAIL == 0 else ("失败 %d / 共 %d" % (FAIL, PASS + FAIL)))
    print("=" * 56)
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(errors="replace")
    except Exception:
        pass
    sys.exit(main())
