#!/usr/bin/env python3
"""从逻辑分析仪的边沿 CSV 解出 VAN 舒适总线帧,并提取**车速字段**。

为什么这么做(2026-09-19,5 分钟实车抓包 drive5min.csv):
  · 逻辑分析仪导出的时间列是**秒**,但只有 0.25µs 的量化。`edge_stats.parse_csv`
    以前对它做 int(t*1e6),8.25µs 的槽被截成 8µs —— 槽时间就永远量不出来。
    现在秒/毫秒列保留 4MHz 细网格(tick = 0.25µs)。
  · 于是量出**最小跳变间隔 = 2.0625µs**;跳变间隔全部落在它的 1/2/3/4/5 倍上
    ⇒ 位槽 = **8.25µs(≈121kbit/s)**,是固件假设的 8.00µs 的 **1.031 倍**
    —— 这正是固件 `frames=0` 的原因(每 32 位就漂掉一整个槽)。
  · 每帧都以 **SOF = 0000111101**(10 槽)开头 —— 本次抓包 **17106/17106 帧 100% 命中**。
  · SOF 之后依次是 IDEN 15 位、CMD 5 位,再接 10 槽/字节的数据区。

结论(2026-09-19):
  · **车速在 IDEN=0x44A9 / CMD=0x11 这个报文里**(实测 79.6Hz),
    数据区第 4 个 10 槽字节的低 8 位 = 帧内 bit 71..78。
  · 该字段全程**单调平滑**(42 个不同值,全部是偶数):
    停车 8 → 加速 → 巡航平台 46(出现 860 次) → 末端 81。
    低速 8 占 1019 次,说明"停车"时字段并不是 0 —— 可能带零偏。
  · ★ **标度与量程都还是推测**:
      标度 0.5km/h 一位 是按"巡航 46 ⟹ ≈23km/h"猜的(没有地面真值);
      若其实是 1km/h 一位,则巡航 46km/h、末端 81km/h。
      7 位字段上限 127,本段只到 81,**看不出量程**。
  · 所以下一步必须做一次**标定跑**:车速表分别稳在 20/40/60/80km/h 各几秒,
    斜率给标度、截距给零偏、最高点给量程 —— 一次全定。

用法:
    python tools/van-decode/decode_frames.py 抓包.csv
    python tools/van-decode/decode_frames.py 抓包.csv --speed-id 0x44A9
"""

import os
import sys
from collections import Counter, defaultdict

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import edge_stats as es

for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors='replace')
    except Exception:
        pass

QUANT_US = 2.0625        # µs:实测最小跳变间隔(跳变都落在它的整数倍上)
IDLE_US = 17.0           # 帧间空闲门限(>2 个位槽)
SOF = [0, 0, 0, 0, 1, 1, 1, 1, 0, 1]   # 17106/17106 帧命中
SOF_BITS, IDEN_BITS, CMD_BITS = 10, 15, 5
SPEED_IDEN, SPEED_CMD = 0x44A9, 0x11   # 本次抓包里带车速的报文
SPEED_OFF, SPEED_W = 71, 7             # 帧内 bit 偏移 / 位宽


def load_frames(path, idle_us=IDLE_US):
    """CSV → (帧列表, 起始 tick, tick 的 µs)。每帧是 [(tick, level), ...] 跳变表。"""
    tick_us, rows = es.parse_csv(path)
    edges = es.edges_from_levels(rows, tick_us)
    if not edges:
        return [], 0, tick_us
    t0 = edges[0][0]
    frames, cur = [], [edges[0]]
    last = edges[0][0]
    for t, lv in edges[1:]:
        if (t - last) * tick_us > idle_us:
            if len(cur) >= 6:
                frames.append(cur)
            cur = []
        cur.append((t, lv))
        last = t
    if len(cur) >= 6:
        frames.append(cur)
    return frames, t0, tick_us


def frame_to_slots(f, tick_us):
    """跳变表 → 每槽一个电平(槽宽 QUANT_US)。脉冲宽度按量化单元取整。"""
    slots = []
    lv = f[0][1]
    for i in range(len(f) - 1):
        n = max(1, int(round((f[i + 1][0] - f[i][0]) * tick_us / QUANT_US)))
        slots += [lv] * n
        lv ^= 1
    return slots


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    for i, a in enumerate(argv):
        if a == '--idle' and i + 1 < len(argv):
            pass
    frames, t0, tick_us = load_frames(path)
    if not frames:
        print('没解出帧')
        return 1
    slots = [frame_to_slots(f, tick_us) for f in frames]
    ts = [(f[0][0] - t0) * tick_us / 1e6 for f in frames]
    span = ts[-1] - ts[0]

    print('=' * 76)
    print('输入 %s' % path)
    print('时间分辨率 %.3fµs · 帧 %d · 时长 %.1fs · 帧率 %.1f/s'
          % (tick_us, len(frames), span, len(frames) / span))

    # --- 帧结构自检 ---
    ok = sum(1 for s in slots if s[:SOF_BITS] == SOF)
    print('SOF 0000111101 命中 %d/%d (%.1f%%)' % (ok, len(frames), 100.0 * ok / len(frames)))

    # --- IDEN / CMD 聚类 ---
    fam = defaultdict(list)
    for k, s in enumerate(slots):
        if s[:SOF_BITS] != SOF:
            continue
        i0 = SOF_BITS
        iden = int(''.join(map(str, s[i0:i0 + IDEN_BITS])), 2)
        cmd = int(''.join(map(str, s[i0 + IDEN_BITS:i0 + IDEN_BITS + CMD_BITS])), 2)
        fam[(iden, cmd)].append(k)

    print('\n-- 报文表(按帧数) --')
    print('   IDEN    CMD     帧数   时长µs  数据位  载荷变化形态')
    rows = []
    for (iden, cmd), mem in sorted(fam.items(), key=lambda kv: -len(kv[1])):
        if len(mem) < 20:
            continue
        nb = min(len(slots[k]) for k in mem)
        durs = [(frames[k][-1][0] - frames[k][0][0]) * tick_us for k in mem]
        body = nb - (SOF_BITS + IDEN_BITS + CMD_BITS)
        shapes = len(set(tuple(slots[k][SOF_BITS + 20:]) for k in mem))
        rows.append((iden, cmd, len(mem), sum(durs) / len(durs), body, shapes))
        print('   0x%04X  0x%02X  %5d  %7.1f  %4d     %5d (%.2f/帧)'
              % (iden, cmd, len(mem), sum(durs) / len(durs), body, shapes, shapes / len(mem)))
    if not rows:
        return 1

    # --- 车速 ---
    mem = fam.get((SPEED_IDEN, SPEED_CMD)) or fam[max(fam, key=lambda k: len(fam[k]))]
    key = (SPEED_IDEN, SPEED_CMD) if (SPEED_IDEN, SPEED_CMD) in fam else max(fam, key=lambda k: len(fam[k]))
    mem = sorted(fam[key], key=lambda k: ts[k])
    print('\n-- 车速字段: IDEN=0x%04X CMD=0x%02X,帧内 bit %d..%d (%d 位) --'
          % (key[0], key[1], SPEED_OFF, SPEED_OFF + SPEED_W - 1, SPEED_W))
    vals = [int(''.join(map(str, slots[k][SPEED_OFF:SPEED_OFF + SPEED_W])), 2) for k in mem]
    per = len(mem) / span
    print('   报文率 %.1fHz · 原始值 %d..%d (%d 个不同值)'
          % (per, min(vals), max(vals), len(set(vals))))
    print('   轨迹(每 4 秒取该秒末值,原始值 / +0.5km/h 推测标度):')
    buck = {}
    for k, v in zip(mem, vals):
        buck[int(ts[k])] = v          # 后到的覆盖前面的 → 每秒末值
    line = ['%ds:%d/%.1f' % (t, buck[t], buck[t] * 0.5)
            for t in sorted(buck) if t % 4 == 0]
    print('     ' + ' '.join(line))
    print('   ★ 标度 0.5km/h 与量程均为**推测**:需一次标定跑(稳在 20/40/60/80km/h 各几秒)')
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
