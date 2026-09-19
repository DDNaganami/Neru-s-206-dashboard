#!/usr/bin/env python3
"""把逻辑分析仪抓到的**原始边沿**变成结论 —— 先只看时间,不谈协议。

为什么要有它(2026-09-18):车上的读数很矛盾 ——
  · 双绞线、两根对地 2.82V / 4.05V 且电压在浮动 → 这是一对**醒着的差分总线**;
  · 但固件那边 `frames=0`、解码器还在溢出 —— 说明**要么解码器的时间假设错了,
    要么根本不是数据**。
两者的分界只有一个数:**真实的槽时间是多少**。而这件事不该再靠我们的假设去猜,
所以拿逻辑分析仪抓一段真波形,先做**协议无关**的统计:
  · 边沿间隔直方图 → 若间隔聚在 **8µs 的整数倍**,就是 125kbit/s 的槽;
    聚在 **16µs 整数倍** → 62.5kbit/s;什么都不聚 → 是干扰不是数据。
  · 突发分组(burst)→ 帧的边界和长度一眼就看出来了。

支持三种输入(PulseView 都能导出):
  · CSV  —— "Save As → CSV"(注意:一采样一行,所以**只选几毫秒**再导,
             否则 4MHz × 5 秒 = 2000 万行,文件大到没法看)
  · VCD  —— "Save As → VCD"(紧凑得多,长捕获用这个)
  · 纯文本 —— 每行 "<时间(秒)> <电平>"

用法:
    python tools/van-decode/edge_stats.py 抓的.csv
    python tools/van-decode/edge_stats.py 抓的.vcd --rate 4000000
    python tools/van-decode/edge_stats.py --selftest
"""

import re
import sys

# 控制台兜底:GBK(936) 里没有 'µ' 和 '✓' —— 只放宽 errors,不换编码
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors='replace')
    except Exception:
        pass


def parse_csv(path):
    """认两种 CSV —— 这两种是实际会遇到的:

    · **Saleae Logic 2**(用户的软件就是它):表头 `Time [s], Channel 0`,
      数据行 `0.000000000000, 0`(默认**每个采样点一行**,数字很多但能读)。
    · **PulseView**:头几行是带引号的元信息(含 `"Sample rate: 4 MHz"`),
      数据行 `采样序号,电平`。

    返回 `(tick_us, [(tick, level), ...])` —— tick × tick_us 就是绝对时间(µs)。
    时间列的单位从表头方括号里认(s/ms/us/ns),这样两种软件都不用额外参数。
    """
    rate = None
    unit_us = None
    rows = []
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        for line in fh:
            s = line.strip()
            if not s:
                continue
            m = re.search(r'[Ss]ample\s*rate[:=]?\s*([0-9.]+)\s*([kKmM]?)\s*Hz?', s)
            if m and rate is None:
                rate = float(m.group(1)) * {'': 1, 'k': 1e3, 'm': 1e6}[m.group(2).lower()]
            if unit_us is None and 'time' in s.lower():
                h = re.search(r'\[\s*(fs|ps|ns|us|\u00b5s|ms|s)\s*\]', s, re.I)
                if h:
                    unit_us = {'fs': 1e-9, 'ps': 1e-6, 'ns': 1e-3, 'us': 1.0,
                               '\u00b5s': 1.0, 'ms': 1e3, 's': 1e6}[h.group(1).lower()]
                    continue                       # 表头本身不是数据
            if not (s[0].isdigit() or s[0] in '-+.'):
                continue                           # 通道名/元信息
            parts = [p for p in re.split(r'[,\t]', s.strip('"')) if p.strip() != '']
            if len(parts) < 2:
                continue
            try:
                t = float(parts[0])
                level = int(float(parts[-1]))
            except ValueError:
                continue
            rows.append((int(t * unit_us) if unit_us else int(t), level))

    if unit_us is not None:                        # 时间列:已经换算成 µs 了
        return 1.0, rows
    if rate is None:
        return None, rows                          # 调用方用 --rate 补
    return 1e6 / rate, rows


def parse_vcd(path):
    """极简 VCD 解析:只认 $timescale 和第一个信号的变化。"""
    unit_scale = {'s': 1e0, 'ms': 1e-3, 'us': 1e-6, 'ns': 1e-9, 'ps': 1e-12}
    rate = None                      # VCD 里没有采样率,用时间轴(下面按 ns 处理)
    times, changes = [], []
    t = 0
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        for line in fh:
            line = line.strip()
            if line.startswith('$timescale'):
                m = re.search(r'(\d+)\s*(fs|ps|ns|us|ms|s)', line)
                if m:
                    rate = int(m.group(1)) * unit_scale[m.group(2)]   # 一个 tick = 多少秒
                continue
            if line.startswith('#'):
                try:
                    t = int(line[1:])
                except ValueError:
                    pass
                continue
            m = re.match(r'^([01xz])(\S+)$', line)
            if m:
                changes.append((t, 1 if m.group(1) == '1' else 0))
    return rate, changes


def parse_simple(path):
    """每行 '<时间(秒)> <电平>' 或 '<时间(微秒)> <电平>'。"""
    out = []
    with open(path, 'r', encoding='utf-8', errors='replace') as fh:
        for line in fh:
            parts = line.split()
            if len(parts) < 2 or not parts[0][0].isdigit():
                continue
            try:
                out.append((int(float(parts[0])), int(float(parts[1]))))
            except ValueError:
                continue
    return out


def edges_from_levels(seq, tick_us):
    """[(序号,电平)] → [(时间µs, 电平)] 只保留真正变化的点。"""
    out = []
    last = None
    for ts, lv in seq:
        if last is None or lv != last:
            out.append((ts * tick_us, lv))
            last = lv
    return out


def histogram(values, bin_us=1.0, limit=24):
    buckets = {}
    for v in values:
        b = int(v / bin_us)
        buckets[b] = buckets.get(b, 0) + 1
    keys = sorted(buckets)
    top = sorted(buckets.items(), key=lambda kv: -kv[1])[:limit]
    top.sort()
    return top


def report(edges, tick_us):
    if len(edges) < 3:
        print(f'边沿太少({len(edges)} 个)—— 这段捕获里没有信号,或者夹错通道了')
        return
    diffs = [edges[i + 1][0] - edges[i][0] for i in range(len(edges) - 1)]
    ds = sorted(diffs)
    n = len(ds)
    span_us = edges[-1][0] - edges[0][0]
    print(f'边沿 {n} 个 · 时长 {span_us:.3f}µs ({span_us/1000:.3f}ms) · '
          f'平均 {n / max(1e-9, span_us / 1e6):.0f} 边沿/秒')
    print(f'间隔: 最小 {ds[0]:.2f}µs · 中位 {ds[n//2]:.2f}µs · '
          f'90% {ds[int(n*0.9)]:.2f}µs · 最大 {ds[-1]:.2f}µs')

    print('\n间隔直方图(0.5µs 分箱,只列最多的那些):')
    for b, c in histogram(diffs, 0.5, 14):
        print(f'  {b*0.5:7.2f}~{b*0.5+0.5:6.2f}µs  {c:6d}  ' + '#' * min(60, c))

    # ★ 关键判据:最短的一撮间隔是不是聚在某个基数的整数倍上
    #   注意 Manchester 类编码里**每个位有两个半槽**,所以最短的一撮可能是
    #   半槽(4µs)而不是整槽(8µs)—— 两种都要报出来。
    short = sorted(d for d in diffs if d < max(ds[0] * 2.5, 4.0))
    if short:
        base = short[len(short) // 2]
        print(f'\n★ 最短一撮的中位数 = {base:.2f}µs:')
        for cand, name in ((4.0, '半槽 → 整槽 8µs → **125 kbit/s**(VAN comfort,'
                                 '我们固件的假设)'),
                           (8.0, '整槽 → **125 kbit/s**(VAN comfort,我们固件的假设)'),
                           (16.0, '整槽 → **62.5 kbit/s**(VAN body)')):
            if abs(base - cand) < cand * 0.25:
                print(f'   ≈ {cand}µs = {name} ✓')
        if not any(abs(base - c) < c * 0.25 for c in (4.0, 8.0, 16.0)):
            print('   与 4/8/16µs 都不接近 → 这**不是 VAN**(或不是数字总线信号)')

    # 突发分组:间隔 > 20µs 当作"帧间空闲"
    gap = 20.0
    bursts, cur = [], 1
    for d in diffs:
        if d > gap:
            bursts.append(cur)
            cur = 1
        else:
            cur += 1
    bursts.append(cur)
    big = sorted(b for b in bursts if b >= 8)
    print(f'\n突发分组(间隔 >{gap:.0f}µs 算空闲): 共 {len(bursts)} 段,'
          f'其中 {len(big)} 段 ≥8 个边沿')
    if big:
        print(f'  每段边沿数: 中位 {big[len(big)//2]} · 最少 {big[0]} · 最多 {big[-1]}')


def action_synth(slot_us=8.0, nbits=120, gap_us=400.0, nframes=6, rate_hz=4_000_000):
    """造一段"Manchester 风格的帧 + 长空闲"当自检数据。"""
    rows = []
    t = 0
    lv = 1
    seq = []
    for _ in range(nframes):
        for _ in range(nbits):
            half = slot_us / 2.0
            seq.append((t, lv)); t += half
            lv ^= 1
            seq.append((t, lv)); t += half
            lv ^= 1
        seq.append((t, lv)); t += gap_us       # 空闲(不变)
    for ts, v in seq:
        s = int(ts * rate_hz / 1e6)
        rows.append((s, v))
    return rows


def selftest():
    import os
    import tempfile
    d = tempfile.mkdtemp(prefix='edgestats')
    rate = 4_000_000
    rows = action_synth()                      # 8µs 位 → 每 4µs 一次跳变
    checks = []

    # 1) PulseView 风格:元信息写采样率,数据行是"采样序号,电平"
    csv1 = os.path.join(d, 'pv.csv')
    with open(csv1, 'w', encoding='utf-8') as fh:
        fh.write('"Sample rate: 4 MHz"\n"Channel 1"\n')
        for s, v in rows:
            fh.write(f'{s},{v}\n')
    tick_us, got1 = parse_csv(csv1)
    checks.append(('PulseView CSV(采样序号+采样率)',
                   abs(tick_us - 0.25) < 1e-9 and len(got1) == len(rows)))

    # 2) Saleae Logic 2 风格:表头 "Time [s], Channel 0",时间列是秒
    csv2 = os.path.join(d, 'l2.csv')
    with open(csv2, 'w', encoding='utf-8') as fh:
        fh.write('Time [s], Channel 0\n')
        for s, v in rows:
            fh.write(f'{s / rate:.12f}, {v}\n')
    tick_us2, got2 = parse_csv(csv2)
    e2 = edges_from_levels(got2, tick_us2)
    d2 = sorted(e2[i + 1][0] - e2[i][0] for i in range(len(e2) - 1))
    checks.append(('Logic 2 CSV(Time [s])',
                   tick_us2 == 1.0 and abs(d2[len(d2) // 4] - 4.0) < 0.3))

    for name, good in checks:
        print(f'[{"OK " if good else "FAIL"}] {name}')
    print('\n--- 报告长这样(合成数据:8µs 位 → 每 4µs 一次跳变)---')
    report(edges_from_levels(got1, tick_us), tick_us)
    try:
        os.remove(csv1)
        os.remove(csv2)
        os.rmdir(d)
    except OSError:
        pass
    return 0 if all(g for _, g in checks) else 1


def main(argv):
    if len(argv) > 1 and argv[1] == '--selftest':
        return selftest()
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    rate = None
    for i, a in enumerate(argv):
        if a == '--rate' and i + 1 < len(argv):
            rate = float(argv[i + 1])

    if path.lower().endswith('.vcd'):
        tick_s, rows = parse_vcd(path)
        tick_us = (tick_s or 1e-9) * 1e6
    elif path.lower().endswith('.csv'):
        tick_us, rows = parse_csv(path)
        if tick_us is None:
            if not rate:
                print('CSV 里既没有时间列表头、也没有采样率 —— 请加 --rate 4000000')
                return 2
            tick_us = 1e6 / rate
    else:
        rows = parse_simple(path)
        tick_us = 1.0                                # 纯文本按"微秒"当单位
        if rate:
            tick_us = 1e6 / rate

    print(f'{path}: {len(rows)} 行,时间单位 {tick_us:.4f}µs/点')
    report(edges_from_levels(rows, tick_us), tick_us)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
