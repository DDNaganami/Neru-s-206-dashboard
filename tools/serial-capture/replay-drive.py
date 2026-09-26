#!/usr/bin/env python3
"""把**实车抓包当成录像**回放进设备:一条真实行驶录像,而不是手编一帧。

为什么需要它(2026-09-26 晚):
  · `replay.py` 只发**一帧常量**(`VAN 824 18 F8 27 10 00 00 00`)⇒ 只能证明
    "语法能过、字段会变成 van",证明不了**数值会跟着帧走**、更证明不了两块板
    在**连续变化**的数据上是否一致。
  · 车主手上的录像就是 `drive-2026-09-22-van.csv`(368.2 s 真实行驶:56 s 怠速
    →起步→市区→120.7 s 处 **39 计数 = 99.84 km/h** 最高点→256 s 后回到怠速)。
    这份文件的刻度已经被 OBD 侧独立验过(车速 ×2.56,R²=0.9984;转速 ×0.125,
    怠速中位数 898.5 rpm 对地面真值 900 rpm)⇒ 回放它能同时验**解帧、数据源切换、
    ESP-NOW 转发、两块屏**,而不需要车在手上。

这一帧的行格式(firmware 侧 `lib/dashcore/van_replay.h`):
    VAN <iden 3 位十六进制> <数据字节…>     例:`VAN 824 1C 2A 27 00 00 00 61`
本工具按**旧列宽**的 4 个字段还原:
    data[0] = data0、data[1] = data1(转速大端)、data[2] = spd_count、
    data[6] = seq。
★ 诚实标注:`data[3..5]` **当时没记**(旧 CSV 只有 7 列)⇒ 回放时填 `00`。
  这不影响本工具的目的 —— 固件解车速/转速只用 `data[0..2]`,`data[6]` 原样带上;
  但**别把这三字节当成"实测值"**。

时间:`t_s` 是**每 0.615 s 一次 flush 的时间戳**(598 个不同值 / 6561 帧,
  平均一批 11 帧),所以**它记的是落盘时刻、不是总线时刻**,同一批内 11 帧是
  连续帧但彼此间隔未知。⇒ 本工具**按固定速率回放**(默认 17.82 Hz = 6561/368.207,
  也就是**这份录像自己的平均帧率**,且与总线全貌表里这一档的 17.8 帧/秒一致),
  **不假装能还原批内微时序**。

判据(全部来自设备自己打印的行,本工具不发明第二套阈值):
  · `VAN?` 一行都不许有               —— 语法/长度解析通过;
  · 主板 `SRC … speed=van rpm=van`    —— 数据源真的切到 van;
  · 从板 `SRC … speed=link rpm=link`  —— 同一份数据经 ESP-NOW 到位;
  · 主板打印的 `v=`/`rpm` 与**录像里那一帧**一致(±1 帧,见下);
  · 同一时刻**两块板打印的数值相同**;
  · 最高点出现 **99.8 km/h**、怠速段 **≈900 rpm**;
  · **不许**出现污染值(`9.0C`/`-39.0C` 那种把包络字节当温度的读数)。

★ 为什么比较要选"最接近的一帧":`SRC` 那行**自己不带时间戳**(5 秒才打一次),
  只能按"它到达主机的时间"反推它当时用的是哪一帧;而主机到设备的 USB CDC 有延迟,
  设备的日志环在没人读的时候还会**排队**(实测 `blocked` 上百万字节)⇒ 一行可能比它
  打印的时刻晚到几百毫秒。所以本工具在 **±8 帧**的窗口里取最接近的一帧,
  并**把用到的偏移量分布打出来** —— 那个分布就是"日志延迟有多少帧"的实测,
  读的人据此判断这次的对齐有多紧,而不是靠一句"允许误差"糊过去。

用法:
    python tools/serial-capture/replay-drive.py --dry-run
    python tools/serial-capture/replay-drive.py --port COM9 --watch COM8
    python tools/serial-capture/replay-drive.py --port COM9 --hz 80 --from 56 --to 140
    python tools/serial-capture/replay-drive.py --port COM9 --from 100 --to 130 --hz 17.82

  控制台文字刻意保持 ASCII(PowerShell/Python 在 GBK 控制台上会把中文打成乱码),
  中文说明留在源码注释里。
"""

import argparse
import csv
import os
import re
import sys
import time

try:
    for _s in (sys.stdout, sys.stderr):
        _s.reconfigure(errors='replace')
except Exception:
    pass

CSV_DEFAULT = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                           'drive-2026-09-22-van.csv')
IDEN = 0x824
# 这份录像自己的平均帧率:6561 帧 / 368.207 s。默认就用它 —— "按录像的节奏放"。
HZ_RECORDED = 6561 / 368.207

RE_SRC = re.compile(r'SRC speed=(\w+) rpm=(\w+) coolant=(\w+) intake=(\w+) \| '
                    r'v=([\d.]+)km/h (\d+)rpm ([\d.]+)C ([\d.]+)C')
RE_VANERR = re.compile(r'VAN\?')
# 包络字节被当温度的实测指纹(coolant 读到 '1'=0x31-40=9.0C,intake 读到 ver=1-40=-39.0C)。
# ★ 必须带前导空格,否则会命中 29.0C / 79.0C(踩过这个坑)。
RE_POLLUTION = re.compile(r' -39\.0C| 9\.0C')
RE_LINKLOCKED = re.compile(r'link: locked')
RE_TICKAGE = re.compile(r'tick_age=(\d+)ms')


def value_identity_report(samples, frames, label, timing=None):
    """判据一:**不依赖任何时间对齐** —— 设备打印的值,在整趟录像里只可能来自哪一帧?

    返回 (可用样本数, 唯一识别样本数, 无匹配样本数)。`timing` 给的是
    (send_t, hz) 时额外做一次"打印时刻 vs 命中帧"的自洽粗查。
    """
    kept, skipped = [], 0
    for s in samples:
        if s['src'][0] in ('van', 'link') and s['src'][1] in ('van', 'link'):
            kept.append(s)
        else:
            skipped += 1
    print('  samples     : %d usable, %d skipped (source was still sim / stale)'
          % (len(kept), skipped))
    if not kept:
        print('  none of the SRC windows landed inside the replay')
        return 0, 0, 0
    unique, ambiguous, nomatch = [], 0, []
    for s in kept:
        cand = candidates_for(s['v'], s['rpm'], frames)
        s['cand'] = cand
        if not cand:
            nomatch.append(s)
        elif len(cand) <= 3:
            unique.append(s)
        else:
            ambiguous += 1
    print('  value exists in the recording : %d/%d samples'
          % (len(kept) - len(nomatch), len(kept)))
    print('  uniquely identifying samples  : %d   (that value can only come from 1..3'
          % len(unique))
    print('                                  frames of the whole %d-frame trip)' % len(frames))
    print('  ambiguous (flat idle region)  : %d   (all frames print the same => no info)'
          % ambiguous)
    if unique:
        tss = [frames[s['cand'][0]][0] for s in unique]
        print('  their frames sit at t=%0.1f..%0.1f s of the trip' % (min(tss), max(tss)))
    if nomatch:
        print('  !! %d samples printed a value that NO frame of the trip can produce:'
              % len(nomatch))
        for s in nomatch[:5]:
            print('     v=%.1f rpm=%d  (%s)' % (s['v'], s['rpm'], s['src']))
    if unique and timing:
        send_t, hz = timing
        far = 0
        for s in unique:
            base = int((s['host_t'] - send_t[0]) * hz)
            if not any(abs(c - base) <= 8 for c in s['cand']):
                far += 1
        print('  of those, printed far from where the replay was: %d' % far)
    return len(kept), len(unique), len(nomatch)


def analyze_logs(paths, frames):
    """离线复算:把已经存下来的日志当成样本源(不碰串口、不用重放一遍)。

    为什么值得有:一次 368 秒的实车回放只跑一次,但"这条判据怎么算"会反复看 ——
    能离线重算就不用为了改一段统计再放一遍录像。
    """
    print('\n================ OFFLINE RE-ANALYSIS ================')
    for path in paths:
        if not os.path.exists(path):
            print('\n---- %s ---- MISSING' % path)
            continue
        with open(path, encoding='utf-8', errors='replace') as fh:
            text = fh.read()
        samples = []
        for line in text.splitlines():
            m = RE_SRC.search(line)
            if not m:
                continue
            samples.append({
                'tag': os.path.basename(path),
                'host_t': None,
                'v': float(m.group(5)),
                'rpm': int(m.group(6)),
                'src': (m.group(1), m.group(2), m.group(3), m.group(4)),
            })
        print('\n---- %s (%d SRC lines in the log) ----' % (path, len(samples)))
        value_identity_report(samples, frames, path)
        van = len(RE_VANERR.findall(text))
        pol = len(RE_POLLUTION.findall(text))
        print('  VAN? echoes = %d   pollution hits = %d' % (van, pol))
    print('====================================================')
    return 0


def load_frames(path, t_from, t_to):
    """读实车 CSV,返回 [(t_s, data0, data1, spd_count, seq)] + 自检计数。"""
    frames = []
    bad = 0
    with open(path, newline='', encoding='utf-8-sig') as fh:
        for row in csv.DictReader(fh):
            try:
                t = float(row['t_s'])
            except (TypeError, ValueError):
                continue
            if t < t_from:
                continue
            if t_to is not None and t > t_to:
                break
            d0, d1 = int(row['data0']), int(row['data1'])
            spd, seq = int(row['spd_count']), int(row['seq'])
            rpm = int(row['rpm_count'])
            # 逐行自检:ACCEPTANCE 已验 6561/6561,这里当场再验一次,
            # 免得文件被谁改过之后我们还在"拿它当真值"。
            if d0 * 256 + d1 != rpm:
                bad += 1
            frames.append((t, d0, d1, spd, seq))
    return frames, bad


def line_for(d0, d1, spd, seq):
    """还原成 firmware 认识的那一行。data[3..5] 当时没记,填 00(见文件头)。"""
    return 'VAN %03X %02X %02X %02X 00 00 00 %02X\n' % (IDEN, d0, d1, spd, seq)


def expected_spd(spd):
    return spd * 2.56          # van_source.h 的 kSpeedScale,已由 OBD 独立验过


def expected_rpm(d0, d1):
    return (d0 * 256 + d1) * 0.125


def prints_as(spd_count, d0, d1):
    """设备那一行会打印成的样子(固件是 `v=%.1fkm/h %.0frpm`)。"""
    return (round(expected_spd(spd_count), 1), round(expected_rpm(d0, d1)))


def candidates_for(v, rpm, frames):
    """**整趟录像里**哪些帧会打印成同一个 `v=`/`rpm=`。

    ★ 这是本工具最强的一条判据,也是它不依赖任何时间对齐的地方:
      如果某个打印值在整趟 6561 帧里**只可能来自某一帧**(候选集很小),
      那么"这个数是从录像里那一帧解出来的"就不再是推测 —— 是唯一的解释。
      反之,怠速段所有帧都是 `0.0km/h 900rpm`,候选集几千个 ⇒ 那条样本
      **不携带信息**(工具会把它单独计出来,而不是拿它充数)。
    """
    out = []
    for i, fr in enumerate(frames):
        pv, pr = prints_as(fr[3], fr[1], fr[2])
        if abs(pv - v) <= 0.05 and abs(pr - rpm) <= 0.5:
            out.append(i)
    return out


class Reader:
    """一个只读的串口:非阻塞收字节,切成行。"""

    def __init__(self, port, baud):
        import serial
        self.port = port
        self.s = serial.Serial()
        self.s.port = port
        self.s.baudrate = baud
        # DTR/RTS 必须在 open() 之前设好,否则这一下就把芯片弄进下载模式。
        self.s._rts_state = False
        self.s._dtr_state = False
        self.s.timeout = 0
        self.s.open()
        self.buf = ''
        self.raw = []

    def poll(self):
        out = []
        try:
            data = self.s.read(65536)
        except Exception as exc:                      # 拔线/驱动抖动都只记一笔
            out.append(('__IOERR__', str(exc)))
            return out
        if not data:
            return out
        text = data.decode('utf-8', 'replace')
        self.raw.append(text)
        self.buf += text
        while '\n' in self.buf:
            line, self.buf = self.buf.split('\n', 1)
            out.append(('__LINE__', line.rstrip('\r')))
        return out

    def close(self):
        try:
            self.s.close()
        except Exception:
            pass

    def dump(self, path):
        if not self.raw:
            return 0
        text = ''.join(self.raw)
        with open(path, 'w', encoding='utf-8') as fh:
            fh.write(text)
        return len(text)


def classify(tag, lines, state):
    """把设备打印的行归到 state 里 —— 判据全部来自设备自身。"""
    for line in lines:
        if RE_VANERR.search(line):
            state['vanerr'] += 1
            state['vanerr_lines'].append(line)
        if RE_POLLUTION.search(line):
            state['pollution'] += 1
            state['pollution_lines'].append(line)
        if RE_LINKLOCKED.search(line):
            state['locked'] += 1
            m = RE_TICKAGE.search(line)
            if m:
                state['tick_age'].append(int(m.group(1)))
        m = RE_SRC.search(line)
        if m:
            spd, rpm, cool, intake = m.group(1), m.group(2), m.group(3), m.group(4)
            state['srclast'] = line
            state['srcmix'][(spd, rpm, cool, intake)] = \
                state['srcmix'].get((spd, rpm, cool, intake), 0) + 1
            # 带数值的那一行才参与比对(它 5 秒一次,是唯一能对数的窗口)
            state['samples'].append({
                'tag': tag,
                'host_t': time.time(),
                'v': float(m.group(5)),
                'rpm': int(m.group(6)),
                'cool': float(m.group(7)),
                'intake': float(m.group(8)),
                # ★ 必须把**数据源标签**一起留下:回放开始喂帧之前(以及 3 秒新鲜度
                #   过期时)设备用的是 sim,而 sim 会自己造出 187.7km/h 这种
                #   **录像里根本不存在**的值。把那些样本算进来会让"最大值对不上"
                #   看起来像解帧错了 —— 踩过一次,所以这里按标签过滤。
                'src': (spd, rpm, cool, intake),
            })


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--csv', default=CSV_DEFAULT)
    ap.add_argument('--port', default='COM9', help='主板(收 VAN 那一块)')
    ap.add_argument('--watch', default='', help='同时只读的第二块板(从板),可留空')
    ap.add_argument('--hz', type=float, default=HZ_RECORDED,
                    help='回放速率,默认 17.82(录像自身的平均帧率)')
    ap.add_argument('--from', dest='t_from', type=float, default=0.0)
    ap.add_argument('--to', dest='t_to', type=float, default=None)
    ap.add_argument('--baud', type=int, default=115200)
    ap.add_argument('--boot-wait', type=float, default=2.5)
    ap.add_argument('--loop', type=int, default=1)
    ap.add_argument('--out', default='', help='原始日志前缀(默认按端口写在当前目录)')
    ap.add_argument('--dry-run', action='store_true',
                    help='不发串口,只打印将要发的帧与这条路线的地标')
    ap.add_argument('--analyze', nargs='*', default=None,
                    help='离线复算:只读已存下的日志(不碰串口),重算"打印值在录像里的候选帧"')
    args = ap.parse_args()

    frames, bad = load_frames(args.csv, args.t_from, args.t_to)
    if not frames:
        print('NO FRAMES in that slice -- check --csv/--from/--to')
        return 1

    if args.analyze is not None:
        paths = args.analyze or ['full-master.log', 'full-slave.log']
        return analyze_logs(paths, frames)

    span = frames[-1][0] - frames[0][0]
    secs = len(frames) / args.hz
    peak = max(frames, key=lambda f: f[3])
    print('=== real-drive replay ===')
    print('csv        : %s' % args.csv)
    print('frames     : %d   (rpm self-check failures: %d)' % (len(frames), bad))
    print('recorded   : t=%0.3f..%0.3f s (span %0.1f s)' % (frames[0][0], frames[-1][0], span))
    print('replay     : %g Hz  ->  %0.1f s wall clock  (recorded rate was %0.2f Hz)'
          % (args.hz, secs, HZ_RECORDED))
    print('landmarks  : peak spd_count=%d (=%.2f km/h) at t=%0.1fs, rpm=%.1f'
          % (peak[3], expected_spd(peak[3]), peak[0], expected_rpm(peak[1], peak[2])))
    if args.t_from < 1.0:
        print('             idle prefix rpm=%.1f (recorded median in standstill=898.5)'
              % expected_rpm(frames[0][1], frames[0][2]))
    else:
        # 切片回放时第一帧未必是怠速 ⇒ 别把它的读数叫作"怠速地标"(那会读歪)。
        print('             slice starts mid-trip: first frame rpm=%.1f (not an idle landmark)'
              % expected_rpm(frames[0][1], frames[0][2]))
    print('first line : %s' % line_for(*frames[0][1:]).strip())
    print('last line  : %s' % line_for(*frames[-1][1:]).strip())
    print('NOTE data[3..5] were not logged in the old column width -> sent as 00')

    if args.dry_run:
        print('dry-run: nothing sent')
        return 0

    rd_master = Reader(args.port, args.baud)
    rd_watch = Reader(args.watch, args.baud) if args.watch else None
    state = {'vanerr': 0, 'vanerr_lines': [], 'pollution': 0, 'pollution_lines': [],
             'locked': 0, 'tick_age': [], 'srclast': '', 'srcmix': {}, 'samples': []}
    state_w = {k: (dict(v) if isinstance(v, dict) else list(v) if isinstance(v, list)
                   else v) for k, v in state.items()}

    try:
        # 与 replay.py 同一条纪律:开串口会让板子复位,先等它启动完再喂帧,
        # 否则开机的 Serial.begin() 会把入口 FIFO 清空、前面几百毫秒的字白丢。
        t0 = time.time()
        while time.time() - t0 < args.boot_wait:
            classify('master', [v for k, v in rd_master.poll() if k == '__LINE__'], state)
            if rd_watch:
                classify('slave', [v for k, v in rd_watch.poll() if k == '__LINE__'],
                         state_w)
            time.sleep(0.05)

        send_t = []                # send_t[i] = 第 i 帧发出的主机时刻
        n_sent = 0
        for _ in range(args.loop):
            start = time.time()
            for i, fr in enumerate(frames):
                due = start + i / args.hz
                now = time.time()
                if due > now:
                    time.sleep(due - now)
                rd_master.s.write(line_for(*fr[1:]).encode('ascii'))
                send_t.append(time.time())
                n_sent += 1
                if n_sent % 200 == 0:
                    rd_master.s.flush()
                for tag, rd, st in (('master', rd_master, state),
                                    ('slave', rd_watch, state_w)):
                    if rd is None:
                        continue
                    classify(tag, [v for k, v in rd.poll() if k == '__LINE__'], st)
    except KeyboardInterrupt:
        print('\n[stopped by hand]')
    finally:
        time.sleep(0.4)
        for tag, rd, st in (('master', rd_master, state), ('slave', rd_watch, state_w)):
            if rd is None:
                continue
            classify(tag, [v for k, v in rd.poll() if k == '__LINE__'], st)
        rd_master.close()
        if rd_watch:
            rd_watch.close()

    # ---------------- 比对 ----------------
    def compare(st, label):
        print('\n---- %s ----' % label)
        if not st['samples']:
            print('  no SRC samples captured (device silent?)')
            return
        # 只比对**设备自己说来源是回放数据**的那些样本(van=主板解帧、link=从板收到的)。
        # 其余是 sim 兜底的窗口,它们的数值是仿真器造的,不属于这次回放的判据
        # (踩过一次:sim 会造出 187.7km/h,算进来会让"最大值对不上"看起来像解帧错了)。
        kept_n, uniq_n, nomatch_n_local = value_identity_report(
            st['samples'], frames, label, timing=(send_t, args.hz))
        kept = [s for s in st['samples']
                if s['src'][0] in ('van', 'link') and s['src'][1] in ('van', 'link')]
        if not kept:
            return
        deltas, offsets = [], {}
        detail = []
        for s in kept:
            # SRC 不带时间戳 ⇒ 只能按到达时刻反推它用的是哪一帧。±8 帧的窗口不是为了
            # 放水,而是因为设备的日志环在满的时候会**排队**(USB CDC 那一侧没人读时
            # 实测 blocked 上百万字节)⇒ 一行可能比它打印的时刻晚到几百毫秒。
            # 取窗口内最接近的那一帧,并把**用到的偏移量**统计出来:那个分布本身就是
            # "日志延迟有多少帧"的实测,读的人可以直接判断这次的容忍度是否合理。
            best = None
            for off in range(-8, 9):
                idx = int((s['host_t'] - send_t[0]) * args.hz) + off
                if idx < 0 or idx >= len(send_t) or idx >= len(frames):
                    continue
                fr = frames[idx]
                dv = abs(s['v'] - expected_spd(fr[3]))
                dr = abs(s['rpm'] - expected_rpm(fr[1], fr[2]))
                if best is None or (dv + dr / 8.0) < best[0]:
                    best = (dv + dr / 8.0, dv, dr, off, fr)
            if best is None:
                continue
            _, dv, dr, off, fr = best
            deltas.append((dv, dr))
            offsets[off] = offsets.get(off, 0) + 1
            best_idx = int((s['host_t'] - send_t[0]) * args.hz) + off
            s['aligned'] = best_idx
            detail.append('  %-6s v=%6.1f (want %6.2f) rpm=%5d (want %6.1f) off=%+d'
                          % (s['tag'], s['v'], expected_spd(fr[3]), s['rpm'],
                             expected_rpm(fr[1], fr[2]), off))
        if not deltas:
            print('  samples existed but none could be aligned')
            return kept_n, uniq_n, nomatch_n_local
        dvs = sorted(d[0] for d in deltas)
        drs = sorted(d[1] for d in deltas)
        exact = sum(1 for d in deltas if d[0] <= 0.06 and d[1] <= 0.6)
        tight = sum(n for o, n in offsets.items() if -1 <= o <= 1)
        print('  |dv| median : %.3f km/h   max %.3f km/h' % (dvs[len(dvs) // 2], dvs[-1]))
        print('  |drpm| med  : %.3f rpm     max %.3f rpm' % (drs[len(drs) // 2], drs[-1]))
        print('  exact match : %d/%d  (|dv|<=0.06 and |drpm|<=0.6, i.e. the same frame)'
              % (exact, len(deltas)))
        print('  frame offset: %s' % ', '.join('%+d x%d' % (o, n)
                                               for o, n in sorted(offsets.items())))
        print('                %d/%d landed within +-1 frame  (the rest is log-path queueing,'
              % (tight, len(deltas)))
        print('                which is measured here, not assumed away)')
        print('  src mix     : %s' % ', '.join('%s/%s/%s/%s x%d' % (k + (v,))
                                               for k, v in st['srcmix'].items()))
        vs = [s['v'] for s in kept]
        rs = [s['rpm'] for s in kept]
        print('  observed    : v %.1f..%.1f km/h, rpm %d..%d  (replay-fed samples only)'
              % (min(vs), max(vs), min(rs), max(rs)))
        if detail:
            with open('%s-compare.txt' % label, 'w', encoding='utf-8') as fh:
                fh.write('\n'.join(detail) + '\n')
            print('  per-sample table -> %s-compare.txt' % label)
        return kept_n, uniq_n, nomatch_n_local

    prefix = args.out or ('replay-' + args.port.replace(':', ''))
    print('\n================ VERDICT ================')
    m_stats = compare(state, 'MASTER %s' % args.port) or (0, 0, 0)
    w_stats = (compare(state_w, 'SLAVE %s' % args.watch) or (0, 0, 0)) if rd_watch \
        else (0, 0, 0)
    nomatch_m, nomatch_w = m_stats[2], w_stats[2]

    print('\n---- checks that do not depend on timing ----')
    n1 = rd_master.dump('%s-master.log' % prefix)
    print('  log         : %s-master.log (%d bytes)' % (prefix, n1))
    if rd_watch:
        n2 = rd_watch.dump('%s-slave.log' % prefix)
        print('                %s-slave.log (%d bytes)' % (prefix, n2))
    for label, st in (('master', state), ('slave', state_w)):
        if rd_watch is None and label == 'slave':
            continue
        print('  %-6s VAN? echoes = %d   pollution hits = %d   link locked lines = %d'
              % (label, st['vanerr'], st['pollution'], st['locked']))
        if st['tick_age']:
            print('          tick_age max = %d ms' % max(st['tick_age']))
        if st['pollution_lines']:
            print('          first pollution line: %s' % st['pollution_lines'][0][:160])
        if st['vanerr_lines']:
            print('          first VAN? line      : %s' % st['vanerr_lines'][0][:160])
    # 两块板"显示的是不是同一个数":**不能**按时间配对 —— 两块板的 `SRC` 各有自己的
    # 5 秒节拍、相位互不相关(实测 74 条样本里重合 0 对)⇒ 那样只会打印一个
    # 看起来像失败的 0。真正有力的证据是上面那条:两块板**各自**都和**同一份录像**
    # 逐条对上了(且相当一部分样本在整趟 6561 帧里唯一)⇒ 转发链没有改动数值。
    msamples = [s for s in state['samples'] if s.get('cand')]
    wsamples = [s for s in state_w['samples'] if s.get('cand')]
    if rd_watch and msamples and wsamples:
        # 只有"两块板的样本恰好落在同一帧上"时才能直接对拍,这里如实报出有多少对。
        m = {s['aligned']: (s['v'], s['rpm']) for s in msamples if 'aligned' in s}
        same = diff = 0
        for s in wsamples:
            if 'aligned' not in s:
                continue
            near = [k for k in m if abs(k - s['aligned']) <= 1]
            if not near:
                continue
            k = min(near, key=lambda x: abs(x - s['aligned']))
            if m[k] == (s['v'], s['rpm']):
                same += 1
            else:
                diff += 1
                print('  MISMATCH frame#%d master=%s slave=%s'
                      % (s['aligned'], m[k], (s['v'], s['rpm'])))
        if same + diff == 0:
            print('  cross-board: no sample instants coincide (independent 5 s cadences --')
            print('               expected), so the two boards are compared to the SAME')
            print('               recording instead: %d/%d master and %d/%d slave samples'
                  % (len(msamples), len(msamples) + nomatch_m,
                     len(wsamples), len(wsamples) + nomatch_w))
            print('               carried a value found in the recording.')
        else:
            print('  master==slave on %d coincident samples, mismatches %d' % (same, diff))
    print('========================================')
    return 0


if __name__ == '__main__':
    sys.exit(main())
