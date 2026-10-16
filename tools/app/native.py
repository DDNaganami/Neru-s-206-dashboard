#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
native.py -- 用**系统自带的 WebView2** 开一个真原生窗口（不再是浏览器标签页）

车主 2026-10-16："打开 exe 为什么还是在浏览器里进行操作呢" ⇒ 这一层就是"原生窗口"。

----------------------------------------------------------------------------
它做三件事（都不动 server.py 的既有边界）
----------------------------------------------------------------------------
1. **起同一个本地服务**：直接 `import server`，用它的 `Handler` 起
   `ThreadingHTTPServer(("127.0.0.1", port), …)` —— **仍然只听 127.0.0.1**（一个字没改），
   路由 / 白名单 / 偏移 / `POST /api/quit` 全是原来那一份（**没有第二套服务**）。
2. **开窗口**：pywebview —— Windows 上走 **Edge WebView2**：那是**系统已经装好**的运行时
   （注册表里的 `Microsoft\\EdgeWebView`），**不自带浏览器内核**，跟 Electron / Tauri
   不是一回事。
3. **关窗 = 停服务**：窗口一关就把 HTTP 服务 `shutdown()`；反过来，界面上那颗
   「退出应用」（`POST /api/quit`）把服务停掉时，窗口也一起关 —— 两条路**同一个结果**，
   绝不出现"窗口关了服务还在"。

★ WebView2 没装 / 窗口起不来 / 连 pywebview 都没装 ⇒ **明确打印一段话
  （说清缺什么、怎么办）+ 回退到系统浏览器**（老行为），**绝不静默失败** ✗。
★ `--devtools` 才开 DevTools（**默认关**）；平时要刷新就用界面上那颗
  「重新载入这个窗口」（服务端已经 no-cache，普通 reload 就能拿到新版）。
★ 这个 exe 仍然是 `--console` 构建（日志是这工具的一部分），窗口是额外开的那一个。
"""

import argparse
import os
import sys
import threading
import time
import webbrowser
from http.server import ThreadingHTTPServer

APP_DIR = os.path.dirname(os.path.abspath(__file__))
if APP_DIR not in sys.path:
    sys.path.insert(0, APP_DIR)

import server as app_server          # noqa: E402  （同一个服务，不是第二套）

WINDOW_TITLE = "206Dash 表盘助手"


def start_http(port):
    """把**原来那个** Handler 起在 127.0.0.1 上（只听本机，与 server.py 完全一致）。

    返回 (srv, stopped_event)。`stopped` 在服务被 shutdown 时置位 —— 界面里的
    「退出应用」走的就是 `srv.shutdown()`（`POST /api/quit`），窗口据此一起关。
    """
    srv = ThreadingHTTPServer(("127.0.0.1", port), app_server.Handler)
    stopped = threading.Event()
    real_shutdown = srv.shutdown

    def shutdown_and_flag():
        stopped.set()
        real_shutdown()

    srv.shutdown = shutdown_and_flag
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, stopped


def open_in_browser(url, why):
    """★ 回退路径：窗口开不了就**明说**，再用系统浏览器打开（老行为）。"""
    print("")
    print("=" * 66)
    print("★ 开不了原生窗口：" + why)
    print("  两种办法（都不影响功能，页面在浏览器里一样用）：")
    print("    ① 装一下 WebView2 运行时（系统自带的 Edge 一般已经有了；")
    print("       没有就去 https://developer.microsoft.com/microsoft-edge/webview2/ 下 Evergreen）")
    print("    ② 或者用参数强制走浏览器：native.py --browser")
    print("  现在先用**系统浏览器**打开：" + url)
    print("=" * 66)
    try:
        webbrowser.open(url)
    except Exception as e:
        print("  （连浏览器都没打开：%s —— 手动访问上面那个地址即可）" % e)


def main():
    # GBK 控制台别把程序打崩（踩过）；顺便**行缓冲**——输出被重定向到文件时也要能及时看到
    # （"窗口起不来"这类话必须当场看得见，别憋在缓冲区里）。
    for _s in (sys.stdout, sys.stderr):
        try:
            _s.reconfigure(errors="replace", line_buffering=True)
        except Exception:
            pass

    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8787)
    ap.add_argument("--repo", default=None, help="仓库根目录（打包成 exe 之后建议显式给）")
    ap.add_argument("--devtools", action="store_true", help="开 DevTools（默认关）")
    ap.add_argument("--browser", action="store_true", help="强制用系统浏览器（跳过原生窗口）")
    ap.add_argument("--no-window", action="store_true",
                    help="只起服务、不开任何窗口（给自动化测试/只想用别的浏览器看的人）")
    a = ap.parse_args()

    if a.repo:
        app_server.REPO = os.path.abspath(a.repo)
    if not app_server.repo_present():
        print("★ 注意：%s 看起来不是仓库根目录（%s 不存在）——"
              % (app_server.REPO, app_server.repo_need()))
        print("  「配色 / 图片」编辑器仍然能打开；构建固件 / 跑预览要用仓库里的工程。")

    url = "http://127.0.0.1:%d/tools/app/index.html" % a.port
    srv, stopped = start_http(a.port)

    print("=" * 66)
    print("206Dash 开发台%s" % ("（打包版）" if app_server.FROZEN else ""))
    print("  仓库 : %s" % app_server.REPO)
    print("  地址 : %s" % url)
    print("  窗口 : %s" % ("系统浏览器（--browser）" if a.browser else "原生窗口（WebView2）"))
    print("  停止 : 关掉窗口，或界面右下角「退出应用」（两条路都会把服务停掉）")
    print("=" * 66)

    if a.no_window:
        print("只起服务、不开窗口（--no-window）：%s  —— Ctrl+C 停止。" % url)
        wait_forever(stopped, srv)
        return

    if a.browser:
        open_in_browser(url, "你指定了 --browser")
        wait_forever(stopped, srv)
        return

    try:
        import webview                                    # noqa: F401
    except Exception as e:
        open_in_browser(url, "没装 pywebview（%s）" % e)
        wait_forever(stopped, srv)
        return

    try:
        win = webview.create_window(WINDOW_TITLE, url,
                                    width=1280, height=900, min_size=(980, 680))

        def on_closed():
            # ★ 关窗 = 停服务（与界面里的「退出应用」同一个结果）
            print("窗口关了 —— 正在停止本地服务…")
            try:
                srv.shutdown()          # start_http() 里包过：置 stopped + 真正 shutdown
            except Exception:
                pass

        try:
            win.events.closed += on_closed
        except Exception:
            pass                                      # 老版本 pywebview 没 events：还有下面的看门狗

        def watchdog():
            # 界面里点「退出应用」⇒ 服务停了 ⇒ 窗口也一起关（不留一个空窗口）
            stopped.wait()
            try:
                win.destroy()
            except Exception:
                pass
        threading.Thread(target=watchdog, daemon=True).start()

        print("正在打开原生窗口（WebView2）…")
        webview.start(gui="edgechromium", debug=bool(a.devtools))
        print("窗口已关闭。")
    except Exception as e:
        open_in_browser(url, "WebView2 窗口起不来（%s）" % e)
        wait_forever(stopped, srv)
        return

    # 窗口关掉了 ⇒ 服务也停掉（幂等：可能已经被 /api/quit 停过）
    try:
        srv.shutdown()
    except Exception:
        pass


def wait_forever(stopped, srv):
    """回退到浏览器之后：服务继续跑，直到 Ctrl+C 或界面里的「退出应用」。"""
    try:
        while not stopped.is_set():
            time.sleep(0.5)
    except KeyboardInterrupt:
        pass
    print("\n已停止。")


if __name__ == "__main__":
    main()
