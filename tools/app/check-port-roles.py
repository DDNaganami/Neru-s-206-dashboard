#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
check-port-roles.py -- 「端口下拉标出角色」那套判据的**可执行证明**

    <真 python> tools/app/check-port-roles.py      # 退出码 0 = 全过；1 = 有判据红

车主 2026-10-15 的原话："标出角色吧，分别是新板（未定义），主板，从板，而且你写进板子
这个只有一个按钮，也没区分主/从板啊。"

----------------------------------------------------------------------------
这份脚本证明的是四件事（都是**纯逻辑**，不碰硬件）
----------------------------------------------------------------------------
  ① `SER=` 抠得出来，而且知道**哪个口的 SER 才是板子的 MAC**（乐鑫原生 USB 303A:xxxx；
     CH34x 那种串口桥的 SER 是桥片的序列号 ⇒ 认不出角色就显示"未知"，不硬凑）；
  ② 角色要么来自**现场证据**（板子自己起的 AP 名 `206dash-MASTER|SLAVE` + BSSID =
     那块板的 MAC，或板子自报的 `role=`），要么来自**缓存**（显示"上次已知"+「未确认」）；
  ③ **拿不到证据就是「角色未知」** —— 这一条是本脚本存在的理由（"不要猜"要能被跑出来，
     不能只写在注释里）；
  ④ 主板板载串口（`ACPI\\PNP0501`，没有 SER）标成"不是本项目的板"，**不进"写两块"的目标**。

★ 全程**不打开任何串口**：netsh 那一步也被换成构造的文本（真机上那张网卡连着家里的
  WiFi，跟板子无关，真跑反而说明不了问题）。
"""

import importlib.util
import io
import json
import os
import shutil
import sys
import tempfile

APP_DIR = os.path.dirname(os.path.abspath(__file__))

try:
    sys.stdout.reconfigure(errors="replace")     # GBK 控制台别把脚本打崩
except Exception:
    pass

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


spec = importlib.util.spec_from_file_location("dsh_app_server",
                                              os.path.join(APP_DIR, "server.py"))
server = importlib.util.module_from_spec(spec)
spec.loader.exec_module(server)

# 缓存写到临时目录：**不碰**真机那个 C:\206dash-scratch\app-board-roles.json
TMP = tempfile.mkdtemp(prefix="dsh-roles-")
server.SCRATCH = TMP
server.ROLE_CACHE_PATH = os.path.join(TMP, "app-board-roles.json")


class FakeSub(object):
    """把 server.subprocess 换掉：netsh 的输出由我们给（不真跑）。

    ★ 只换 `run`（`PIPE`/`STDOUT` 那些常量还得是真的）—— 一开始把整个模块换掉，
      wifi_link() 里的 `subprocess.PIPE` 就 AttributeError，而它被 try/except 吞掉，
      表现为"解析不出来"，白查了半天。
    """
    def __init__(self, text):
        self.text = text
        self.PIPE = server.subprocess.PIPE
        self.STDOUT = server.subprocess.STDOUT

    def run(self, *a, **kw):
        text = self.text

        class R(object):
            stdout = text.encode("utf-8")
        return R()


# 今天这台机器上 `pio device list` 的**真实**输出（COM1 是主板板载口）
RAW_PORTS = [
    {"port": "COM1", "detail": "---- Hardware ID: ACPI\\PNP0501\\0 Description: 通信端口 (COM1)"},
    {"port": "COM8", "detail": "---- Hardware ID: USB VID:PID=303A:1001 "
                               "SER=90:70:69:EA:F6:B4 Description: USB 串行设备 (COM8)"},
    {"port": "COM9", "detail": "---- Hardware ID: USB VID:PID=303A:1001 "
                               "SER=80:45:6B:35:A3:C4 Description: USB 串行设备 (COM9)"},
]
HOME_WIFI = "Name                   : WLAN 2\nState                  : connected\n" \
            "SSID                   : MikamoNeru\nAP BSSID               : 10:2f:6e:8a:b1:f2\n"
BOARD_WIFI = "    名称                   : WLAN\n    状态                   : 已连接\n" \
             "    SSID                   : 206dash-SLAVE\n" \
             "    BSSID                  : 90:70:69:ea:f6:b4\n"

# ---------------------------------------------------------------------------
section("① SER= 抠得出来；板载口（ACPI）没有 SER")
p8 = server.parse_port_detail(RAW_PORTS[1]["detail"])
p1 = server.parse_port_detail(RAW_PORTS[0]["detail"])
ok(p8["vid_pid"] == "303A:1001", "COM8 的 VID:PID = 303A:1001（乐鑫原生 USB）")
ok(p8["ser"] == "90:70:69:EA:F6:B4", "COM8 的 SER = 90:70:69:EA:F6:B4（= 板子的 MAC）")
ok(p1["ser"] is None and "PNP0501" in RAW_PORTS[0]["detail"],
   "主板板载口（ACPI\\PNP0501）没有 SER ⇒ 不可能被当成板子")
ok(server.norm_mac("90-70-69-ea-f6-b4") == server.norm_mac("907069EAF6B4") == "90:70:69:EA:F6:B4",
   "MAC 归一化：分隔符/大小写不同也是同一块板")

# ---------------------------------------------------------------------------
section("② 没证据 ⇒ 「角色未知」，不猜（网卡连着家里的 WiFi）")
server.subprocess = FakeSub(HOME_WIFI)
info = server.annotate_ports(RAW_PORTS)
by_port = dict((p["port"], p) for p in info["ports"])
ok(info["wifi"]["ssid"] == "MikamoNeru", "netsh 解析到 SSID（英文键 AP BSSID 那种写法）")
ok(by_port["COM1"]["is_project_board"] is False, "COM1 不是本项目的板")
ok(by_port["COM1"]["label"] == "COM1（不是本项目的板）", "COM1 的标签：" + by_port["COM1"]["label"])
for c in ("COM8", "COM9"):
    ok(by_port[c]["is_project_board"] is True, c + " 是乐鑫原生 USB 口 ⇒ 本项目的板")
    ok(by_port[c]["role"] is None and by_port[c]["role_state"] == "unknown",
       c + " 没有证据 ⇒ role 为空")
    ok(by_port[c]["label"] == c + "（角色未知）", c + " 的标签：" + by_port[c]["label"])
ok(by_port["COM8"]["detail"] == "（角色未知）",
   "detail 那一格就是给编辑器下拉看的角色行（编辑器页面本单不碰）")
ok(by_port["COM1"]["detail"] == "（不是本项目的板）",
   "不是本项目的板也写在 detail 里（编辑器那个下拉同样能看出「这个口别碰」）")

# ---------------------------------------------------------------------------
section("③ 现场证据：板子的 AP 名 + BSSID = 那块板的 MAC ⇒ 认出来并缓存")
server.subprocess = FakeSub(BOARD_WIFI)
info = server.annotate_ports(RAW_PORTS)
by_port = dict((p["port"], p) for p in info["ports"])
ok(info["wifi"]["ssid"] == "206dash-SLAVE", "netsh 解析到板子的 AP 名（中文键那种写法也认）")
ok(by_port["COM8"]["role"] == "SLAVE" and by_port["COM8"]["role_state"] == "confirmed",
   "COM8（SER 与 BSSID 对上）⇒ SLAVE，已确认")
ok(by_port["COM8"]["label"] == "COM8（SLAVE · 左屏 转速+水温）",
   "COM8 的标签：" + by_port["COM8"]["label"])
ok(by_port["COM9"]["role"] is None, "COM9 的 MAC 与 BSSID 对不上 ⇒ 仍然是未知（不牵连）")
cached = json.load(io.open(server.ROLE_CACHE_PATH, encoding="utf-8"))
ok(cached["roles"].get("90:70:69:EA:F6:B4", {}).get("role") == "SLAVE",
   "现场证据写进了缓存（下次哑板也认得）")
ok("板子的 WiFi" in (cached["roles"]["90:70:69:EA:F6:B4"].get("source") or ""),
   "缓存里记着**证据来源**：" + str(cached["roles"]["90:70:69:EA:F6:B4"].get("source")))

# ---------------------------------------------------------------------------
section("④ 只有缓存 ⇒ 显示「上次已知角色」+「未确认」")
server.subprocess = FakeSub(HOME_WIFI)          # 电脑没连板子的 WiFi 了
info = server.annotate_ports(RAW_PORTS)
by_port = dict((p["port"], p) for p in info["ports"])
ok(by_port["COM8"]["role_state"] == "cached", "COM8 走缓存（state=cached）")
ok(by_port["COM8"]["label"] == "COM8（SLAVE · 左屏 转速+水温 · 未确认）",
   "COM8 的标签带「未确认」：" + by_port["COM8"]["label"])
ok(by_port["COM9"]["label"] == "COM9（角色未知）",
   "没缓存过的 COM9 仍然是「角色未知」：" + by_port["COM9"]["label"])

# ---------------------------------------------------------------------------
section("⑤ 板子自报 role=（既有 /api/board-status 那条路）⇒ 只在「连的就是它」时才绑")
server.subprocess = FakeSub(BOARD_WIFI)
note = server.learn_role_from_status({"role": "SLAVE", "fw": "x"})
ok(bool(note) and "SLAVE" in note and "记成了" in note,
   "连的就是那块板子的 AP ⇒ 学下来了：" + str(note))
cached = json.load(io.open(server.ROLE_CACHE_PATH, encoding="utf-8"))
ok(cached["roles"]["90:70:69:EA:F6:B4"]["role"] == "SLAVE", "缓存里是 SLAVE")
ok("板子自报" in cached["roles"]["90:70:69:EA:F6:B4"]["source"], "来源记成「板子自报 role=」")
server.subprocess = FakeSub(HOME_WIFI)
note2 = server.learn_role_from_status({"role": "MASTER"})
ok(note2 is not None and "不是它的 WiFi" in note2,
   "连的是家里路由器时**不写缓存**，只说明原因：" + str(note2))
cached = json.load(io.open(server.ROLE_CACHE_PATH, encoding="utf-8"))
ok(cached["roles"].get("10:2F:6E:8A:B1:F2") is None,
   "★ 没有把**路由器**的 MAC 记成板子角色（这条一开始真踩了：连着家里 WiFi 读 /status）")
ok(cached["roles"].get("80:45:6B:35:A3:C4") is None, "没证据 ⇒ 缓存里没有凭空多出来的 MASTER")

# ---------------------------------------------------------------------------
section("⑥ 两台电脑通用：缓存文件是可读的 JSON，路径就在答复里")
info = server.annotate_ports(RAW_PORTS)
ok(os.path.isfile(server.ROLE_CACHE_PATH), "缓存文件真的写出来了")
ok(info["role_cache"] == server.ROLE_CACHE_PATH, "/api/ports 会把缓存路径告诉前端（可删/可查）")
ok([p["port"] for p in info["ports"] if p["is_project_board"]] == ["COM8", "COM9"],
   "「两块板」的目标只有这两个口（COM1 那个板载口不在里面）")

shutil.rmtree(TMP, ignore_errors=True)

print("\n" + "=" * 56)
if FAIL == 0:
    print("全部通过:%d 项判据（没打开任何串口，netsh 也是构造的文本）" % PASS)
else:
    print("失败 %d 项 / 共 %d 项" % (FAIL, PASS + FAIL))
print("=" * 56)
sys.exit(0 if FAIL == 0 else 1)
