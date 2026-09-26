#!/usr/bin/env python3
"""往设备串口贴一行 VAN 回放帧,然后看设备的反应(离线联调用)。

用途:收发器还没接、或车不在手边时,先把"解帧 → 数据源 → 表盘"这条链跑一遍。
设备端收到形如 `VAN 824 18 F8 27 10 00 00 00` 的行,会当成一帧真实 VAN 帧喂进去
(见 lib/dashcore/van_replay.h);解析失败会回显 `VAN? <原文>`。

怎么判断成功:
  · 没有 `VAN?` 回显 —— 说明语法解析通过;
  · 之后每 5 秒那行 `SRC speed=... rpm=... coolant=... intake=...` 里
    对应的字段从 `sim` 变成 `van`。
  ★ 现行刻度(2026-09-26 更正,旧文写的是 `data[2..3]` 大端 ×0.01 —— **已作废**):
    车速 = `data[2]` 单字节 × **2.56**(`van_source.h` 的 `kSpeedScale`,
    真值源是 ELM327 的 `010D`,回归 R² = 0.9984);
    转速 = `data[0..1]` **大端** × **0.125**。
    所以默认那帧 `18 F8 27 10 00 00 00` = 转速 `0x18F8`(6392) × 0.125 = **799 rpm**、
    车速 `0x27`(39) × 2.56 = **99.84 → 打印 99.8 km/h**
    (与旧的 ×0.01 口径**巧合同量级**,别因此以为旧公式还对)。
  看到 `v=99.8km/h 799rpm` 且 `speed=van`,就说明整条链通了。
  ★ 想验"数值跟着帧走"而不是只看一帧常量,用同目录的 **`replay-drive.py`**
    (把整趟实车录像按 17.82 Hz 放完,并逐条对数)。

★ 为什么要先等两秒再发:
  打开串口这一下会让板子复位(板载 CH340 的 DTR/RTS 接在 EN/IO0 上),
  而 Arduino 的 `Serial0.begin()` 会**清空 RX FIFO** ——
  开机那几百毫秒里发进去的字会被直接丢掉。所以:开串口 → 等设备启动完 → 再发。

★ 为什么是**反复发**而不是发一帧:
  设备端的源有 3 秒新鲜度窗口(data_service.cpp 的 kStaleMs = 3000),
  过期就退回 sim;而 `SRC` 那行 5 秒才打一次 —— 只发一帧的话,
  很可能是"帧早过期了才轮到打日志",看起来像没生效。真车上本来就是连续帧,
  所以这里默认每 0.5 秒重发一次,持续整个观察窗口。

用法:
    python tools/serial-capture/replay.py COM4 "VAN 824 18 F8 27 10 00 00 00"
    python tools/serial-capture/replay.py COM4            # 不带参数就发默认示例帧
"""

import sys
import time

import serial

DEFAULT_LINE = 'VAN 824 18 F8 27 10 00 00 00'
# 开串口到发帧之间的等待:够 ROM + bootloader + setup 跑完(实测约 0.3 秒起步,
# 但留足余量,免得又被 begin() 清掉一次)。
BOOT_WAIT_S = 2.5
# 重发间隔:必须明显小于 kStaleMs(3000),否则数据会一会儿新鲜一会儿过期。
RESEND_S = 0.5


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else 'COM4'
    line = sys.argv[2] if len(sys.argv) > 2 else DEFAULT_LINE
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 12.0

    s = serial.Serial()
    s.port = port
    s.baudrate = 115200
    # ★ 与 capture.py 同一条纪律:DTR/RTS 必须在 open() **之前**设好,
    #   否则这一下就把芯片弄进下载模式(见 capture.py 与 ACCEPTANCE.md)。
    s._rts_state = False
    s._dtr_state = False
    s.timeout = 0.05
    s.open()

    # 先收一会儿:让设备的开机日志打出来(顺便证明它在跑),再贴帧。
    print(f'[等待 {BOOT_WAIT_S:g} 秒设备启动]', flush=True)
    t0 = time.time()
    while time.time() - t0 < BOOT_WAIT_S:
        d = s.read(4096)
        if d:
            sys.stdout.write(d.decode('utf-8', 'replace'))
            sys.stdout.flush()

    payload = (line.strip() + '\n').encode('ascii')
    print(f'\n[开始每 {RESEND_S:g} 秒发一次] {line.strip()}', flush=True)

    n, sent = 0, 0
    t0 = time.time()
    next_send = t0
    while time.time() - t0 < seconds:
        now = time.time()
        if now >= next_send:
            s.write(payload)
            s.flush()
            next_send = now + RESEND_S
            sent += 1
        d = s.read(4096)
        if d:
            n += len(d)
            sys.stdout.write(d.decode('utf-8', 'replace'))
            sys.stdout.flush()
    s.close()
    print(f'\n[共发出 {sent} 帧,收到 {n} 字节 / {seconds:g} 秒]')
    print('判据:上面的 SRC 行里 speed 应该从 sim 变成 van,'
          '数值应稳定在 99.8km/h / 799rpm(默认帧按现行刻度算出来的就是这两个数)')
    if n == 0:
        print('贴帧之后一个字都没回来 —— 检查:口对不对?线插在 UART 口上吗?'
              '(设备每 5 秒至少有一行 SRC,所以正常情况下不可能这么安静)')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
