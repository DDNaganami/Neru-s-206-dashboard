#!/usr/bin/env python3
"""量「帧内最大跳变间隔」与「帧间空闲分布」—— 给 van_phy_gpio.cpp 的 kIdleCloseUs 定值。

为什么要有它(2026-09-19,5 分钟行驶抓包 drive5min.csv,17106 帧):
  · 固件的关帧判据是"总线空闲超过 kIdleCloseUs 就 finish()"。这个值原先取
    **300µs**,是**帧结构还没定案**时拍的保守值(那时只知道"判晚了不丢数据")。
  · 帧结构定案之后两头都看得见了,门限被夹在一个区间里:
      · **下界** = 帧内最长的一段同电平(这段里没有跳变,门限比它小就会在帧中间
        finish(),帧被截断 → FCS 必不过);
      · **上界** = 最短的帧间空闲(门限比它大就会把背靠背的两帧并进同一个缓冲,
        丢一帧)。
  · 这两个数只能**量**,不能猜。本脚本先给**不预设门限**的间隔直方图(两撮之间
    那个空档就是可用区间),再按门限切段、**逐段重算 FCS** —— 只有 FCS 通过的
    段才真的是"一帧",统计它才有意义。

量法(三步,顺序不能反):
  ① 全部跳变间隔的直方图(按实测槽时间 8.25µs 折成"槽"),找空档;
  ② 按 --split(默认 70µs = 固件新门限)切段,逐段跑 fit_fcs 的
     decode_frame()/verify_fcs():段数、FCS 通过、**并帧(一段 ≥2 个 SOF)** 段数、
     以及"固件真能回收几帧"(见 recover_frames 的说明);
  ③ 帧间空闲(切段用的那些间隔)的分位数 + <300µs / <50µs 的计数
     —— 前者就是**旧门限 300µs 正在丢的帧边界数**。

用法:
    python tools/van-decode/gap_stats.py 抓包.csv
    python tools/van-decode/gap_stats.py 抓包.csv --split 70 --split 300
    python tools/van-decode/gap_stats.py 抓包.csv --slots 40     # 直方图列出 1..40 槽

★ 时间列是**秒**(Logic 2 导出,0.25µs 量化,表头可能带 BOM):一律走
  edge_stats.parse_csv / edges_from_levels,别自己 float(line[0])(踩过:BOM
  吃掉首条边沿、整段错位)。
"""

import os
import sys
from collections import Counter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import edge_stats as es
import fit_fcs as ff

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors='replace')
    except Exception:
        pass

SPLIT_US = 70.0        # 固件新门限(与 van_phy_gpio.cpp 的 kIdleCloseUs 对齐)
LEGACY_US = 300.0      # 旧门限:留着是为了**量出它丢了多少**,不是候选值
SLOT_US = 8.25         # 实测槽时间(van_wire.h 的 kTsNs 同源)


def gaps_of(edges):
    """相邻跳变的间隔(µs)。edges 里的时间已经是 µs。"""
    return [edges[i + 1][0] - edges[i][0] for i in range(len(edges) - 1)]


def segment(edges, idle_us):
    """按"间隔 > idle_us"切段 → (段列表, 切段处的间隔列表)。

    与 fit_fcs.split_frames / decode_frames.load_frames 同一套逻辑,
    只是把"切段用的那些间隔"也返回出来 —— 它们正是**帧间空闲**的实测值。
    """
    if not edges:
        return [], []
    segs, inter, cur = [], [], [edges[0]]
    last = edges[0][0]
    for t, lv in edges[1:]:
        d = t - last
        if d > idle_us:
            if len(cur) >= 6:
                segs.append(cur)
            inter.append(d)
            cur = []
        cur.append((t, lv))
        last = t
    if len(cur) >= 6:
        segs.append(cur)
    return segs, inter


def frame_starts(s):
    """段内**真正的帧起点**位置:段首,或前面有 ≥8 个连续 recessive 的 SOF。

    为什么不能直接 s.count(SOF):帧体里偶然也会出现 SOF 的 10 槽图案,
    但那时解码器还在 InFrame(它只在非 InFrame 时才跑 SOF 匹配),不会重新起帧。
    真帧界前面必定有一段 recessive(8 槽 EOF + 帧间空闲),所以用"前面 ≥8 个
    recessive"当帧起点的判据 —— 它与门限无关,两帧并在一段里才会数出 2 个。
    """
    out, i = [], s.find(ff.SOF)
    while i >= 0:
        if i == 0 or s[max(0, i - 8):i] == '1' * 8:
            out.append(i)
        i = s.find(ff.SOF, i + 1)
    return out


def seg_report(segs, label):
    """切段质量:段数 / 干净收出的帧 / **并帧段数** / 物理帧数。

    · **单帧段** = 整段的槽串按定案布局解出来、FCS 通过 ⇒ 这一段确实只有一帧
      (并了帧的段必然解不过:每帧各有一个 EOD 违约,而正常帧全帧只有一个)。
    · **并帧段** = 段里数出 ≥2 个帧起点 ⇒ 门限太大,把两帧并进了同一个缓冲。
    """
    n_single = n_merge = n_frames = 0
    for f in segs:
        s, _ = ff.frame_slots(f)
        if not s:
            continue
        st = frame_starts(s)
        n_frames += len(st)
        n_merge += max(0, len(st) - 1)
        d = ff.decode_frame(s)
        if d is not None and ff.verify_fcs(d):
            n_single += 1
    print('  [%s] 段 %d · 单帧段(FCS 通过) %d · **并帧段 %d** · 段内帧起点合计 %d'
          % (label, len(segs), n_single, n_merge, n_frames))
    return n_single, n_merge, n_frames


def pct(ds, p):
    return ds[min(len(ds) - 1, int(len(ds) * p))]


def empty_band(g, max_slots=40, min_width=3):
    """找**第一个够宽的空档**(≥min_width 个连续槽没有样本)→ (空档左边界, 右边界)。

    为什么取"第一个够宽的"而不是"最宽的":帧间空闲那一侧本身是稀疏的
    (几十槽、几百槽都可能没有样本),取最宽的会挑到稀疏区里,毫无意义。
    而**帧内间隔 → 帧间空闲**这个台阶只出现一次:从槽 1 往上扫,
    第一个连续空档就是它。
    返回 None 表示没找到(数据不够干净,门限没法定)。
    """
    present = set(int(round(x / SLOT_US)) for x in g if 0 < x <= max_slots * SLOT_US)
    k = 1
    while k <= max_slots:
        if k in present:
            k += 1
            continue
        j = k
        while j <= max_slots and j not in present:
            j += 1
        if j - k >= min_width and j - 1 < max_slots:
            lo = [s for s in present if s < k]
            hi = [s for s in present if s > j - 1]
            return (max(lo) if lo else 1), (min(hi) if hi else max_slots)
        k = j
    return None


def slot_hist(gaps, nmax):
    c = Counter(int(round(x / SLOT_US)) for x in gaps)
    print('  槽数(×%.2fµs)   间隔区间(µs)          个数' % SLOT_US)
    for k in range(1, nmax + 1):
        n = c.get(k, 0)
        print('   %3d          %7.2f ~ %-8.2f %9d  %s'
              % (k, (k - .5) * SLOT_US, (k + .5) * SLOT_US, n,
                 '★ 空档' if n == 0 else '#' * min(50, n // max(1, max(c.values()) // 50))))
    print('   >%d         %7.2f ~              %9d'
          % (nmax, (nmax + .5) * SLOT_US, sum(v for k, v in c.items() if k > nmax)))


def report(path, splits, slot_max):
    tick_us, rows = es.parse_csv(path)
    edges = es.edges_from_levels(rows, tick_us)
    neg = sum(1 for t, _ in edges if t < 0)
    edges = [(t, lv) for t, lv in edges if t >= 0]
    if len(edges) < 10:
        print('边沿太少 —— 先确认 CSV 里 CH0 在跳变(edge_stats.py 能出直方图吗?)')
        return 1
    g = gaps_of(edges)
    n = len(g)
    span = (edges[-1][0] - edges[0][0]) / 1e6

    print('=' * 78)
    print('%s' % path)
    print('边沿 %d 条(负时间戳 %d 条已丢)· 时长 %.1fs · 时间分辨率 %.4fµs'
          % (len(edges), neg, span, tick_us))
    print('跳变间隔: 最小 %.4fµs · 中位 %.2fµs · p90 %.2fµs · p99 %.2fµs · 最大 %.2fµs'
          % (min(g), pct(sorted(g), 0.5), pct(sorted(g), 0.9), pct(sorted(g), 0.99), max(g)))

    # ---- ① 不预设门限:两撮之间的空档在哪 ----
    print('\n-- ① 全部间隔按槽折叠(找空档;空档就是门限的可用区间)--')
    slot_hist(g, slot_max)
    band = empty_band(g, slot_max + 10)
    inner_max = inter_min = 0.0
    if band:
        lo, hi = band
        # 空档左边界那一槽的中心 = 帧内/帧间的分界(间隔都落在 8.25µs 的整数倍上)
        cut = (lo + 0.5) * SLOT_US
        inner_max = max(x for x in g if x < cut)
        inter_min = min(x for x in g if x > cut)
        print('  ★ 空档 = %d~%d 槽(%d 个槽宽,直方图里 0 个样本)'
              % (lo + 1, hi - 1, hi - lo - 1))
        print('  ★ **帧内最大间隔 = %.1fµs**(%d 槽 · %d 处)· **帧间最小间隔 = %.1fµs**(%d 槽)'
              % (inner_max, lo, sum(1 for x in g if x == inner_max), inter_min, hi))
        print('    ⇒ 任何门限都必须落在 (%.1f, %.1f)µs 开区间里' % (inner_max, inter_min))
    else:
        print('  ★ 没找到空档 —— 这份抓包定不了门限,别硬猜')

    # ---- ② 门限切段 + 逐段 FCS 校验 ----
    print('\n-- ② 按门限切段、逐段重算 FCS(fit_fcs 的定案约定)--')
    res = {}
    for sp in splits:
        segs, _ = segment(edges, sp)
        res[sp] = seg_report(segs, 'split=%.0fµs' % sp)
    main = splits[0]
    print('  ⇒ 门限 %.0fµs(%s):干净收出 **%d** 帧(段内帧起点合计 %d)'
          % (main, '固件新值' if main == SPLIT_US else '候选',
             res[main][0], res[main][2]))
    for sp in splits[1:]:
        print('  ⇒ 门限 %.0fµs:干净收出 %d 帧 · 比 %.0fµs 少 **%d 帧**(并帧段 %d 处)'
              % (sp, res[sp][0], main, res[main][0] - res[sp][0], res[sp][1]))

    # ---- ③ 帧间空闲分布 ----
    # 用最小的那个门限切出来的间隔当"帧间空闲"最保险:门限越小,越不会把
    # 并帧之后的假间隔算进来(误差是单向的:报出的帧间空闲只会偏小)。
    si = sorted(segment(edges, min(splits))[1])
    print('\n-- ③ 帧间空闲分布(按 %.0fµs 门限切出的 %d 个帧边界)--'
          % (min(splits), len(si)))
    print('  最小 %.2fµs · p1 %.2fµs · 中位 %.2fµs · p90 %.2fµs · p99 %.2fµs · 最大 %.2fµs'
          % (si[0], pct(si, 0.01), pct(si, 0.5), pct(si, 0.9), pct(si, 0.99), si[-1]))
    print('  分位数(µs): %s' % ' '.join(
        'p%g=%.1f' % (q * 100, pct(si, q)) for q in (0.01, 0.05, 0.25, 0.5, 0.75, 0.9, 0.99)))
    print('  小于某个值的帧边界数(= 该门限会**并帧丢帧**的次数):')
    for t in (50, 100, 150, 200, 300, 400):
        c = sum(1 for d in si if d < t)
        flag = ''
        if t == LEGACY_US and c:
            flag = '  ← 旧门限 300µs 就在这些边界上并帧'
        if t == 50:
            flag = '  ← 必须接近 0(否则门限没法和帧内间隔分开)'
        print('    < %4dµs : %6d 个 (%.2f%%)%s' % (t, c, 100.0 * c / len(si), flag))
    if si[0] < SPLIT_US:
        print('  ★ 有帧边界比候选门限 %.0fµs 还短 —— 这个门限会并帧,必须再降' % SPLIT_US)
    print('  ⇒ 门限 %.0fµs 的两侧余量:距帧内最大 +%.1fµs(%.2f×)· 距帧间最小 -%.1fµs'
          % (main, main - inner_max, main / inner_max if inner_max else 0, si[0] - main))
    return 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    splits, slot_max = [], 30
    for i, a in enumerate(argv):
        if a == '--split' and i + 1 < len(argv):
            splits.append(float(argv[i + 1]))
        if a == '--slots' and i + 1 < len(argv):
            slot_max = int(argv[i + 1])
    if not splits:
        splits = [SPLIT_US, LEGACY_US]
    return report(path, splits, slot_max)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
