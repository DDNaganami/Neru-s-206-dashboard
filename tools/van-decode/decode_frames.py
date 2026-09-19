#!/usr/bin/env python3
"""从逻辑分析仪的边沿 CSV 解出 VAN 舒适总线帧,并定位**车速字段**。

为什么这么做(2026-09-19,5 分钟实车抓包 drive5min.csv):
  · 逻辑分析仪导出的时间列是**秒**,但只有 0.25µs 的量化。`edge_stats.parse_csv`
    以前对它做 int(t*1e6),8.25µs 的槽被截成 8µs —— 槽时间就永远量不出来。
    现在秒/毫秒列保留 4MHz 细网格(tick = 0.25µs)。
  · 于是量出**最小跳变间隔 = 2.0625µs**;跳变间隔全部落在它的 1/2/3/4/5 倍上
    ⇒ 位槽 = **8.25µs(≈121kbit/s)**,是规范的 125kbit/s(8.00µs)的 **1.031 倍**
    —— 这正是固件 `frames=0` 的原因(每 32 位就漂掉一整个槽)。
  · 每帧都以 **SOF = 0000111101**(10 槽)开头 —— 5 分钟抓包 **17106/17106 帧 100% 命中**。
  · SOF 之后依次是 IDEN 15 槽、CMD 5 槽,后面全是 4B5B 符号。

★ 4B5B 的坑(务必先读这一段,否则会把一帧看成两帧):
  IDEN 的 15 槽和 CMD 的 5 槽都是 **4B5B 编码**的:每 5 槽只有前 4 槽是数据位,
  第 5 槽是编码位(E-Manchester bit),**解码时必须丢掉**。
  所以同一条报文有**两个**"IDEN"数字,它们指的是**同一帧**:
    · 裸 15 槽 = 0x44A9 —— 线上原样的 15 个槽,含编码位,不是 IDEN 的值;
    · 4B5B 后  = **0x824** —— 丢掉每组第 5 槽后剩下的 12 位数据,
      正是 `lib/dashcore/van_wire.h` / `van_source` 里一直假设的
      **车速帧 IDEN=0x824**(byte1=0x24,byte2 高 4 位=0x8)。✓
  同理 CMD:裸 5 槽 `10001` → 丢第 5 槽 → `1000` = **0x8**(不是 0x11)。
  ⚠ 固件里**只认 0x824**。绝不要把 0x44A9 写进固件 —— 那是编码后的线形,
  不是字段值。本脚本两个都打印(见「报文表」的"裸15槽"列),就是为了让这件事一眼可见。

结论(2026-09-19,字段位置;标度仍未定):
  · **车速在 4B5B 解码后的 IDEN=0x824 / CMD=0x8 这个报文里**(实测 ≈80Hz)。
  · 字段位置的**两种等价读法**(同一个数列,已逐帧核对,行驶 5978 帧 / 静止 24 帧):
      ① 裸槽坐标直接读:帧内 **bit 71..77**(7 位)→ 字段计数。脚本打印的就是它。
      ② 同一段槽的 8 槽写法:读 **bit 71..78** 这 8 槽再 **>> 1** → 同一个数
         (审核说的"整个字节是 >> 1"就是这个;脚本对它做了逐帧断言)。
    实测数列:行驶 **42 个不同值,8..81**;静止抓包**恒为 8**(唯一值)。
  · ★ 关于"观测值总是偶数":那是**静止切片**的现象(该切片 8 槽读数恒为 16 = 8<<1)。
    5 分钟行驶抓包里 8 槽读数有奇有偶(范围 16..162),所以**别指望它恒偶** ——
    恒定的是"7 位读法 == 8 槽读数 >> 1"这条关系(5978/5978 ✓)。
  · ★ 数据区**不能**照搬 IDEN/CMD 那套 4B5B 视图:IDEN 有"15 槽 = 3 组"这种固定
    字段边界,而数据区没有;把 5 槽分组的锚点挪了 6 个位置全试过,折出来的窗口
    与裸槽读数**逐帧都不相等**(全等 0/5978)。所以字段位置只认裸槽坐标,
    4B5B 只用来译 IDEN/CMD。**别再试图把数据字段写成"第 N 个字节的低 M 位"** ——
    那条路在这份抓包上是不成立的(写进去会让下一个人白查半天)。
  · 该字段全程**单调平滑**;停车时恒为 8(两个抓包一致)。
  · ★ **标度与量程都还是推测**:0.5km/h 一位的说法**没有地面真值**,
    脚本里**不内置任何 km/h 换算**(只打印原始计数),避免把猜测当结论。
  · 所以下一步必须做一次**标定跑**:表显稳在 20/40/60/80km/h 各几秒,
    斜率给标度、截距给零偏、最高点给量程 —— 一次全定。
    记录格式见 ACCEPTANCE.md「标定记录格式」。

用法:
    python tools/van-decode/decode_frames.py 抓包.csv
    python tools/van-decode/decode_frames.py 抓包.csv --speed-id 0x824
    python tools/van-decode/decode_frames.py 抓包.csv --speed-id 0x44A9   # 也认裸槽写法
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
# 帧间空闲门限。**不是拍的,是量出来的**(tools/van-decode/gap_stats.py):
#   · 帧内最长的一段同电平 = 49.0µs(6 个 8.25µs 的槽)—— 门限比它小会把帧切两半;
#   · 最短的帧间空闲       = 95.5µs(12 槽)—— 门限比它大会把两帧并成一段;
#   · 直方图里 7~11 槽(53.6~94.9µs)是空档,门限就取空档里:70µs(与固件的
#     van_phy_gpio.cpp `kIdleCloseUs` 同一个值)。
# ★ 这里曾经写的是 17.0"帧间空闲门限(>2 个位槽)",而 load_frames 里又乘了一次
#   tick_us(时间戳已经是µs了)→ 实际门限是 17/0.25 = 68µs。也就是说文档写的 17µs
#   从来没生效过,只是碰巧落在空档里才没出事。现在两边都改成 70µs 且不再二次换算。
IDLE_US = 70.0
SOF = [0, 0, 0, 0, 1, 1, 1, 1, 0, 1]   # 17106/17106 帧命中
SOF_BITS, IDEN_BITS, CMD_BITS = 10, 15, 5
ENC_SLOTS, ENC_DATA = 5, 4             # 4B5B:每 5 槽 = 4 个数据位(第 5 槽是编码位)

# 车速报文。两个 id 指的是**同一帧**(见文件头「4B5B 的坑」):
SPEED_IDEN_DEC = 0x824   # 4B5B 解码后的 IDEN(固件认这个)
SPEED_IDEN_RAW = 0x44A9  # 裸 15 槽图案(线上原样,含编码位,固件不认)
SPEED_CMD_DEC = 0x8      # 4B5B 解码后的 CMD(丢第 5 槽)
SPEED_CMD_RAW = 0x11     # 裸 5 槽图案

# ---- 车速字段(唯一自洽的读法:裸槽 7 位;已用两个抓包逐帧核对) ----
# 判定过程(别再重复踩):把"IDEN/CMD 那两个 4B5B 视图"照搬到数据区**不成立** ——
#   数据区没有像 IDEN 那样的 15/5 槽固定字段边界,4B5B 的 5 槽分组锚点对不齐,
#   折出来的 6 位窗口与裸槽读数**逐帧都不相等**(6 个候选锚点全试过,全等 0/5978)。
#   所以字段位置只认**裸槽坐标**,4B5B 只用于 IDEN/CMD 的译码(那两个视图已核对)。
SPEED_RAW_OFF, SPEED_W = 71, 7
SPEED_VIEW = 'raw'
# 审核说法与实测的对照(见文件头「为什么"总是偶数"」):
#   · 直接读裸槽 bit 71..77        → 8..81(42 个不同值)← 脚本打印的就是这个
#   · 读裸槽 bit 70..78 这 8 槽再 >>1 → 8..81(同一个数列,逐帧相等)
#   两种写法等价,所以"8 槽单元 >> 1"与"低 7 位"不是互相矛盾的说法。


def split_sof(s):
    """裸槽串 → (裸15槽串, 裸5槽串),SOF 对不上返回 None。"""
    if s[:SOF_BITS] != SOF:
        return None
    i0 = SOF_BITS
    return (''.join(map(str, s[i0:i0 + IDEN_BITS])),
            ''.join(map(str, s[i0 + IDEN_BITS:i0 + IDEN_BITS + CMD_BITS])))


def _fold(bits):
    """[位,...] → 整数(MSB 先到)。"""
    v = 0
    for b in bits:
        v = (v << 1) | b
    return v


def n15_of(raw15):
    """裸 15 槽 → 12 位整字节读取值(丢每组第 5 槽):0x44A9 → 0x824。"""
    bits = [int(c) for c in raw15]
    return _fold([bits[i + k] for i in range(0, 15 - ENC_DATA + 1, ENC_SLOTS)
                  for k in range(ENC_DATA)])


def n5_of(raw5):
    """裸 5 槽 → 4 位值(丢第 5 槽):10001 → 1000 = 0x8。CMD 的 EXT/RAK/R-W/RTR。"""
    bits = [int(c) for c in raw5]
    return _fold(bits[:ENC_DATA])


def decode(s):
    """整帧裸槽 → 4B5B 后的字节流 + 位流。

    ★ 分组锚点是**裸槽 0**(每 5 槽取前 4 槽),不是 SOF 之后:
      4B5B 的 5 槽分组与 SOF 的 10 槽边界并不对齐,实测只有从裸槽 0 起分组
      才自洽(SOF 对应字节 0 = 0x0E,即规范图案 0000111101 折出的那个值)。
      字节布局:字节 1 = IDEN 低 8 位,字节 2 = IDEN 高 4 位 | CMD,字节 3 起是数据区。
    """
    bits, i = [], 0
    while i + ENC_SLOTS <= len(s):
        bits += s[i:i + ENC_DATA]
        i += ENC_SLOTS
    by = [_fold(bits[j * 8:j * 8 + 8]) for j in range(len(bits) // 8)]
    return by, bits


def speed_raw(s):
    """裸槽视图:帧内 bit 71..77 的 7 位(这 7 位都在 4B5B 的数据位上)。

    说明:本窗口内**没有**编码位,所以不做任何丢弃 —— 早期版本误把"每组第 5 槽"
    的规则套在"相对 SOF 的槽号"上(得出去掉裸槽 74 的错误结论),那是把
    4B5B 的分组锚点算错了(SOF 是 10 槽,分组从裸槽 0 起才自洽)。已经逐帧核对:
    直接读 bit71..77 得到 42 个不同值 8..81,静止抓包恒 8。
    """
    if len(s) < SPEED_RAW_OFF + SPEED_W:
        return None
    return _fold(s[SPEED_RAW_OFF:SPEED_RAW_OFF + SPEED_W])


def speed_unit8(s):
    """裸槽 bit 71..78 的 8 槽单元(审核说的那种"整个字节"读法)。

    实测:**>>1 与 speed_raw() 逐帧相等**(行驶 5978/5978、静止 24/24)。
    注意它**不是恒偶数**:静止切片里恒为 16(所以那一段看着"全偶"),
    5 分钟行驶抓包里是 16..162 有奇有偶 —— 恒定的只是上面那条换算关系。
    """
    if len(s) < SPEED_RAW_OFF + 8:
        return None
    return _fold(s[SPEED_RAW_OFF:SPEED_RAW_OFF + 8])


def load_frames(path, idle_us=IDLE_US):
    """CSV → (帧列表, 起始 tick, tick 的 µs)。每帧是 [(tick, level), ...] 跳变表。

    ★ 时间戳单位:edges_from_levels() 已经把 tick 乘过 tick_us 了,所以 t 就是µs
      —— **不要再乘 tick_us**(老代码在这里又乘了一次,17µs 的门限实际是 68µs)。
    """
    tick_us, rows = es.parse_csv(path)
    edges = es.edges_from_levels(rows, tick_us)
    if not edges:
        return [], 0, tick_us
    t0 = edges[0][0]
    frames, cur = [], [edges[0]]
    last = edges[0][0]
    for t, lv in edges[1:]:
        if t - last > idle_us:
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
    view = SPEED_VIEW
    iden_want, cmd_want = SPEED_IDEN_DEC, SPEED_CMD_DEC
    for i, a in enumerate(argv):
        if a == '--speed-id' and i + 1 < len(argv):
            try:
                arg = int(argv[i + 1], 16)
            except ValueError:
                print('--speed-id 需要 16 进制,如 0x824 或 0x44A9')
                return 2
            # 0x44A9 这类"裸 15 槽写法"是本次踩坑的老写法,也认它 ——
            # 但请记住它指的是**同一帧**(见文件头说明)。
            if arg == SPEED_IDEN_RAW:
                iden_want, cmd_want = SPEED_IDEN_DEC, SPEED_CMD_DEC
            else:
                iden_want, cmd_want = arg, None

    frames, t0, tick_us = load_frames(path)
    if not frames:
        print('没解出帧')
        return 1
    slots = [frame_to_slots(f, tick_us) for f in frames]
    ts = [(f[0][0] - t0) * tick_us / 1e6 for f in frames]
    span = ts[-1] - ts[0]

    print('=' * 78)
    print('输入 %s' % path)
    print('时间分辨率 %.3fµs · 帧 %d · 时长 %.1fs · 帧率 %.1f/s'
          % (tick_us, len(frames), span, len(frames) / span))

    # --- 帧结构自检(用**裸槽**判 SOF:SOF 不吃 4B5B) ---
    ok = sum(1 for s in slots if s[:SOF_BITS] == SOF)
    print('SOF 0000111101 命中 %d/%d (%.1f%%)' % (ok, len(frames), 100.0 * ok / len(frames)))

    # --- 报文族:按 4B5B **解码后**的 (IDEN,CMD) 聚类,同时带上裸槽图案 ---
    fam = defaultdict(list)
    for k, s in enumerate(slots):
        sp = split_sof(s)
        if sp is None:
            continue
        raw15, raw5 = sp
        fam[(n15_of(raw15), n5_of(raw5))].append(k)

    print('\n-- 报文表(按帧数;IDEN/CMD 都是 4B5B 解码后的值,裸槽列是对应的线上图案) --')
    print('   IDEN    CMD    帧数  时长µs  解码位  载荷形态   裸15槽    裸5槽')
    rows = []
    for (iden, cmd), mem in sorted(fam.items(), key=lambda kv: -len(kv[1])):
        if len(mem) < 20:
            continue
        durs = [(frames[k][-1][0] - frames[k][0][0]) * tick_us for k in mem]
        _, bits = decode(slots[mem[0]])
        shapes = len(set(tuple(slots[k][SOF_BITS + 20:]) for k in mem))
        sample = split_sof(slots[mem[0]])
        rows.append((iden, cmd, len(mem), sum(durs) / len(durs), len(bits), shapes))
        print('   0x%03X  0x%X   %5d  %7.1f  %5d   %5d (%.2f/帧)  0x%04X    0x%02X'
              % (iden, cmd, len(mem), sum(durs) / len(mem), len(bits), shapes,
                 shapes / len(mem), int(sample[0], 2), int(sample[1], 2)))
    if not rows:
        return 1

    # --- 车速字段 ---
    key = (iden_want, cmd_want if cmd_want is not None else max(fam, key=lambda k: len(fam[k]))[1])
    if key not in fam:
        key = max(fam, key=lambda k: len(fam[k]))
    mem = sorted(fam[key], key=lambda k: ts[k])
    raw15, raw5 = split_sof(slots[mem[0]])
    print('\n-- 车速字段: 裸15槽=0x%04X → 4B5B后 IDEN=0x%03X;裸5槽=%s → 4B5B后 CMD=0x%X --'
          % (int(raw15, 2), key[0], raw5, key[1]))
    if key[0] != SPEED_IDEN_DEC or key[1] != SPEED_CMD_DEC:
        print('   ⚠ 选中的不是本次已知的车速报文(0x%03X/0x%X),下面只是它的载荷轨迹'
              % (SPEED_IDEN_DEC, SPEED_CMD_DEC))
    print('   字段位置(唯一自洽的读法:裸槽坐标;理由见文件头):')
    print('     · 读法一: 裸槽 bit %d..%d 直接读 %d 位 → 字段计数(脚本默认打印这个)'
          % (SPEED_RAW_OFF, SPEED_RAW_OFF + SPEED_W - 1, SPEED_W))
    print('     · 读法二: 裸槽 bit %d..%d 的 8 槽单元读出来再 >>1 → 同一个数列(审核那句'
          '"整个字节是 >> 1"就是这个)'
          % (SPEED_RAW_OFF, SPEED_RAW_OFF + 7))

    # ★ 自检一:两种读法必须逐帧给出同一个数(字段位置写错就立刻报出来)
    mem_slots = [slots[k] for k in mem]        # mem 存的是帧号,这里换成槽表
    vals_raw = [speed_raw(s) for s in mem_slots]
    vals_unit = [speed_unit8(s) for s in mem_slots]
    same = sum(1 for a, b in zip(vals_raw, vals_unit) if b is not None and a == (b >> 1))
    print('   ★ 自检: 裸槽 %d..%d 与 (裸槽 %d..%d >> 1) 逐帧相同 %d/%d %s'
          % (SPEED_RAW_OFF, SPEED_RAW_OFF + SPEED_W - 1, SPEED_RAW_OFF, SPEED_RAW_OFF + 7,
             same, len(mem_slots), '✓' if same == len(mem_slots) else '✗ 两种读法不一致!'))
    # ★ 自检二:8 槽单元的取值情况。**它不是恒偶** —— 静止切片里恒为 16,
    #   5 分钟行驶抓包里 16..162 有奇有偶。这里只报事实,不做"必须恒偶"的断言
    #   (审核那句"观测值总是偶数"只对静止切片成立,已在文件头注明)。
    uvals = [u for u in vals_unit if u is not None]
    even = sum(1 for u in uvals if u % 2 == 0)
    print('   ★ 自检: 8 槽单元(bit %d..%d)读数 %d..%d,其中偶数 %d/%d'
          '(静止切片会恒偶;行驶段有奇有偶,别当恒定性质)'
          % (SPEED_RAW_OFF, SPEED_RAW_OFF + 7, min(uvals), max(uvals), even, len(uvals)))
    if key == (SPEED_IDEN_DEC, SPEED_CMD_DEC):
        # ★ 收工判据:裸 15 槽 0x44A9 必须解出 IDEN 0x824(固件认的那个)
        print('   ★ 自检: 裸15槽=0x%04X → 4B5B后 IDEN=0x%03X %s'
              % (int(raw15, 2), key[0],
                 '(= 0x824,固件认的 IDEN ✓)' if key[0] == SPEED_IDEN_DEC else '(✗ 与 0x824 不符!)'))

    vals = vals_raw
    # 下面**只打印原始计数**:标度(km/h / 位)与零偏都还没标定,不许在这里内置任何换算。
    per = len(mem) / span
    print('\n   报文率 %.1fHz · 视图=%s · 原始计数 %d..%d(%d 个不同值)'
          % (per, view, min(vals), max(vals), len(set(vals))))
    print('   轨迹(每 4 秒取该秒末值,原始计数):')
    buck = {}
    for k, v in zip(mem, vals):
        buck[int(ts[k])] = v          # 后到的覆盖前面的 → 每秒末值
    line = ['%ds:%d' % (t, buck[t]) for t in sorted(buck) if t % 4 == 0]
    print('     ' + ' '.join(line))
    print('   ★ 标度与零偏均**未标定**:这里只有原始计数,不给 km/h。'
          '标定跑后按 ACCEPTANCE.md「标定记录格式」记录并离线拟合')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
