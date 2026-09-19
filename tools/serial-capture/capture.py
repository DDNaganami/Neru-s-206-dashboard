#!/usr/bin/env python3
"""抓设备端日志 —— 尤其是**复位后的完整开机日志**,以及**车上抓 VAN 帧**。

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
    python tools/serial-capture/capture.py COM4 600 quiet --out van_capture_1.txt
    python tools/serial-capture/capture.py --selftest      # 只验统计逻辑,不碰串口

车上抓帧专用的三个开关(2026-09-18 加的,前两个是踩过的坑):
  --out FILE        由脚本自己写 UTF-8 文件。
                    ★ 为什么不用 shell 重定向:这台机器上是 Windows PowerShell
                    5.1,`>` 写出来的文件是 **UTF-16LE**(而且带 BOM),回头一分析
                    全是乱码;重定向还让控制台看不到实时统计。
  --until-frames N  抓到 N 个好帧就提前收工 —— 车上不用干等满 600 秒。
  --baud N          串口波特率,默认 115200(与固件 `dash_log_begin()` 一致)。
                    ★ 留这个开关的原因:115200 只有 ≈11.5KB/s。VAN 帧一行约 48 字符,
                    所以总线超过 ~230 帧/秒时串口就会成为瓶颈 —— 而**阻塞在串口写**
                    会拖住主循环、丢边沿,表现是 `dropped` 在涨 + 坏帧变多。
                    真遇到就把固件的 `dash_log_begin(115200)` 改成 921600 重刷一遍,
                    再用 `--baud 921600` 抓(见 PINOUT.md「E. 已知的坑」)。
  --selftest        用几行样板日志验证"好帧/坏帧/IDEN/edges"的统计对不对。

实时统计一律打到 **stderr**:这样即使用 `> 文件` 也只看得到日志正文进文件,
该看进度的时候屏幕上还是有人话。"""

import re
import sys
import time

import serial

# 控制台兜底(2026-09-18 实测踩到):这台机器的控制台默认代码页是 GBK(936),
# 而 '✓'(U+2713)**不在 GBK 里** —— 直接 print 会 `UnicodeEncodeError` 把整个脚本
# 打断,而且是在**打小结的时候**才炸(等于跑完一趟才发现日志没写成)。
# 只放宽 errors、不换编码:换成 UTF-8 会让 GBK 控制台里的中文全变乱码。
# (真正的日志文件是显式 `encoding='utf-8'` 写的,与这里无关。)
for _stream in (sys.stdout, sys.stderr):
    try:
        _stream.reconfigure(errors='replace')
    except Exception:
        pass

# ---- 日志行解析(纯函数式:feed() 只吃"一整行",便于 --selftest 直接验) ----
#
# 好帧(设备端 VanLogSink 打的,格式**故意**与 van_replay 的行格式一致,
# 所以抓到的日志可以直接粘回串口回放):
#     VAN 824 18 F8 27 10 00 00 00   # cmd=1 ack=0
# 坏帧(CRC 没过,单独用 '# ' 开头:不能回放,但"到底有没有收到东西"要看它):
#     # VAN 校验失败 iden=824 len=3
# 每秒诊断(只在计数有变化时打,免得刷屏):
#     van: edges=305 frames=0 fcs_ok=0 dropped=0(队列0) 待收=0
_RE_VAN = re.compile(r'^VAN\s+([0-9A-Fa-f]{3,4})(?:\s+(.*))?$')
_RE_VANBAD = re.compile(r'^#\s*VAN\s+校验失败\s+iden=([0-9A-Fa-f]+)')
_RE_DIAG = re.compile(r'^van:\s*(.+)$')
_RE_SRC = re.compile(r'^SRC\s+(.+)$')
_RE_EDGES = re.compile(r'edges=(\d+)')


class Stats:
    """一趟抓取下来"看到了什么" —— 只留分析时真正要用的那些数字。"""

    def __init__(self):
        self.bytes = 0
        self.lines = 0
        self.good = 0                      # CRC 通过、可以回放的帧
        self.bad = 0                       # 收到但 CRC 没过的帧
        self.good_by_iden = {}
        self.bad_by_iden = {}
        self.example = {}                  # iden → 第一行样例(排错时最有用)
        self.diag = ''                     # 最近一行每秒诊断
        self.src = ''                      # 最近一行数据源
        self.edges = None                  # 最近一次 edges 计数:"线接对没有"的第一手数字

    def feed(self, line):
        line = line.rstrip('\r\n')
        if not line:
            return
        self.lines += 1

        m = _RE_VAN.match(line)
        if m:
            iden = int(m.group(1), 16)
            self.good += 1
            self.good_by_iden[iden] = self.good_by_iden.get(iden, 0) + 1
            # 只留第一条:同一 IDEN 的样例几乎都一样,留多了只会淹掉别的 IDEN
            if iden not in self.example:
                self.example[iden] = line[:72]
            return

        m = _RE_VANBAD.match(line)
        if m:
            iden = int(m.group(1), 16)
            self.bad += 1
            self.bad_by_iden[iden] = self.bad_by_iden.get(iden, 0) + 1
            return

        m = _RE_DIAG.match(line)
        if m:
            self.diag = line
            e = _RE_EDGES.search(line)
            if e:
                self.edges = int(e.group(1))
            return

        if _RE_SRC.match(line):
            self.src = line


def summarize(st, seconds):
    """把 Stats 变成人看的几行 —— 车上回来先看这几行就知道这趟有没有白跑。"""
    out = []
    out.append(f'==== 抓帧小结({seconds:g} 秒)====')
    out.append(f'字节 {st.bytes} · 行 {st.lines} · 好帧 {st.good} · 坏帧 {st.bad}')
    if st.good_by_iden or st.bad_by_iden:
        out.append(f'{"IDEN":<6}{"好帧":>6}{"坏帧":>6}   样例')
        for iden in sorted(set(st.good_by_iden) | set(st.bad_by_iden)):
            g = st.good_by_iden.get(iden, 0)
            b = st.bad_by_iden.get(iden, 0)
            out.append(f'{iden:03X}   {g:>6}{b:>6}   {st.example.get(iden, "")}')
    else:
        out.append('一条 VAN 帧都没有。桌面(没接总线)这样是正常的;'
                   '车上出现就是 PINOUT.md「C. 上车步骤」那张表的哪一种,照着办。')
    if st.diag:
        out.append(f'最近诊断: {st.diag}')
    if st.src:
        out.append(f'最近数据源: {st.src}')
    return out


def live_line(st, elapsed, width=118):
    idens = ' '.join(f'{k:03X}×{v}' for k, v in
                     sorted(st.good_by_iden.items(), key=lambda kv: -kv[1])[:4])
    s = (f'[{elapsed:7.1f}s] 字节={st.bytes} 行={st.lines} 好帧={st.good} 坏帧={st.bad}')
    if idens:
        s += '  IDEN ' + idens
    if st.diag:
        s += '  | ' + st.diag
    return s[:width]


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


def drain(s, seconds, out=None, until_frames=0, live=True):
    st = Stats()
    t0 = time.time()
    tail = ''                      # 跨 read 的半行:一行可能被切成两次读
    fh = open(out, 'w', encoding='utf-8') if out else None
    last_live = 0.0
    try:
        while True:
            elapsed = time.time() - t0
            if elapsed >= seconds:
                break
            if until_frames and st.good >= until_frames:
                sys.stderr.write('\n')
                print(f'[抓帧] 已抓到 {st.good} 个好帧,提前收工', file=sys.stderr)
                break
            d = s.read(4096)
            if d:
                st.bytes += len(d)
                text = d.decode('utf-8', 'replace')
                if fh:
                    fh.write(text)
                else:
                    sys.stdout.write(text)
                    sys.stdout.flush()
                tail += text
                parts = tail.split('\n')
                tail = parts.pop()          # 最后一段可能还没结束,留到下一轮
                for ln in parts:
                    st.feed(ln)
                if len(tail) > 4096:        # 噪声导致的超长行:别让它无限长大
                    st.feed(tail)
                    tail = ''
            # 实时进度最多 2Hz:太快只是闪屏,人眼也读不过来
            if live and time.time() - last_live >= 0.5:
                last_live = time.time()
                sys.stderr.write('\r' + live_line(st, time.time() - t0))
                sys.stderr.flush()
    finally:
        if fh:
            fh.close()
    if tail:
        st.feed(tail)
    if live:
        sys.stderr.write('\r' + ' ' * 118 + '\r')
        sys.stderr.flush()
    return st


# ---- --selftest:不碰串口,只验"这趟抓到了什么"的统计对不对 ----
# 为什么值得写:车上抓完回来,结论**全靠这个统计**;它要是把坏帧算成好帧、
# 或者把 IDEN 解析错,人会照着错数字去改协议代码。
_SAMPLE = [
    'I (123) BOOT',
    'van phy: gpio 就绪 RX=GPIO16(RO),空闲 300us 关帧',
    'van: edges=15 frames=0 fcs_ok=0 dropped=0(队列0) 待收=0',
    'VAN 824 18 F8 27 10 00 00 00   # cmd=1 ack=0',
    'VAN 824 18 F8 28 10 00 00 00   # cmd=1 ack=0',
    'VAN 8A1 01 02 03   # cmd=0 ack=1',
    '# VAN 校验失败 iden=824 len=3',
    'van: edges=99 frames=2 fcs_ok=2 dropped=1(队列0) 待收=0',
    'SRC speed=van rpm=sim coolant=sim intake=sim | v=57.3km/h 2100rpm 88.0C 21.0C',
    '',                                   # 空行不算行
]


def selftest_run():
    st = Stats()
    for ln in _SAMPLE:
        st.feed(ln)
    checks = [
        ('行数(空行不计)', st.lines, 9),
        ('好帧数', st.good, 3),
        ('坏帧数', st.bad, 1),
        ('IDEN 824 好帧', st.good_by_iden.get(0x824), 2),
        ('IDEN 824 坏帧', st.bad_by_iden.get(0x824), 1),
        ('IDEN 8A1 好帧', st.good_by_iden.get(0x8A1), 1),
        ('edges 取最后一次', st.edges, 99),
        ('数据源识别', st.src.startswith('SRC speed=van'), True),
        ('样例保留第一行', st.example.get(0x824, '').startswith('VAN 824 18 F8 27'), True),
    ]
    bad = 0
    for name, got, want in checks:
        ok = got == want
        bad += 0 if ok else 1
        print(f'{"✓" if ok else "✗"} {name}: {got!r}' + ('' if ok else f' ≠ 期望 {want!r}'))
    print('\n--- 小结长这样 ---')
    for ln in summarize(st, 600):
        print(ln)
    if bad:
        print(f'\n{bad} 项不符')
        return 1
    print('\nselftest 全部通过')
    return 0


def main(argv):
    out = None
    until = 0
    baud = 115200
    selftest = False
    rest = []
    i = 1
    while i < len(argv):
        a = argv[i]
        if a == '--out':
            i += 1
            out = argv[i]
        elif a.startswith('--out='):
            out = a.split('=', 1)[1]
        elif a == '--until-frames':
            i += 1
            until = int(argv[i])
        elif a.startswith('--until-frames='):
            until = int(a.split('=', 1)[1])
        elif a == '--baud':
            i += 1
            baud = int(argv[i])
        elif a.startswith('--baud='):
            baud = int(a.split('=', 1)[1])
        elif a == '--selftest':
            selftest = True
        else:
            rest.append(a)
        i += 1

    if selftest:
        return selftest_run()

    port = rest[0] if rest else 'COM4'
    seconds = float(rest[1]) if len(rest) > 1 else 12.0
    mode = rest[2] if len(rest) > 2 else 'reset'

    if mode == 'quiet':
        # 只旁观:芯片在跑什么就收什么,一次复位都不做
        s = open_port(port, baud=baud)
        print(f'[旁观] {port}@{baud} rts={s.rts} dtr={s.dtr} (都应为 False)', flush=True)
        st = drain(s, seconds, out=out, until_frames=until)
        s.close()
        print(f'[旁观] 共 {st.bytes} 字节 / {seconds:g} 秒')
        for ln in summarize(st, seconds):
            print(ln)
        if out:
            print(f'[旁观] 原始日志已写入 {out}')
        return 0

    # 按顺序拉线:先按住复位,再放开 —— 这样能抓到 ROM 的第一个字节
    s = open_port(port, baud=baud, hold_in_reset=True)
    time.sleep(0.3)
    print('[抓取] 放开复位(EN 高),开始从第一个字节收...', flush=True)
    s.rts = False
    st = drain(s, seconds, out=out, until_frames=until)
    s.close()
    print(f'[抓取] 共 {st.bytes} 字节 / {seconds:g} 秒')
    for ln in summarize(st, seconds):
        print(ln)
    if out:
        print(f'[抓取] 原始日志已写入 {out}')
    if st.bytes == 0:
        print('一个字都没有 —— 检查:口选对了没?线插在 UART 口上吗?'
              '芯片是不是被别的程序占着(监视器/另一个 esptool)?')
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
