#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
server.py -- 206Dash 开发台的本地服务（**只用标准库**，不装任何东西）

为什么要一个本地服务，而不是继续双击 .html 用 file:// 打开：
  1. **同源**：把仓库根目录整个挂在一个 http://127.0.0.1 源上，四个页面（配色/图片/
     预览/入口）就成了同源 ⇒ 应用外壳可以用 iframe 把它们收进一个窗口，
     而且将来要跨页读数据也不用跟跨源策略打架（file:// 下每个文件是 opaque origin）。
  2. **安全上下文**：WebSerial / fetch 到开发板的 AP 都需要安全上下文；
     `http://127.0.0.1` 算安全上下文，file:// 在各浏览器上口径不一。
  3. **真正需要后端的那几件事**：列串口、跑 esptool 烧写、跑 platformio 构建/预览
     —— 这些没法在浏览器里干（也不该把 shell 暴露给页面），所以放在这里，
     并且**只认白名单**（env / 偏移 / 操作都在下面写死）。

用法（Windows，双击 tools\\app\\run.bat 也行）：
    python tools/app/server.py                 # 起服务并打开浏览器
    python tools/app/server.py --port 8787 --no-open
    python tools/app/server.py --app-window     # 用 Chrome/Edge 的 --app 无地址栏窗口打开

★ 安全边界（写清楚，别越）：它**只监听 127.0.0.1**，只服务**本仓库目录**，
  只肯执行下面白名单里的命令；不提供任意 shell、不允许路径跳出仓库。
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import webbrowser
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

# ---------------------------------------------------------------------------
# 路径与环境（照抄仓库里那套"能编过"的配方，见 docs 与 tools/build/pio.ps1）
# ---------------------------------------------------------------------------
APP_DIR = os.path.dirname(os.path.abspath(__file__))
# ★ 打包成 exe 之后（`sys.frozen`）`__file__` 指向解包出来的临时目录，
#   所以"应用自己的三个文件"要分两种情况找：
#     · 开发态：就在 APP_DIR 里；
#     · exe 态：PyInstaller 用 --add-data 把它们放在 sys._MEIPASS 下的 tools/app。
FROZEN = getattr(sys, "frozen", False)
BUNDLE_DIR = getattr(sys, "_MEIPASS", APP_DIR)
APP_FILES_DIR = (os.path.join(BUNDLE_DIR, "tools", "app")
                 if os.path.isdir(os.path.join(BUNDLE_DIR, "tools", "app")) else APP_DIR)


def find_repo(explicit=None):
    """找仓库根目录（需要它来服务编辑器/预览页、构建、烧写）。

    顺序（都可解释，别乱改）：
      1. 命令行 `--repo`（打包成 exe 之后最可靠的写法）；
      2. 环境变量 `DSH_206DASH_REPO`；
      3. 从"应用自己所在的位置"往上找：仓库的标志是**同时**有 `platformio.ini`
         与 `tools/app` 目录 —— 只认目录名会把别的东西认成仓库；
      4. 从当前工作目录往上找（有人在仓库里双击 exe 的情形）；
      5. 本机默认路径（开发机上那个）。
    找不到就返回 None —— 界面仍然能打开（应用文件是内嵌的），
    但"构建/烧写/编辑器"会明说"没找到仓库"。
    """
    cands = []
    if explicit:
        cands.append(explicit)
    if os.environ.get("DSH_206DASH_REPO"):
        cands.append(os.environ["DSH_206DASH_REPO"])
    here = os.path.dirname(os.path.abspath(sys.executable if FROZEN else __file__))
    for base in (here, os.getcwd()):
        p = base
        for _ in range(6):                       # 往上最多找 6 层
            cands.append(p)
            p = os.path.dirname(p)
            if not p or p == os.path.dirname(p):
                break
    cands.append(os.path.join(os.path.expanduser("~"), "206Dash", "Neru-s-206-dashboard"))
    cands.append(r"C:\Users\张九思\206Dash\Neru-s-206-dashboard")
    for c in cands:
        if c and os.path.isfile(os.path.join(c, "platformio.ini")) and \
           os.path.isdir(os.path.join(c, "tools", "app")):
            return os.path.abspath(c)
    return None


REPO = find_repo(None) or os.getcwd()
SCRATCH = r"C:\206dash-scratch"                           # ASCII 路径的镜像/工具链（可选）
# 平台核心目录：先看环境变量（谁设的听谁的），再试本机那个 ASCII 镜像，
# 最后退回官方的 ~/.platformio —— 这样这套东西换一台机器也能跑。
PIO_CORE = (os.environ.get("PLATFORMIO_CORE_DIR")
            or (os.path.join(SCRATCH, "pio-core-mix")
                if os.path.isdir(os.path.join(SCRATCH, "pio-core-mix"))
                else os.path.join(os.path.expanduser("~"), ".platformio")))
PIO_PY = os.path.join(PIO_CORE, "penv", "Scripts", "python.exe")
PYLIBS = os.path.join(REPO, ".pio-pylibs")
ZIGBIN = os.path.join(SCRATCH, "zigbin")
# ★ 这台机器直连 github.com 的 TLS 校验会失败（缺中间证书链），拉包必须走本机代理。
PROXY = os.environ.get("HTTPS_PROXY") or "http://127.0.0.1:7890"

# ---- 白名单：环境名 / 分区偏移 / 允许的固件 -------------------------------
# 只有列在这里的东西才可能被执行 —— 页面传来的字符串永远只当"键"用，不当命令拼。
ALLOWED_ENVS = [
    "esp32s3-rgb-slave-ota",     # 从板：带 BLE + 无线刷写
    "esp32s3-rgb-master-ota",    # 主板：带无线刷写
    "esp32s3-rgb-slave-obdtest", # 从板：不带 OTA（现状工作档）
    "esp32s3-rgb-slave-now",
    "esp32s3-rgb-master-now",
    "pcpreview",                 # 宿主机预览（生成 preview/frames）
]
# 分区偏移：只允许这三个（theme / image / 整片 assets 的起点 = theme）
ALLOWED_FLASH = {
    "theme": 0x210000,
    "assets": 0x210000,
    "image": 0x254000,
}
PREVIEW_SECONDS = 40


def pio_python():
    """能跑 platformio 的那个 python（没有就用当前解释器）。"""
    return PIO_PY if os.path.exists(PIO_PY) else sys.executable


def child_env():
    """子进程环境：把仓库那套配方补齐（否则 platformio / esptool 会找不到东西）。"""
    env = dict(os.environ)
    env["PLATFORMIO_CORE_DIR"] = PIO_CORE
    env["PYTHONPATH"] = os.pathsep.join([PYLIBS, PYTHONPATH_EXTRA()]).strip(os.pathsep)
    env["PYTHONUTF8"] = "1"
    env["PYTHONIOENCODING"] = "utf-8"
    env["HTTPS_PROXY"] = PROXY
    env["HTTP_PROXY"] = PROXY
    if os.path.isdir(ZIGBIN):
        env["PATH"] = ZIGBIN + os.pathsep + env.get("PATH", "")
    return env


def PYTHONPATH_EXTRA():
    # 旧脚本用过的 esptoolpy 目录（存在就带上，纯增益）
    p = r"C:\Users\Public\206dash\.pio-core\packages\tool-esptoolpy"
    return p if os.path.isdir(p) else ""


def run(cmd, timeout=900, cwd=REPO):
    """跑一条命令，返回 (ok, output)。输出一律当**文本**返回，前端只负责显示。"""
    try:
        p = subprocess.run(cmd, cwd=cwd, env=child_env(), timeout=timeout,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        out = p.stdout.decode("utf-8", "replace")
        return (p.returncode == 0), out
    except subprocess.TimeoutExpired as e:
        got = (e.stdout or b"").decode("utf-8", "replace")
        return False, got + "\n★ 超时（%ds）被终止\n" % timeout
    except FileNotFoundError as e:
        return False, "命令找不到: %s\n%s" % (cmd[0], e)


# ---------------------------------------------------------------------------
# 业务动作
# ---------------------------------------------------------------------------
def list_ports():
    """列串口。优先 `pio device list`（配方里就是这么用的），失败再退回 pyserial。"""
    ok, out = run([pio_python(), "-m", "platformio", "device", "list"], timeout=120)
    ports = []
    if ok:
        # 形如：  COM8\n----\nHardware ID: USB VID:PID=303A:1001 ...
        for m in re.finditer(r"^(COM\d+)\s*$([\s\S]*?)(?=^COM\d+\s*$|\Z)", out, re.M):
            ports.append({"port": m.group(1), "detail": " ".join(m.group(2).split())[:160]})
    return {"ok": True, "ports": ports, "raw": out[-1500:]}


def list_bins():
    """列出可烧的固件：各 env 的 firmware.bin（大小 + 时间）。"""
    items = []
    for env in ALLOWED_ENVS:
        f = os.path.join(REPO, ".pio", "build", env, "firmware.bin")
        if not os.path.exists(f):
            f = os.path.join(SCRATCH, "now-vanraw", ".pio", "build", env, "firmware.bin")
        if os.path.exists(f):
            st = os.stat(f)
            items.append({"env": env, "path": f, "bytes": st.st_size,
                          "mtime": time.strftime("%Y-%m-%d %H:%M", time.localtime(st.st_mtime)),
                          "ota_ok": env.endswith("-ota")})
    items.sort(key=lambda x: x["mtime"], reverse=True)
    return {"ok": True, "bins": items}


def bin_path_for(env):
    """某个 env 的 firmware.bin 真实路径（仓库内或 scratch 镜像里，只认白名单）。"""
    if env not in ALLOWED_ENVS:
        return None
    for f in (os.path.join(REPO, ".pio", "build", env, "firmware.bin"),
              os.path.join(SCRATCH, "now-vanraw", ".pio", "build", env, "firmware.bin")):
        if os.path.exists(f):
            return f
    return None


def do_build(env):
    if env not in ALLOWED_ENVS:
        return {"ok": False, "out": "env 不在白名单里: %s" % env}
    ok, out = run([pio_python(), "-m", "platformio", "run", "-e", env], timeout=1800)
    return {"ok": ok, "env": env, "out": out[-8000:], "bins": list_bins()["bins"]}


def do_flash(port, kind, path):
    """USB 烧写素材分区（theme / image / assets）。★ 偏移只从白名单取，页面传什么都不信。"""
    if kind not in ALLOWED_FLASH:
        return {"ok": False, "out": "不允许的分区: %s" % kind}
    if not re.fullmatch(r"COM\d+", port or ""):
        return {"ok": False, "out": "端口不合法: %s" % port}
    if not path or not os.path.exists(path):
        return {"ok": False, "out": "文件不存在: %s" % path}
    # 只允许仓库内或 scratch 内的文件
    real = os.path.abspath(path)
    if not (real.startswith(REPO) or real.startswith(SCRATCH)):
        return {"ok": False, "out": "拒绝仓库/scratch 之外的文件: %s" % real}
    off = ALLOWED_FLASH[kind]
    cmd = [pio_python(), "-m", "esptool", "--chip", "esp32s3", "--port", port,
           "--baud", "921600", "write_flash", hex(off), real]
    ok, out = run(cmd, timeout=900)
    return {"ok": ok, "out": out[-6000:], "offset": hex(off)}


def http_json(url, timeout=6):
    """GET one small JSON document (the board's /status). Never raises: the board is
    expected to be unreachable while it reboots, and that is not an error here."""
    import urllib.request
    # 直连，别走 proxy：板子在 192.168.4.1/局域网，proxy 一旦被环境变量打开就会
    #  把请求劫走（实测报 TLSV1_ALERT_INTERNAL_ERROR）—— 这是"读不到板子"的一个真坑。
    _op = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with _op.open(url, timeout=timeout) as r:
            return json.loads(r.read().decode("utf-8", "replace"))
    except Exception as e:
        return {"_error": str(e)}


def board_status(host):
    """Read the board's own bookkeeping: fw / role / running slot / target slot."""
    h = (host or "192.168.4.1").strip().replace("http://", "").strip("/")
    st = http_json("http://%s/status" % h, timeout=5)
    st["_host"] = h
    return st


def serial_send(port, text):
    """Write one command to a board's serial port.

    Why PowerShell and not pyserial: this app is stdlib-only on purpose, and on Windows a
    COM port needs a real DCB setup -- System.IO.Ports.SerialPort does that for us.
    DTR/RTS are held LOW so opening the port does not reset the board (on these native
    USB-CDC boards a reset also parks the chip in the ROM stub, see docs/OTA.md 5.1).
    """
    if not re.fullmatch(r"COM\d+", port or ""):
        return {"ok": False, "error": "bad port (want COM<n>)"}
    ps = ("$p=New-Object System.IO.Ports.SerialPort '%s',115200,'None',8,'One';"
          "$p.DtrEnable=$false;$p.RtsEnable=$false;$p.Open();Start-Sleep -Milliseconds 500;"
          "try{$p.DiscardInBuffer();$p.Write(\"%s`r`n\");Start-Sleep -Milliseconds 300;$p.Close()}"
          "catch{Write-Error $_;exit 1}"
          ) % (port, text)
    try:
        r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                            "-Command", ps], capture_output=True, timeout=30)
        _err = (r.stderr or b"").decode("utf-8", "replace")
        return {"ok": r.returncode == 0 and "Error" not in _err and "Exception" not in _err, "port": port,
                "err": (r.stderr or b"").decode("utf-8", "replace")[-200:]}
    except Exception as e:
        return {"ok": False, "error": str(e)}


def ota_verify(host, port, before, timeout_s=70):
    """Decide whether an upload REALLY landed, using the board's own bookkeeping.

    Why this endpoint exists (learned the hard way, 2026-10-10): `Update` restarts the
    board the moment the write completes, which can kill the HTTP reply in flight -- so a
    failed upload response does NOT mean a failed write, and a browser cannot tell.
    The board's own answer is authoritative: re-enter OTA mode (serial `u`) and read
    /status; if `running`/`target` swapped over, the image went into the other slot.
    """
    t0 = time.time()
    sent = serial_send(port, "u") if port else {"ok": False, "error": "no port given"}
    deadline = t0 + max(5, int(timeout_s))
    last = None
    while time.time() < deadline:
        time.sleep(2.0)
        st = board_status(host)
        if st.get("ota_mode"):
            run_slot, tgt = st.get("running"), st.get("target")
            return {"ok": True, "waited_ms": int((time.time() - t0) * 1000),
                    "before": before or "", "running": run_slot, "target": tgt,
                    "fw": st.get("fw"), "flipped": bool(before) and run_slot != before,
                    "status": st, "serial": sent}
        last = st
    return {"ok": False, "waited_ms": int((time.time() - t0) * 1000),
            "error": "/status 没在 %ss 内回来（板子还在 OTA 模式吗？热点连上了吗？）" % timeout_s,
            "last": last, "serial": sent}

def do_preview():
    """跑宿主机预览（pcpreview）若干秒，生成 preview/frames/*，然后收工。"""
    frames = os.path.join(REPO, "preview", "frames")
    if os.path.isdir(frames):
        for f in os.listdir(frames):
            try:
                os.remove(os.path.join(frames, f))
            except OSError:
                pass
    cmd = [pio_python(), "-m", "platformio", "run", "-e", "pcpreview", "-t", "exec"]
    try:
        p = subprocess.Popen(cmd, cwd=REPO, env=child_env(),
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        try:
            out = p.communicate(timeout=PREVIEW_SECONDS)[0].decode("utf-8", "replace")
        except subprocess.TimeoutExpired:
            subprocess.run(["taskkill", "/PID", str(p.pid), "/T", "/F"],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            out = (p.communicate()[0] or b"").decode("utf-8", "replace")
            out += "\n★ 预览跑满 %ds 已停止（正常）\n" % PREVIEW_SECONDS
    except FileNotFoundError as e:
        return {"ok": False, "out": "起不来: %s" % e, "frames": 0}
    n = len(os.listdir(frames)) if os.path.isdir(frames) else 0
    return {"ok": n > 0, "out": out[-4000:], "frames": n}


# ---------------------------------------------------------------------------
# HTTP
# ---------------------------------------------------------------------------
class Handler(SimpleHTTPRequestHandler):
    server_version = "206dash-app/1.0"

    def __init__(self, *a, **kw):
        super().__init__(*a, directory=REPO, **kw)

    # -- 工具 ---------------------------------------------------------------
    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _send_file(self, path, ctype):
        data = open(path, "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _serve_app_file(self, name):
        """应用自己的文件（index.html / app.css / app.js / check-app.js）：
        先看仓库，再看 exe 内嵌的那一份 —— 这样**没有仓库也能打开界面**。"""
        for d in (os.path.join(REPO, "tools", "app"), APP_FILES_DIR):
            p = os.path.join(d, name)
            if os.path.isfile(p):
                ct = {"html": "text/html; charset=utf-8",
                      "js": "application/javascript; charset=utf-8",
                      "css": "text/css; charset=utf-8"}.get(name.rsplit(".", 1)[-1], "application/octet-stream")
                return self._send_file(p, ct)
        return self._json({"ok": False, "error": "app file not found: %s" % name}, 404)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0:
            return {}
        try:
            return json.loads(self.rfile.read(n).decode("utf-8"))
        except Exception:
            return {}

    def log_message(self, fmt, *args):        # 安静一点：只留一行
        sys.stderr.write("[app] %s\n" % (fmt % args))

    # -- GET ---------------------------------------------------------------
    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        # 应用自己的文件先走内嵌/仓库那份（exe 在没有仓库时也能开界面）
        m = re.fullmatch(r"/tools/app/(index\.html|app\.css|app\.js|README\.md)", u.path)
        if m:
            return self._serve_app_file(m.group(1))
        if u.path == "/api/health":
            # `repo_ok` 是**判据**：仓库里得有 platformio.ini。
            #   exe 在没有仓库时照样能开界面（应用文件是内嵌的），但"构建/烧写/编辑器"
            #   会打不开 —— 所以这个字段要如实报，别让用户猜。
            repo_ok = os.path.isfile(os.path.join(REPO, "platformio.ini"))
            return self._json({"ok": True, "repo": REPO, "repo_ok": repo_ok,
                               "frozen": FROZEN, "exe": sys.executable if FROZEN else "",
                               "pio": pio_python(), "proxy": PROXY,
                               "time": time.strftime("%H:%M:%S")})
        if u.path == "/api/board-status":
            return self._json(board_status((q.get("host") or [""])[0]))
        if u.path == "/api/ports":
            return self._json(list_ports())
        if u.path == "/api/bins":
            return self._json(list_bins())
        if u.path == "/api/bin":
            # 把某个 env 的 firmware.bin 直接吐给页面（OTA 要用它当上传体）。
            # 为什么不让页面自己拼静态路径：产物可能在 scratch 镜像里，静态服务够不着。
            env = (q.get("env") or [""])[0]
            f = bin_path_for(env)
            if not f:
                return self._json({"ok": False, "error": "no bin for %s" % env}, 404)
            data = open(f, "rb").read()
            self.send_response(200)
            self.send_header("Content-Type", "application/octet-stream")
            self.send_header("Content-Length", str(len(data)))
            self.send_header("Content-Disposition", 'attachment; filename="firmware.bin"')
            self.end_headers()
            self.wfile.write(data)
            return
        if u.path == "/":
            self.send_response(302)
            self.send_header("Location", "/tools/app/index.html")
            self.end_headers()
            return
        return super().do_GET()

    # -- POST --------------------------------------------------------------
    def do_POST(self):
        u = urlparse(self.path)
        if u.path == "/api/upload":
            return self._upload()
        b = self._body()
        if u.path == "/api/build":
            return self._json(do_build(str(b.get("env", ""))))
        if u.path == "/api/flash":
            return self._json(do_flash(str(b.get("port", "")), str(b.get("kind", "")),
                                       str(b.get("path", ""))))
        if u.path == "/api/ota-verify":
            return self._json(ota_verify(str(b.get("host", "")), str(b.get("port", "")),
                                       str(b.get("before", "")), int(b.get("timeout_s", 70) or 70)))
        if u.path == "/api/preview":
            return self._json(do_preview())
        if u.path == "/api/quit":
            # 打包成 exe 之后没有 Ctrl+C 可用（尤其 --noconsole），所以给界面一个"退出"按钮。
            self._json({"ok": True, "bye": "服务即将退出"})
            threading.Thread(target=self.server.shutdown, daemon=True).start()
            return
        return self._json({"ok": False, "error": "unknown endpoint"}, 404)

    def _upload(self):
        """页面选的文件没有真实路径 ⇒ 先传到 scratch 里落盘，再交给 esptool。
        只接单文件、大小上限 16MB（分区最大就是 8MB + 余量）。"""
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n > 16 * 1024 * 1024:
            return self._json({"ok": False, "error": "坏的长度: %d" % n}, 400)
        name = os.path.basename(self.headers.get("X-Filename") or "upload.bin")
        name = re.sub(r"[^A-Za-z0-9._-]", "_", name)[:80] or "upload.bin"
        d = os.path.join(SCRATCH, "app-upload")
        os.makedirs(d, exist_ok=True)
        dst = os.path.join(d, name)
        with open(dst, "wb") as fh:
            left = n
            while left > 0:
                chunk = self.rfile.read(min(65536, left))
                if not chunk:
                    break
                fh.write(chunk)
                left -= len(chunk)
        return self._json({"ok": True, "path": dst, "bytes": os.path.getsize(dst)})


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--no-open", action="store_true")
    ap.add_argument("--repo", default=None, help="仓库根目录（打包成 exe 之后建议显式给）")
    ap.add_argument("--app-window", action="store_true",
                    help="用 Chrome/Edge 的 --app 打开（无地址栏，像原生窗口）")
    a = ap.parse_args()

    global REPO
    if a.repo:
        REPO = os.path.abspath(a.repo)
    if not os.path.isfile(os.path.join(REPO, "platformio.ini")):
        print("★ 注意：%s 看起来不是仓库根目录（没有 platformio.ini）——" % REPO)
        print("  「配色 / 图片 / 预览」这些页会打不开；用 --repo <仓库路径> 指定一下。")

    url = "http://127.0.0.1:%d/tools/app/index.html" % a.port
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)

    print("=" * 66)
    print("206Dash 开发台%s" % ("（打包版）" if FROZEN else ""))
    print("  仓库 : %s" % REPO)
    print("  地址 : %s" % url)
    print("  停止 : %s" % ("界面右下角「退出应用」" if FROZEN else "Ctrl+C（或界面里的「退出应用」）"))
    print("=" * 66)

    if not a.no_open:
        def open_it():
            time.sleep(0.6)
            if a.app_window:
                for exe in (r"C:\Program Files\Google\Chrome\Application\chrome.exe",
                            r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
                            r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
                            r"C:\Program Files\Microsoft\Edge\Application\msedge.exe"):
                    if os.path.exists(exe):
                        subprocess.Popen([exe, "--app=" + url, "--window-size=1280,900"])
                        return
            webbrowser.open(url)
        threading.Thread(target=open_it, daemon=True).start()

    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\n已停止。")


if __name__ == "__main__":
    main()
