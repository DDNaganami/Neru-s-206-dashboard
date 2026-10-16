#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check-flash-offset.py -- 「写进板子的偏移固定 0x210000，而且界面上改不了它」的可执行判据

    <真 python> tools/app/check-flash-offset.py      # 退出码 0 = 全过；1 = 有判据红

----------------------------------------------------------------------------
为什么要有它（2026-10-16「打包 + 客户端集成」那一单）
----------------------------------------------------------------------------
那一单的红线是：**用户在界面里不能改偏移**（保存安装包 → 写进板子一律 0x210000）。
而这件事**不能靠真刷来验收** —— 车主机器上的 COM8/COM9 正跑着长记录，真刷要占串口。
所以这里把 server.do_flash() 用的 `run()` 换成**只记账、不执行**的假函数，再看它
拼出来的那条命令：于是"偏移取自服务端白名单"这件事是**算出来的**，不是"我看着像"。

★ 它**不做任何真实刷写 / 串口操作**：run() 被换掉之后 esptool 根本不会被启动；
  端口/分区/文件那几条校验都在"拼命令之前"，所以连假函数都不会被调到。
★ 它**不写仓库里的任何文件**：那个用来过"文件必须在仓库或 scratch 内"的假 bin
  放在 SCRATCH 下，跑完删掉。
★ 它只读源码做静态判据的部分（do_POST 那一处调用）用正则 —— 那是**接口形状**的判据：
  页面能传的只有 port/kind/path 三个键，没有第四个。
"""

import importlib.util
import os
import re
import sys

APP_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(APP_DIR))
PAGE = os.path.join(REPO, "tools", "theme-editor", "index.html")

# ★ 中文 Windows 的控制台/重定向用的是 GBK，而这个脚本会打中文与符号（✗ 这类字符
#   GBK 里没有）—— 编码错误降级成替换字符：报告可能缺一个标记，脚本不许倒。
try:
    sys.stdout.reconfigure(errors="replace")
except Exception:
    pass

pass_n = 0
fail_n = 0


def ok(cond, what):
    global pass_n, fail_n
    if cond:
        pass_n += 1
        print("  ok  " + what)
    else:
        fail_n += 1
        print("  BAD " + what)
    return bool(cond)


def section(t):
    print("\n== " + t)


# ---------------------------------------------------------------------------
# 把 server.py 当模块加载（**不**跑 main()：它只在 __main__ 里跑）
# ---------------------------------------------------------------------------
spec = importlib.util.spec_from_file_location("dsh_app_server",
                                              os.path.join(APP_DIR, "server.py"))
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)

CALLS = []


def fake_run(cmd, timeout=900, cwd=None):
    """只记账，不执行 —— 这就是"不碰串口"的那一步。"""
    CALLS.append(list(cmd))
    return True, "stub: 没有真的跑（这是判据，不是刷写）"


server.run = fake_run
SERVER_SRC = open(os.path.join(APP_DIR, "server.py"), encoding="utf-8").read()

# ---------------------------------------------------------------------------
section("① 分区偏移表：assets 就是 0x210000（页面上那条命令的偏移从这来）")
ok(server.ALLOWED_FLASH == {"theme": 0x210000, "assets": 0x210000, "image": 0x254000},
   "ALLOWED_FLASH 恰好是 theme/assets=0x210000、image=0x254000 —— 实际 %r" % (server.ALLOWED_FLASH,))
ok(server.ALLOWED_FLASH.get("assets") == 0x210000, "assets 的偏移是 0x210000")

section("② do_flash('assets') 拼出来的命令：write_flash 0x210000（run 被换成假的）")
tmp_dir = os.path.join(server.SCRATCH, "app-upload")
os.makedirs(tmp_dir, exist_ok=True)
tmp_bin = os.path.join(tmp_dir, "check-flash-offset.bin")
with open(tmp_bin, "wb") as fh:
    fh.write(b"\x00" * 64)                      # 内容无关紧要：只看命令
try:
    del CALLS[:]
    r = server.do_flash("COM8", "assets", tmp_bin)
    ok(r.get("ok") is True, "do_flash 返回 ok=True（实际 %r）" % (r.get("ok"),))
    ok(r.get("offset") == "0x210000", "回给页面的 offset 是 0x210000（实际 %r）" % (r.get("offset"),))
    ok(len(CALLS) == 1, "只拼了 1 条命令（实际 %d 条）" % len(CALLS))
    cmd = CALLS[0] if CALLS else []
    print("  - 命令：" + " ".join(cmd))
    ok("write_flash" in cmd, "命令里有 write_flash")
    i = cmd.index("write_flash") if "write_flash" in cmd else -1
    ok(i >= 0 and cmd[i + 1] == hex(0x210000), "write_flash 后面那一个参数就是 0x210000")
    ok(cmd[-1] == os.path.abspath(tmp_bin), "最后一个是刚落盘的那个文件")
    ok("--port" in cmd and cmd[cmd.index("--port") + 1] == "COM8", "端口用的是页面上选的那个")

    section("③ 页面**传不进**偏移：do_flash 没有这个参数，do_POST 也只传三个键")
    try:
        server.do_flash("COM8", "assets", tmp_bin, offset=0x1000)
        ok(False, "do_flash 竟然收下了 offset= ……（说明有第二个偏移入口）")
    except TypeError:
        ok(True, "do_flash(..., offset=…) 直接 TypeError —— 服务端没有「从页面取偏移」这条路")
    # do_POST 里 /api/flash 那一处调用：截到下一个 if u.path == 为止（不猜括号怎么配）
    at = SERVER_SRC.index('"/api/flash":')
    nxt = SERVER_SRC.find("if u.path ==", at)
    call_src = SERVER_SRC[at:(nxt if nxt > at else at + 300)]
    ok("do_flash(" in call_src, "找得到 do_POST 里 /api/flash 那一处调用")
    print("  - do_POST 那一处：" + " ".join(call_src.split()))
    ok(call_src.count("b.get(") == 3, "恰好三个键来自请求体（port / kind / path）")
    ok("offset" not in call_src, "那一处实参里没有 offset（只有 port / kind / path）")
    ok(re.search(r"off\s*=\s*ALLOWED_FLASH\[kind\]", SERVER_SRC) is not None,
       "偏移取自 ALLOWED_FLASH[kind]（白名单），不是请求体里的任何东西")

    section("④ 端口 / 分区 / 文件三条校验在拼命令**之前**（假 run 一次都不该被调到）")
    for bad_args, what in [
        (("COM8", "bootloader", tmp_bin), "分区不在白名单 ⇒ 拒绝"),
        (("ttyUSB0", "assets", tmp_bin), "端口不是 COM<n> ⇒ 拒绝"),
        (("COM8", "assets", os.path.join(tmp_dir, "真的没有这个文件.bin")), "文件不存在 ⇒ 拒绝"),
    ]:
        del CALLS[:]
        rr = server.do_flash(*bad_args)
        ok(rr.get("ok") is False and len(CALLS) == 0, what + "（实际 ok=%r，跑了 %d 条命令）"
           % (rr.get("ok"), len(CALLS)))
finally:
    try:
        os.remove(tmp_bin)
    except OSError:
        pass

# ---------------------------------------------------------------------------
section("⑤ 编辑器页面那一侧：请求体里根本没有偏移这个字段")
page = open(PAGE, encoding="utf-8").read()
body = re.search(r"JSON\.stringify\(\{\s*port:\s*port,\s*kind:\s*\"assets\",\s*path:\s*up\.path\s*\}\)", page)
ok(bool(body), "刷写请求体恰好是 {port, kind:\"assets\", path}")
if body:
    print("  - 请求体：" + body.group(0))
    ok("offset" not in body.group(0) and "偏移" not in body.group(0),
       "请求体里没有 offset / 偏移 ⇒ 想改也没有入口")
ok('flash: "/api/flash"' in page, "走的是客户端现成的 /api/flash")
ok("kind: \"assets\"" in page, "写的是 kind=assets（= 0x210000 那一段）")
ok(re.search(r'id="owner-flash"\s+style="display:none"', page) is not None,
   "那一块默认隐藏（没有客户端服务时页面只剩原来那条 esptool 命令）")
ok('id="offset"' not in page and "owner-offset" not in page, "页面上没有可改的偏移输入框")

# ---------------------------------------------------------------------------
section("⑥ 走**真的 HTTP 路由**再证一遍：页面连「偏移」这个字段都塞不进来（run 仍是假的）")
#   前面几节是直接调函数；这一节把 server.py 的服务起在 127.0.0.1 的一个临时端口上，
#   用真请求打 POST /api/flash —— 走的就是编辑器页面那条路（do_POST → do_flash）。
#   ★ run() 还是假的 ⇒ 依然**不碰串口、不跑 esptool**。
import json
import threading
import urllib.request
from http.server import ThreadingHTTPServer

server.Handler.log_message = lambda self, fmt, *args: None     # 别把请求日志刷进来
srv = ThreadingHTTPServer(("127.0.0.1", 0), server.Handler)     # 端口 0 = 让系统挑一个空的
srv_port = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()
print("  - 临时服务：http://127.0.0.1:%d（只监听 127.0.0.1）" % srv_port)
tmp2 = os.path.join(tmp_dir, "check-flash-offset-http.bin")


def post(path, body):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (srv_port, path),
                                 data=json.dumps(body).encode("utf-8"),
                                 headers={"Content-Type": "application/json"},
                                 method="POST")
    with urllib.request.urlopen(req, timeout=15) as r:
        return json.loads(r.read().decode("utf-8"))


def write_off(cmd):
    """命令里 write_flash 后面那个偏移；没有就是 None。"""
    if "write_flash" in cmd and len(cmd) > cmd.index("write_flash") + 1:
        return cmd[cmd.index("write_flash") + 1]
    return None


try:
    with open(tmp2, "wb") as fh:
        fh.write(b"\x00" * 64)
    # ① 正常那一次（页面就是这么发的：只有 port / kind / path）
    del CALLS[:]
    j1 = post("/api/flash", {"port": "COM8", "kind": "assets", "path": tmp2})
    print("  - HTTP 那一层拼出来的命令：" + (" ".join(CALLS[0]) if CALLS else "（没有命令！）"))
    ok(j1.get("ok") is True and j1.get("offset") == "0x210000",
       "HTTP 那一层回的就是 0x210000（实际 %r）" % (j1.get("offset"),))
    ok(len(CALLS) == 1 and write_off(CALLS[0]) == "0x210000",
       "真路由拼出来的命令也是 write_flash 0x210000")
    # ② 有人想在请求体里偷偷塞一个偏移 —— 必须**完全没用**
    del CALLS[:]
    j2 = post("/api/flash", {"port": "COM8", "kind": "assets", "path": tmp2,
                             "offset": "0x1000", "偏移": "0x1000", "flash_offset": 4096})
    ok(j2.get("ok") is True, "多塞字段的那次也返回 ok（服务端不因为多余字段报错）")
    ok(len(CALLS) == 1 and write_off(CALLS[0]) == "0x210000",
       "请求体里塞 offset=0x1000 也**改不动**命令：仍然是 write_flash 0x210000")
    ok(j2.get("offset") == "0x210000", "回给页面的 offset 也没被带偏（实际 %r）" % (j2.get("offset"),))
    # ③ 端口还是得由用户选（这是唯一可改的那一格）
    del CALLS[:]
    j3 = post("/api/flash", {"port": "COM9", "kind": "assets", "path": tmp2})
    ok(j3.get("ok") is True and len(CALLS) == 1 and
       CALLS[0][CALLS[0].index("--port") + 1] == "COM9",
       "端口跟着页面选的值走（COM9）—— 可改的只有它")
finally:
    try:
        srv.shutdown()
        srv.server_close()
    except Exception:
        pass
    try:
        os.remove(tmp2)
    except OSError:
        pass

# ---------------------------------------------------------------------------
section("⑦ 客户端外壳那两条路（单块 / 两块）也都没有偏移字段")
#   ★ 2026-10-15 新增「写进两块板」：它**也**只调现成的 /api/flash（kind=assets），
#     偏移仍然由服务端白名单给 —— 这一节就是"没有在客户端另开一个偏移入口"的判据。
js = open(os.path.join(APP_DIR, "app.js"), encoding="utf-8").read()
one = re.search(r"api\('/api/flash', \{ port, kind, path: up\.path \}\)", js)
two = re.search(r"api\('/api/flash', \{ port: b\.port, kind: 'assets', path: up\.path \}\)", js)
ok(bool(one), "单块那条路：/api/flash {port, kind, path}")
ok(bool(two), "两块那条路：/api/flash {port, kind:'assets', path}（逐块 await）")
ok(bool(one) and "offset" not in one.group(0) and bool(two) and "offset" not in two.group(0),
   "两条路的请求体里都没有 offset / 偏移 —— 页面上没有能改偏移的地方")
ok("state.boards" in js and "is_project_board" in js,
   "两块那条路只写认出来的本项目的板（is_project_board），主板板载口不会被写")
ok(not re.search(r"COM\d+\s*[:=]\s*['\"](MASTER|SLAVE)", js),
   "外壳里没有「哪个 COM 号 = 哪个角色」的硬编码（角色只从服务端来）")

# ---------------------------------------------------------------------------
print("\n" + "=" * 56)
if fail_n == 0:
    print("全部通过:%d 项判据（全程没有碰串口、没有跑 esptool）" % pass_n)
else:
    print("失败 %d 项 / 共 %d 项" % (fail_n, pass_n + fail_n))
print("=" * 56)
sys.exit(0 if fail_n == 0 else 1)
