#!/usr/bin/env python3
"""抓设备端日志 —— 尤其是**复位后的完整开机日志**。

为什么需要这个脚本,而不是直接 `pio device monitor`:
  · ROM、二级 bootloader、panic 的打印**只走 UART0**;
  · UART0 在板上是那个 **CH340(UART 口)** —— 它的 DTR/RTS 是**真的**接在
    IO0 / EN 上,所以可以精确地"按住复位 → 放开",从第一个字节开始抓;
  · 而 pyserial **打开串口时会默认把 DTR/RTS 都拉高**
    (serialutil.py: `_rts_state = _dtr_state = True`)。在真复位线的板子上,
    "两个都拉高"= 复位并且 IO0 也是低 = **进下载模式**;
    在 S3 的 native USB 口(USB-Serial-JTAG)上,那更是一个"要求进下载模式"的
    软复位请求。所以监视器一打开就把芯片弄进下载模式是常有的事,
    看起来就像"固件根本没跑"(2026-09-18 踩了整整一天)。

做法:在 open() **之前**就把 `_rts_state` / `_dtr_state` 写死,让 DCB 一次到位
(serialwin32.py 的 `_reconfigure_port` 是按这两个值设 `fRtsControl`/`fDtrControl`
的,打开之后再改就晚了)。

用法:
    python tools/serial-capture/capture.py COM4            # 复位后抓 12 秒
    python tools/serial-capture/capture.py COM4 30 quiet   # 只读,不复位
"""

import sys
import time

import serial


def open_port(port, baud=115200, hold_in_reset=False):
    """打开串口,**绝不**顺手拉 DTR/RTS(除非明确要求按住复位)。"""
    s = serial.Serial()
    s.port = port
    s.baudrate = baud
    # ★ 必须在 open() 之前赋值(见文件头说明)
    s._rts_state = bool(hold_in_reset)  # RTS=True → EN 低 → 芯片停在复位
    s._dtr_state = False                # DTR=False → IO0 高 → 从 flash 启动
    s.timeout = 0.2
    s.open()
    return s


def drain(s, seconds):
    t0 = time.time()
    n = 0
    while time.time() - t0 < seconds:
        d = s.read(4096)
        if d:
            n += len(d)
            sys.stdout.write(d.decode('utf-8', 'replace'))
            sys.stdout.flush()
    return n


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else 'COM4'
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 12.0
    mode = sys.argv[3] if len(sys.argv) > 3 else 'reset'

    if mode == 'quiet':
        # 只旁观:芯片在跑什么就收什么,一次复位都不做
        s = open_port(port)
        print(f'[旁观] {port} rts={s.rts} dtr={s.dtr} (都应为 False)', flush=True)
        n = drain(s, seconds)
        s.close()
        print(f'\n[旁观] 共 {n} 字节 / {seconds:g} 秒')
        return 0

    # 按顺序拉线:先按住复位,再放开 —— 这样能抓到 ROM 的第一个字节
    s = open_port(port, hold_in_reset=True)
    time.sleep(0.3)
    print('[抓取] 放开复位(EN 高),开始从第一个字节收...', flush=True)
    s.rts = False
    n = drain(s, seconds)
    s.close()
    print(f'\n[抓取] 共 {n} 字节 / {seconds:g} 秒')
    if n == 0:
        print('一个字都没有 —— 检查:口选对了没?线插在 UART 口上吗?'
              '芯片是不是被别的程序占着(监视器/另一个 esptool)?')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
