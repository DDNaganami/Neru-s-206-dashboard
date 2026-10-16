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

★ 安全边界（写清楚，别越）：它**只监听 127.0.0.1**，只服务**本仓库目录**（外加内嵌在
  这个程序里的编辑器那 7 个静态文件，见 ALLOWED_EDITOR_FILES），只肯执行下面白名单里的
  命令；不提供任意 shell、不允许路径跳出仓库。
"""

import argparse
import hashlib
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
#   所以"界面自己的文件"要分两种情况找：
#     · 开发态：就在 APP_DIR 里；
#     · exe 态：PyInstaller 用 --add-data 把它们放在 sys._MEIPASS 下的 tools/app。
#   ★ 具体取哪一份由 pages_source() 一处决定（整份用仓库、或整份用包内，不许混搭），
#     见下面那一段的注释；"我发的是哪一份"从 /api/health 的 pages_* 字段看。
FROZEN = getattr(sys, "frozen", False)
BUNDLE_DIR = getattr(sys, "_MEIPASS", APP_DIR)


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

# ---------------------------------------------------------------------------
# 编辑器页面（配色 + 图片）实际会请求的静态文件
#
# ★ 这份清单是**抓服务端请求日志**得来的，不是猜的：起一次服务，用无头 Chrome 打开
#   /tools/app/index.html（外壳里的那个 iframe）与 /tools/theme-editor/index.html、
#   以及老的 /tools/theme-editor/image-editor.html，日志里出现的 GET 就这些：
#     /tools/theme-editor/index.html              （唯一的实现：两个页内分区）
#     /tools/theme-editor/image-editor.html       （老链接的重定向薄壳）
#     /tools/theme-editor/face-stages.js
#     /tools/theme-editor/image-blob-build.js
#     /tools/theme-editor/asset-spec.js
#     /tools/theme-editor/theme-json.js
#     /tools/theme-editor/asset-package-core.js
#   build-exe.ps1 的 --add-data 与这一份一一对应（多一份没用、少一份 404）。
#   /favicon.ico 是浏览器自己要的（本来就 404，不算）；preview/* 是仓库那边的构建
#   产物 ⇒ 那是"预览/构建仍然要求仓库在"的东西，不打包。
# ---------------------------------------------------------------------------
ALLOWED_EDITOR_FILES = ("index.html", "image-editor.html",
                        "face-stages.js", "image-blob-build.js", "asset-spec.js",
                        "theme-json.js", "asset-package-core.js")


def repo_present():
    """仓库在不在：判据就是 platformio.ini（与 find_repo() 同一把尺子）。"""
    return os.path.isfile(os.path.join(REPO, "platformio.ini"))


def repo_need(*parts):
    """缺的那个**具体路径**（给用户看一眼就知道缺什么）。"""
    return os.path.join(REPO, *(parts or ("platformio.ini",)))


def repo_missing_out(what):
    """仓库不在时的统一说法：明说缺哪个路径、还要怎么办 —— **不静默失败**。"""
    return ("★ 找不到仓库：%s 不存在。\n"
            "  %s要用整个仓库（PlatformIO 工程：platformio.ini + lib/ + src/）。\n"
            "  把仓库放到它旁边，或者用 --repo <仓库路径> 启动这个程序，再试。\n"
            % (repo_need(), what))


# ---------------------------------------------------------------------------
# 界面（外壳 + 编辑器）从**哪一份**发 —— 只认一个来源，不许混搭
#
# ★ 2026-10-16（车主实测："点了 写进板子 好像没反应"）：
#   外壳与编辑器**必须是同一份、同一个版本**。以前是"每个文件各自兜底"（这一个文件从
#   仓库、那一个从包内），于是出现"外壳是新的、编辑器是旧的"这类**混搭** ——
#   表现就是"按钮在、点了没反应"，最难查。现在先定**整份**的来源：
#     ① 仓库里 `tools/app` 与 `tools/theme-editor` **都在** ⇒ 整份用**仓库**（开发/更新时就是它）；
#     ② 否则（仓库不在、或它缺这两个目录）⇒ 整份用**包内**（exe 自带的 --add-data 那一份）。
#   哪一份、目录、app.js/编辑器页的 sha 前 8 位与改动时间，都从 /api/health 如实报出来，
#   页面上也显示一行 ⇒ "我看到的到底是哪一份"不用猜。
# ---------------------------------------------------------------------------
def sha8(path):
    try:
        with open(path, "rb") as fh:
            return hashlib.sha256(fh.read()).hexdigest()[:8]
    except Exception:
        return ""


def mtime_text(path):
    try:
        return time.strftime("%Y-%m-%d %H:%M", time.localtime(os.path.getmtime(path)))
    except Exception:
        return ""


def pages_source():
    """(来源, 目录)：`repo` 或 `exe`。判据就是那两个目录**都在**（见上面那段注释）。"""
    repo_app = os.path.join(REPO, "tools", "app")
    repo_ed = os.path.join(REPO, "tools", "theme-editor")
    if os.path.isdir(repo_app) and os.path.isdir(repo_ed):
        return "repo", REPO
    return "exe", BUNDLE_DIR


def editor_markers():
    """只读解析编辑器页面**自己暴露**的版本与能力（那一页的约定，**只加不改语义**）：

        window.__OWNER_MODE_VERSION = "2026-10-15.3"
        window.__OWNER_MODE_CAPS    = { flashNoSave: true, importRestoresImages: true, … }
        <html data-owner-mode-version="…">          （同一个版本号的 DOM 侧副本）

    ★ 为什么服务端也要读一份：外壳靠它**在发出去之前**就知道"我发的编辑器页是哪一版、
      有没有 flashNoSave"—— /api/health 里一眼看得见，排查时不用去点按钮。
    ★ 这里**不改那一页**，只是把它的字读出来（没有这些标记 = 老页面，如实报空）。
    """
    path = os.path.join(pages_source()[1], "tools", "theme-editor", "index.html")
    out = {"editor_version": "", "editor_caps": {}}
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return out
    m = (re.search(r'__OWNER_MODE_VERSION\s*=\s*"([^"]+)"', text)
         or re.search(r'\bOWNER_MODE_VERSION\s*=\s*"([^"]+)"', text)
         or re.search(r'data-owner-mode-version="([^"]+)"', text))
    if m:
        out["editor_version"] = m.group(1)
    m = re.search(r"OWNER_MODE_CAPS\s*=\s*\{([\s\S]*?)\n\};", text)
    if m:
        caps = {}
        for cm in re.finditer(r"([A-Za-z_]\w*)\s*:\s*(true|false)", m.group(1)):
            caps[cm.group(1)] = (cm.group(2) == "true")
        out["editor_caps"] = caps
    return out


def pages_info():
    """给 /api/health 与页面看的一小份"我发的是哪一份"自检。"""
    src, base = pages_source()
    app_js = os.path.join(base, "tools", "app", "app.js")
    ed_html = os.path.join(base, "tools", "theme-editor", "index.html")
    info = {"pages_from": src, "pages_dir": base,
            "app_js": {"sha8": sha8(app_js), "mtime": mtime_text(app_js)},
            "editor_html": {"sha8": sha8(ed_html), "mtime": mtime_text(ed_html)}}
    info.update(editor_markers())
    return info


def app_files_dir():
    """外壳那 4 个文件从哪找（与编辑器同一个来源 —— 不许混搭）。"""
    return os.path.join(pages_source()[1], "tools", "app")


def editor_dirs():
    """编辑器那几个静态文件从哪找（与外壳同一个来源 —— 不许混搭）。

    ★ 仍然返回一个列表（保持原来的调用形状），但正常情况下只有**一项**。
    """
    return [os.path.join(pages_source()[1], "tools", "theme-editor")]


# ---------------------------------------------------------------------------
# 串口 ↔ 角色：哪一块是主板、哪一块是从板
#
# 车主 2026-10-15 的反馈："标出角色吧，分别是新板（未定义），主板，从板，而且你写进
# 板子这个只有一个按钮，也没区分主/从板啊。"
#
# ★ 判据（三条，按证据强弱）：
#   ① **COM 号会变**（Windows 按枚举顺序分配，换口/重插就换号）⇒ **不能**靠它认板子；
#   ② **MAC 不变** ⇒ 认板子用 MAC：`pio device list` 那一行里的 `SER=` 就是板子烧在
#      芯片里的 MAC（303A:1001 这两个口就是；实测 COM9 = SER 80:45:6B:35:A3:C4 = MASTER、
#      COM8 = SER 90:70:69:EA:F6:B4 = SLAVE）。★ 这不是"新写一套串口实现"：
#      pio device list → pyserial 读的就是 Windows 的 PnP 属性（车主截图里那个 USB SER=）。
#   ③ 角色来自**板子自己**：它起的 AP 名就是 `206dash-MASTER|SLAVE`
#      （固件 lib/dashcore/ota_wifi.cpp:186/188），电脑连上那个 AP 时
#      `netsh wlan show interfaces` 给出 SSID + BSSID(= 板子的 MAC) ⇒ 这一对就把
#      "角色 ↔ MAC"绑上了；`/status` 里的 `role=` 是同一件事的第二份证据（既有能力）。
#
# ★ 拿不到证据 ⇒ **显示"角色未知"，绝不猜**（车主那一条）。
# ★ 学到了就**缓存**（SCRATCH 下的一个小 JSON）：板子是哑的、或电脑没连它的 WiFi 时，
#   还能显示"上次已知角色"，并带一个 **未确认** 标记 —— 未确认就是未确认，不装成现场读到的。
# ---------------------------------------------------------------------------
ROLE_CACHE_PATH = os.path.join(SCRATCH, "app-board-roles.json")
ROLE_TEXT = {
    "MASTER": {"side": "主板", "screen": "右屏", "shows": "速度+进气"},
    "SLAVE":  {"side": "从板", "screen": "左屏", "shows": "转速+水温"},
}
# 板子自己起的 AP 名（固件 ota_wifi.cpp：`206dash-<ROLE>`）—— 认角色用它
AP_SSID_ROLE = {"206dash-master": "MASTER", "206dash-slave": "SLAVE"}
# 乐鑫原生 USB（USB-CDC）的 VID 前缀：这种口的 `SER=` 才是**板子的 MAC**
ESP_USB_VID = "303A"


def norm_mac(s):
    """MAC / 序列号统一成大写冒号写法；不是 12 位十六进制就原样大写返回。

    ★ 为什么两种都收：CH34x 那种串口桥的 `SER=` 是**桥片**的序列号，不是板子 MAC。
      它不会等于 AP 的 BSSID ⇒ 天然不会误配（不需要特判），但也认不出角色（显示未知）。
    """
    t = re.sub(r"[^0-9A-Fa-f]", "", str(s or ""))
    if len(t) == 12:
        return ":".join(t[i:i + 2] for i in range(0, 12, 2)).upper()
    return (str(s or "").strip().upper() or None)


def parse_port_detail(detail):
    """从 `pio device list` 那一行里抠出 VID:PID / SER（= MAC）/ 描述。"""
    d = str(detail or "")
    out = {"vid_pid": None, "ser": None, "desc": None}
    m = re.search(r"VID:PID=([0-9A-Fa-f]{4}:[0-9A-Fa-f]{4})", d)
    if m:
        out["vid_pid"] = m.group(1).upper()
    m = re.search(r"\bSER=([0-9A-Fa-f:.\-]+)", d)
    if m:
        out["ser"] = norm_mac(m.group(1))
    m = re.search(r"Description:\s*(.+?)\s*$", d)
    if m:
        out["desc"] = m.group(1)[:120]
    return out


def load_role_cache():
    """角色缓存（MAC ⇒ 角色 + 证据来源 + 时间）。读不出来就是空的 —— 不编。"""
    try:
        with open(ROLE_CACHE_PATH, "r", encoding="utf-8") as fh:
            d = json.load(fh)
    except Exception:
        return {"version": 1, "roles": {}}
    if not isinstance(d, dict):
        return {"version": 1, "roles": {}}
    if not isinstance(d.get("roles"), dict):
        d["roles"] = {}
    d["version"] = 1
    return d


def save_role_cache(d):
    try:
        os.makedirs(SCRATCH, exist_ok=True)
        tmp = ROLE_CACHE_PATH + ".tmp"
        with open(tmp, "w", encoding="utf-8") as fh:
            json.dump(d, fh, ensure_ascii=False, indent=2, sort_keys=True)
        os.replace(tmp, ROLE_CACHE_PATH)
    except OSError:
        pass


def learn_role(mac, role, source):
    """记下"这块 MAC 的角色"。★ 只有**有证据**的调用方才该调它（见下面两处）。"""
    mac = norm_mac(mac)
    role = str(role or "").upper()
    if not mac or role not in ROLE_TEXT:
        return None
    d = load_role_cache()
    cur = d["roles"].get(mac)
    if cur and cur.get("role") == role and cur.get("source") == source:
        return mac
    d["roles"][mac] = {"role": role, "source": source,
                       "at": time.strftime("%Y-%m-%d %H:%M")}
    save_role_cache(d)
    return mac


def wifi_link():
    """电脑现在连着的 WiFi（SSID + BSSID）。读不到就返回空 —— **不猜**。

    ★ BSSID 就是那个 AP 的 MAC；板子的 AP 叫 `206dash-MASTER|SLAVE` ⇒ 这一对是
      "角色 ↔ MAC"的现场证据。字段名 SSID/BSSID 是 ASCII（不随系统语言变），
      解析不依赖中文。
    """
    out = {"ssid": None, "bssid": None}
    try:
        p = subprocess.run(["netsh", "wlan", "show", "interfaces"],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
        text = (p.stdout or b"").decode("utf-8", "replace")
    except Exception:
        return out
    for line in text.splitlines():
        m = re.match(r"^\s*(?:AP\s+)?BSSID\s*:\s*(\S+)\s*$", line, re.I)
        if m:
            out["bssid"] = norm_mac(m.group(1))
            continue
        m = re.match(r"^\s*SSID\s*:\s*(\S.*?)\s*$", line, re.I)
        if m:
            out["ssid"] = m.group(1)
    return out


def live_role_for_ser(wifi, ser):
    """现场证据：SSID 说角色、BSSID 说 MAC，而且**两个都要对得上**才算数。"""
    ssid = (wifi or {}).get("ssid") or ""
    role = AP_SSID_ROLE.get(ssid.strip().lower())
    bssid = norm_mac((wifi or {}).get("bssid"))
    mine = norm_mac(ser)
    if role and bssid and mine and mine == bssid:
        return role
    return None


def learn_role_from_status(st):
    """板子自报的 `role=` + 电脑此刻连着的 AP 的 BSSID(= 板子的 MAC) ⇒ 绑定并缓存。

    ★ 判据要严：**只有"电脑此刻连着的就是**这个角色的**板子 AP"才算证据** ——
      SSID 必须是 `206dash-<同一个 role>` 且 BSSID 拿得到。
      （实测踩过：连着家里路由器去读板子 /status，会把**路由器**的 MAC 记成板子的角色 ——
        那是臆造，不是证据。所以这条写成两半对得上才算。）
    ★ 证据不齐就**不写缓存**，只回一句说明给前端。
    ★ 这是"既有能力"顺带做的事：`/api/board-status` 本来就是读板子 /status 的那条路
      （「看看板子连上没有」/升级后确认成功都用它）—— 没有新开串口、没有新开接口。
    """
    wifi = wifi_link()
    role = str((st or {}).get("role", "") or "").upper()
    if role not in ROLE_TEXT:
        return None
    ssid = (wifi or {}).get("ssid") or ""
    ssid_role = AP_SSID_ROLE.get(ssid.strip().lower())
    mac = norm_mac((wifi or {}).get("bssid"))
    if ssid_role == role and mac:
        learn_role(mac, role, "板子自报 role=%s" % role)
        return ("把 %s（BSSID %s）记成了 %s ⇒ 端口下拉里那块板就带角色了"
                % (ssid, mac, role))
    if ssid_role and ssid_role != role:
        return ("板子自报 role=%s，但它自己的 AP 叫 %s（对不上）⇒ 不写缓存，"
                "先看清是不是同一块板" % (role, ssid))
    return ("读到板子自报 role=%s 了，但电脑现在连的不是它的 WiFi（当前：%s）"
            "⇒ 绑不到具体那个串口，不写缓存" % (role, ssid or "没连 WiFi"))


def role_text(role, state):
    """人话的角色那一格：`MASTER · 右屏 速度+进气`（缓存来的加「未确认」）。"""
    if role not in ROLE_TEXT:
        return "角色未知"
    info = ROLE_TEXT[role]
    t = "%s · %s %s" % (role, info["screen"], info["shows"])
    if state == "cached":
        t += " · 未确认"
    return t


def port_label(port, is_board, role, state):
    """下拉里那一行：`COM9（MASTER · 右屏 速度+进气）` / `COM1（不是本项目的板）`。"""
    if not is_board:
        return "%s（不是本项目的板）" % port
    return "%s（%s）" % (port, role_text(role, state))


def annotate_ports(raw_ports):
    """给"列串口"的结果补上：MAC / 是不是本项目的板 / 角色（+ 证据来源）。

    ★ `detail` 这一格**换成角色那一行**是有意的：编辑器页面的串口下拉直接显示
      `port + detail`（那一页本单不碰）⇒ "下拉里标出角色"这件事在服务端做完就生效。
      原始硬件串留在 `hw` 里，谁想看都还在。
    """
    wifi = wifi_link()
    roles = load_role_cache()["roles"]
    out = []
    for p in raw_ports:
        pnp = parse_port_detail(p.get("detail"))
        ser = pnp["ser"]
        role, state, source = None, "unknown", None
        live = live_role_for_ser(wifi, ser)
        if live:
            role, state = live, "confirmed"
            source = "板子的 WiFi（SSID+BSSID）"
            learn_role(ser, live, source)          # 现场证据 ⇒ 记下来
        elif ser and ser in roles and roles[ser].get("role") in ROLE_TEXT:
            role, state = roles[ser]["role"], "cached"
            source = roles[ser].get("source") or "上次记下来的"
        is_board = bool((pnp["vid_pid"] or "").startswith(ESP_USB_VID)) or bool(ser and ser in roles)
        out.append({
            "port": p.get("port"),
            # detail：给编辑器那个下拉用的**角色那一行**（见上面的注释）。
            #   不是本项目的板也明说 —— 那一页自己只显示 `port + detail`，
            #   所以"是不是板子"这件事也在服务端说清，就不必去改那一页。
            "detail": "（%s）" % ("不是本项目的板" if not is_board else role_text(role, state)),
            "hw": p.get("detail") or "",
            "vid_pid": pnp["vid_pid"],
            "ser": ser,
            "desc": pnp["desc"],
            "is_project_board": is_board,
            "role": role,
            "role_state": state,                   # confirmed / cached / unknown
            "role_text": role_text(role, state),
            "role_source": source,
            "label": port_label(p.get("port"), is_board, role, state),
        })
    return {"ports": out, "wifi": wifi, "role_cache": ROLE_CACHE_PATH}


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
    """列串口。优先 `pio device list`（配方里就是这么用的），失败再退回 pyserial。

    ★ 返回的每一格都带**角色**（哪块是主板/从板）与"是不是本项目的板"——
      判据与缓存的来龙去脉见上面 ROLE_CACHE_PATH 那一段注释。
    ★ `detail` 是给编辑器那个下拉看的**角色那一行**；原始硬件串在 `hw` 里。
    """
    ok, out = run([pio_python(), "-m", "platformio", "device", "list"], timeout=120)
    ports = []
    if ok:
        # 形如：  COM8\n----\nHardware ID: USB VID:PID=303A:1001 SER=90:70:69:EA:F6:B4 ...
        for m in re.finditer(r"^(COM\d+)\s*$([\s\S]*?)(?=^COM\d+\s*$|\Z)", out, re.M):
            ports.append({"port": m.group(1), "detail": " ".join(m.group(2).split())[:160]})
    info = annotate_ports(ports)
    info["ok"] = True
    info["raw"] = out[-1500:]
    info["boards"] = [p for p in info["ports"] if p["is_project_board"]]
    return info


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
    if not repo_present():
        return {"ok": False, "out": repo_missing_out("构建固件")}
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
    # 2026-10-14: **已经在 OTA 模式就别再发 `u`** —— 发 `u` 会重启板子并把 10 分钟窗口
    #   重新计时，屏幕因此多黑一轮（当天就是因为这条，车主对着黑屏等了很久）。
    #   先读一次 /status：`ota_mode=true` 就跳过，并把它如实回给前端。
    _pre = board_status(host)
    _already = bool(_pre.get("ota_mode"))
    if _already:
        sent = {"ok": True, "skipped": True, "why": "board is already in OTA mode"}
    elif port:
        sent = serial_send(port, "u")
    else:
        sent = {"ok": False, "error": "no port given"}
    deadline = t0 + max(5, int(timeout_s))
    last = None
    while time.time() < deadline:
        time.sleep(2.0)
        st = board_status(host)
        if st.get("ota_mode"):
            run_slot, tgt = st.get("running"), st.get("target")
            return {"ok": True, "already_ota": _already, "waited_ms": int((time.time() - t0) * 1000),
                    "before": before or "", "running": run_slot, "target": tgt,
                    "fw": st.get("fw"), "flipped": bool(before) and run_slot != before,
                    "status": st, "serial": sent}
        last = st
    return {"ok": False, "waited_ms": int((time.time() - t0) * 1000),
            "error": "/status 没在 %ss 内回来（板子还在 OTA 模式吗？热点连上了吗？）" % timeout_s,
            "last": last, "serial": sent}

def do_preview():
    """跑宿主机预览（pcpreview）若干秒，生成 preview/frames/*，然后收工。"""
    if not repo_present():
        return {"ok": False, "out": repo_missing_out("跑宿主机预览"), "frames": 0}
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
    def end_headers(self):
        """★ 本地工具页**一律不许缓存**（在一个地方统一加，别在每个 send_header 里抄）。

        车主 2026-10-16 那一课：编辑器页以前是 SimpleHTTPRequestHandler 直接发的
        （带 Last-Modified、**没有** Cache-Control）⇒ 浏览器启发式缓存 ⇒
        "改了代码、重启了客户端，页面上还是旧的"，只有 Ctrl+F5 才更新 ✗。
        这里是本地工具，每次都从磁盘读，正确性比省那几毫秒重要得多。
        """
        try:
            self.send_header("Cache-Control", "no-store, no-cache, must-revalidate")
            self.send_header("Pragma", "no-cache")
            self.send_header("Expires", "0")
        except Exception:
            pass
        super().end_headers()

    def _json(self, obj, code=200):
        body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_file(self, path, ctype):
        data = open(path, "rb").read()
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def _serve_app_file(self, name):
        """外壳自己的文件（index.html / app.css / app.js / README.md）。

        ★ 来源与编辑器**同一个**（见 pages_source()）：整份用仓库，或整份用包内 ——
          不许"外壳新、编辑器旧"这种混搭（那是"点了没反应"的温床）。
        """
        p = os.path.join(app_files_dir(), name)
        if os.path.isfile(p):
            ct = {"html": "text/html; charset=utf-8",
                  "js": "application/javascript; charset=utf-8",
                  "css": "text/css; charset=utf-8"}.get(name.rsplit(".", 1)[-1], "application/octet-stream")
            return self._send_file(p, ct)
        src, base = pages_source()
        return self._json({"ok": False,
                           "error": ("找不到界面文件 %s；来源=%s（%s）\n"
                                     "★ 用 build-exe.ps1 重打一次（它带 --add-data），"
                                     "或者确认仓库里 tools/app 是齐的。" % (name, src, base))}, 404)

    def _serve_theme_file(self, name):
        """编辑器页面（配色 + 图片）那几个静态文件 —— 来源与外壳**同一个**（不许混搭）。

        ★ 为什么不能只让 SimpleHTTPRequestHandler 从 REPO 服务：打包成 exe 之后
          （尤其在没有仓库的机器上）那条路会 404 —— 而"离了仓库仍然能改色、框选、
          导出 assets.bin"是必须的。所以显式按 pages_source() 找，找不到就**明说**。
        """
        if name not in ALLOWED_EDITOR_FILES:
            return self._json({"ok": False, "error": "编辑器文件不在白名单里: %s" % name}, 404)
        for d in editor_dirs():
            p = os.path.join(d, name)
            if os.path.isfile(p):
                ct = {"html": "text/html; charset=utf-8",
                      "js": "application/javascript; charset=utf-8"}.get(
                          name.rsplit(".", 1)[-1], "application/octet-stream")
                return self._send_file(p, ct)
        src, base = pages_source()
        dirs = " ； ".join(editor_dirs()) or "（一个都没有）"
        return self._json({"ok": False,
                           "error": ("找不到编辑器文件 %s；来源=%s（找过：%s）\n"
                                     "★ 用 build-exe.ps1 重打一次（它带 --add-data），"
                                     "或者确认仓库里 tools/theme-editor 是齐的。" % (name, src, dirs))},
                          404)

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
        # 编辑器页面（配色 + 图片）那几个静态文件：仓库优先、包内兜底（见 _serve_theme_file）
        m = re.fullmatch(r"/tools/theme-editor/([A-Za-z0-9._-]+)", u.path)
        if m:
            return self._serve_theme_file(m.group(1))
        if u.path == "/api/health":
            # `repo_ok` 是**判据**：仓库里得有 platformio.ini。
            #   exe 在没有仓库时照样能开界面（应用文件与编辑器都是内嵌的），但
            #   "构建固件 / 跑预览"要用仓库里的工程 —— 所以这两个字段要如实报，
            #   别让用户猜：repo_need = 缺的那个**具体路径**。
            # ★ pages_* 是"**我发的是哪一份界面**"：来源（repo/exe）、目录、
            #   app.js / 编辑器页的 sha 前 8 位与改动时间 —— 都在这儿，不用猜。
            repo_ok = repo_present()
            info = {"ok": True, "repo": REPO, "repo_ok": repo_ok,
                    "repo_need": repo_need(),
                    "editor_from": (editor_dirs() or [""])[0],
                    "frozen": FROZEN, "exe": sys.executable if FROZEN else "",
                    "pio": pio_python(), "proxy": PROXY,
                    "time": time.strftime("%H:%M:%S")}
            info.update(pages_info())
            return self._json(info)
        if u.path == "/api/board-status":
            st = board_status((q.get("host") or [""])[0])
            # ★ 顺手把"角色 ↔ MAC"学下来（既有能力，不新开一条路、不碰串口）：
            #   板子自报 `role=`（固件 /status）+ 电脑此刻连着的 AP 的 BSSID(= 板子的 MAC)
            #   ⇒ 两个证据凑齐就缓存；缺一个就只留一句说明（**不猜**）。
            note = learn_role_from_status(st)
            if note:
                st["_role_note"] = note
            return self._json(st)
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
                # 没有仓库时**明说**缺哪个路径（"没有这个 env 的产物"是另一回事）
                err = ("no bin for %s" % env) if repo_present() else repo_missing_out("取固件产物").strip()
                return self._json({"ok": False, "error": err}, 404)
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
    # ★ 打包成 exe 之后，标准输出/错误被重定向到文件时用的是**本机代码页**
    #   （中文 Windows = GBK），而这里的提示与请求日志里有中文与符号 —— 一旦某个字符
    #   GBK 编不出来，一条提示就能把整个程序打崩（实测踩过：一句 print 里的 "⇒" 直接
    #   UnicodeEncodeError，服务都没起来）。所以把编码错误降级成替换字符：
    #   提示可能缺一个字，程序不许倒。
    for _stream in (sys.stdout, sys.stderr):
        try:
            _stream.reconfigure(errors="replace")
        except Exception:
            pass

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
    if not repo_present():
        print("★ 注意：%s 看起来不是仓库根目录（%s 不存在）——" % (REPO, repo_need()))
        print("  「配色 / 图片」编辑器仍然能打开（应用文件与编辑器都内嵌在这个程序里）；")
        print("  但「构建固件 / 跑预览」要用仓库里的工程：用 --repo <仓库路径> 指一下。")

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
