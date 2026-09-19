#!/usr/bin/env python3
"""从真实抓包里**定案** VAN 舒适总线的帧布局与 FCS/CRC 约定。

为什么要有它(2026-09-19,5 分钟实车抓包 drive5min.csv,17106 帧):
  · 固件 `frames=0` 的唯一原因是 FCS 校验过不去。而 FCS 之前"定不了"并不是
    多项式选错 —— 是**字段边界和覆盖范围**错了(旧模型:IDENlo/IDENhi 那套
    字节打包 + FCS 18 槽)。边界一旦按真实槽位切对,CRC 立刻自洽。
  · 所以这个脚本做两件事,顺序不能反:
      ① 布局:按槽位切出 IDEN/CMD/DATA/FCS/ACK,报每个报文的字节数;
      ② FCS:按定案的约定(poly 0x0F9D/init 0x7FFF/取反/MSB-first,
         覆盖 IDEN+CMD+DATA)逐帧重算,**全中才算过**。
    布局错了的话 ② 一定是 0 命中 —— 不要靠调多项式去凑。

实测结论(两个独立抓包都全中:drive5min 17106/17106、sample 66/66):
  帧(槽 = TS = 8.25µs):
    SOF      10 槽   固定 0000111101(裸槽,**不走 4B5B**)
    IDEN     15 槽   3 个 4B5B 组 = 12 位(丢每组第 5 槽)→ 线上字节流 bit11..4
    CMD       5 槽   1 个 4B5B 组 = 4 位(EXT/RAK/R-W/RTR)
    DATA     10N 槽  每字节 2 组
    FCS      20 槽   4 个 4B5B 组 = 16 位 = **15 位 CRC + 1 个固定 0 位**
    EOD       0 槽   FCS 末字节的 bit0(=0)与紧随的编码位(=0)= 一对 dominant
                     ⇒ 一次 E-Manchester 违约。线上并不额外多一个槽。
    ACK       2 槽   仅当 CMD 的 bit2=1 **且 RTR(bit0)=0**(实测只有 0xC/0xE 有):
                     第 1 槽 recessive、第 2 槽 dominant = 总线上有接收方应答。
                     0x8(bit2=0)与 0xF(RTR=1)的帧两槽都是 recessive,直接并进空闲,
                     帧体因此少 2 槽。
    EOF       8 槽   8 个连续 recessive(与帧间空闲连成一片,不在跳变表里单列)
  所以帧的槽数 = 50 + 10N (+2 若被应答),本脚本按实测长度反推 N,再验 FCS。

  编码位规则(比"每 5 槽丢第 5 槽"更强,用来定位帧尾):
    每组 5 槽 = 4 个数据位 + **第 4 位的反相**。正常的组永远有跳变;
    全帧**恰好最后一组**违反这条(= EOD)。旧脚本只按固定步长丢第 5 槽,
    所以看不出帧尾在哪。

用法:
    python tools/van-decode/fit_fcs.py 抓包.csv              # 复核(默认)
    python tools/van-decode/fit_fcs.py 抓包.csv --dump 200    # 导 (数据,FCS) 语料
    python tools/van-decode/fit_fcs.py 抓包.csv --search      # 约定对不上时重搜

★ 时间列是**秒**(Logic 2 导出,0.25µs 量化),表头可能带 UTF-8 BOM:
  edge_stats.parse_csv 会跳过非数字行,所以 BOM 不会吃掉第一条边沿
  —— 但**别自己**写 `float(line[0])` 那套(踩过:BOM 让首条边沿丢失、整段错位)。
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

SOF = '0000111101'
GAP_US = 60.0          # 帧间空闲门限:实测帧内最长同电平 = **6 槽 ≈ 49.0µs**
                       # (17105 帧里的 1232 处),帧间最短 = 95.5µs;7~11 槽是空档,
                       # 60µs 落在空档里(与固件 kIdleCloseUs=70 同一个区间,
                       # 量法见 gap_stats.py)
TS_NOMINAL_US = 8.25   # 实测槽时间(固件 kTsNs 同源);下面还会按帧重新最小二乘拟合

# ---- 定案的 FCS 约定(van_wire.cpp 的 crc15_van_iso) ----
CRC_POLY, CRC_INIT, CRC_XOR = 0x0F9D, 0x7FFF, 0x7FFF
CRC_WIDTH = 15


def fold(bits):
    v = 0
    for b in bits:
        v = (v << 1) | int(b)
    return v


def split_frames(edges, gap_us=GAP_US):
    """按空闲间隔切帧:返回 [[(t_us, level), ...], ...]"""
    if not edges:
        return []
    fr, cur, last = [], [edges[0]], edges[0][0]
    for t, lv in edges[1:]:
        if t - last > gap_us:
            if len(cur) >= 6:
                fr.append(cur)
            cur = []
        cur.append((t, lv))
        last = t
    if len(cur) >= 6:
        fr.append(cur)
    return fr


def fit_ts(widths, ts0=TS_NOMINAL_US):
    """最小二乘拟合本帧的槽时间:先用 ts0 取整,再按 sum(w·n)/sum(n²) 回代。"""
    ts = ts0
    for _ in range(6):
        ns = [max(1, int(round(w / ts))) for w in widths]
        den = sum(n * n for n in ns)
        if den == 0:
            break
        ts = sum(w * n for w, n in zip(widths, ns)) / den
    return ts


def frame_slots(f):
    """一帧的跳变表 → (每槽电平串, 本帧槽时间)。末段(空闲)不计入帧体。"""
    if len(f) < 3:
        return None, None
    w = [f[i + 1][0] - f[i][0] for i in range(len(f) - 1)]
    lv = [f[i][1] for i in range(len(f) - 1)]
    ts = fit_ts(w)
    ns = [max(1, int(round(x / ts))) for x in w]
    return ''.join(str(l) * n for l, n in zip(lv, ns)), ts


def grupoflen(s):
    """按 5 槽一组切(锚点在槽 0:SOF 正好 2 组,后续字段边界都是 5 的倍数)。"""
    return [s[i:i + 5] for i in range(0, len(s) - len(s) % 5, 5)]


def decode_frame(s):
    """槽串 → dict(iden, cmd, data bits, fcs 16 位, ack) ;布局不符返回 None。

    只看结构,不看 CRC —— CRC 由 verify_fcs() 单独判。
    """
    if s[:10] != SOF:
        return None
    groups = grupoflen(s[10:])                     # SOF 之后的组
    if len(groups) < 6:                            # 至少 IDEN3+CMD1+FCS4... 太短的直接丢
        return None
    # 槽数不是 5 的倍数 ⇒ 尾部多 2 个槽 = ACK 窗口(实测只有这一种可能)
    tail = s[len(s) - len(s) % 5:] if len(s) % 5 else ''
    ack = tail if tail else None
    if tail and len(tail) != 2:
        return None
    viol = [i for i, g in enumerate(groups) if g[4] == g[3]]   # E-Manchester 违约
    if viol != [len(groups) - 1]:
        return None                                # 正常帧**只有最后一组**违约(=EOD)
    if groups[-1][4] != '0':
        return None                                # EOD 必须是一对 dominant
    bits = ''.join(g[:4] for g in groups)
    body = bits[16:]                               # 去掉 IDEN(12)+CMD(4)
    if len(body) < 16 or (len(body) - 16) % 8:
        return None
    return {
        'iden': fold(bits[0:12]),
        'cmd': fold(bits[12:16]),
        'data': body[:len(body) - 16],
        'fcs': body[len(body) - 16:],
        'ack': ack,
        'slots': len(s),
    }


def crc15_bits(bits):
    """定案的 CRC-15:poly 0x0F9D / init 0x7FFF / 输出取反 / MSB-first(不反射)。"""
    m = (1 << CRC_WIDTH) - 1
    crc = CRC_INIT
    for b in bits:
        t = ((crc >> (CRC_WIDTH - 1)) & 1) ^ b
        crc = (crc << 1) & m
        if t:
            crc ^= CRC_POLY
    return crc ^ CRC_XOR


def crc15_bytes(data):
    """同一约定的**字节**入口(固件 crc15_van_iso 就是这个):喂 IDEN/CMD/DATA。"""
    bits = []
    for by in data:
        bits += [(by >> (7 - i)) & 1 for i in range(8)]
    return crc15_bits(bits)


def wire_bytes(fr):
    """帧 → 线上字节流(固件 BitDecoder 吐出来的那串):
    [IDEN>>4, (IDEN&0xF)<<4|CMD, DATA..., FCS_hi, FCS_lo]"""
    iden, cmd = fr['iden'], fr['cmd']
    out = [(iden >> 4) & 0xFF, ((iden & 0x0F) << 4) | (cmd & 0x0F)]
    out += [int(fr['data'][i * 8:(i + 1) * 8], 2) for i in range(len(fr['data']) // 8)]
    field = int(fr['fcs'], 2)
    out += [(field >> 8) & 0xFF, field & 0xFF]
    return bytes(out)


def verify_fcs(fr):
    """FCS 16 位 = [15 位 CRC][固定 0];比对时取 field>>1。"""
    field = int(fr['fcs'], 2)
    if field & 1:
        return False
    return (field >> 1) == crc15_bits([int(c) for c in
                                       '{:012b}{:04b}{}'.format(fr['iden'], fr['cmd'], fr['data'])])


def collect(path):
    """CSV → (帧列表, 每帧的槽串, tick_us)。"""
    tick_us, rows = es.parse_csv(path)
    edges = es.edges_from_levels(rows, tick_us)
    frames = split_frames(edges)
    out = []
    for f in frames:
        s, ts = frame_slots(f)
        if s:
            out.append((s, ts))
    return len(edges), out


def report(path, dump=None, search=False):
    n_edges, slots = collect(path)
    if not slots:
        print('没解出帧 —— 先确认 CSV 里 CH0 在跳变(edge_stats.py 能出直方图吗?)')
        return 1
    print('=' * 78)
    print('%s:边沿 %d · 帧段 %d' % (path, n_edges, len(slots)))

    ok_sof = sum(1 for s, _ in slots if s[:10] == SOF)
    tss = sorted(ts for _, ts in slots)
    print('槽时间拟合:中位 %.4fµs(p10 %.4f / p90 %.4f) · SOF 命中 %d/%d'
          % (tss[len(tss) // 2], tss[len(tss) // 10], tss[len(tss) * 9 // 10],
             ok_sof, len(slots)))

    fam = defaultdict(list)
    bad = []
    for s, _ in slots:
        d = decode_frame(s)
        if d is None:
            bad.append(s)
            continue
        fam[(d['iden'], d['cmd'])].append(d)
    print('布局可解 %d/%d(不可解 %d 条:多半是抓包首尾的半帧)'
          % (sum(len(v) for v in fam.values()), len(slots), len(bad)))

    # ---- 报文表:长度是否定长、数据几字节、是否被应答、FCS 是否全中 ----
    print('\n-- 报文表(槽数/字节数都是**实测**;FCS 按定案约定重算) --')
    print('   IDEN    CMD  帧数  帧体槽数  数据字节  ACK   FCS 通过   槽数校验')
    tot = fcs_ok = 0
    acked = unacked = 0
    for (iden, cmd), mem in sorted(fam.items(), key=lambda kv: -len(kv[1])):
        nbytes = Counter(len(m['data']) // 8 for m in mem)
        nslots = Counter(m['slots'] for m in mem)
        ack = Counter('有' if m['ack'] else '无' for m in mem)
        good = sum(1 for m in mem if verify_fcs(m))
        tot += len(mem)
        fcs_ok += good
        acked += sum(1 for m in mem if m['ack'])
        unacked += sum(1 for m in mem if not m['ack'])
        # 槽数换算:SOF10 + IDEN15 + CMD5 + 10·N + FCS20 (+ACK2 若被应答)
        exp = set(50 + 10 * n + (2 if a else 0) for n in nbytes for a in (0, 1))
        exp &= set([50 + 10 * n + (2 if ack.get('有', 0) else 0) for n in nbytes])
        flag = '✓' if set(nslots) <= exp else '★实测槽数与换算不符!'
        print('  0x%03X 0x%X %6d  %-12s %-9s %-5s %5d/%d  %s' % (
            iden, cmd, len(mem), sorted(nslots), sorted(nbytes),
            '有' if not ack.get('无') else ('无' if not ack.get('有') else '混'),
            good, len(mem), flag))
    print('\n★ FCS 复核:**%d/%d 帧通过**(约定 poly=0x%04X init=0x%04X 取反=0x%04X,'
          '覆盖 IDEN+CMD+DATA,MSB-first)' % (fcs_ok, tot, CRC_POLY, CRC_INIT, CRC_XOR))
    # ACK 的存在与 CMD 一一对应(实测:bit2=1 且 RTR=0,即 0xC/0xE):
    # 对不上说明布局还有别的分支
    mix = sum(1 for v in fam.values() for m in v
              if bool(m['ack']) != bool(m['cmd'] & 0x4 and not m['cmd'] & 0x1))
    print('  被应答 %d 帧 / 未被应答 %d 帧;ACK 与 "CMD bit2=1 且 RTR=0" 不一致 %d 帧 %s'
          % (acked, unacked, mix, '✓' if mix == 0 else '★需重新看布局'))
    if fcs_ok != tot:
        print('  ★ 有帧不通过 —— 布局或约定变了,**别改多项式去凑**:'
              '先看 --dump 出来的语料哪一批不对,再用 --search 重搜')

    if dump:
        p = dump if isinstance(dump, str) else 'fcs_corpus.txt'
        with open(p, 'w', encoding='utf-8') as fh:
            fh.write('# iden cmd data_hex fcs16_bits\n')
            seen = set()
            for (iden, cmd), mem in fam.items():
                for m in mem:
                    key = (iden, cmd, m['data'], m['fcs'])
                    if key in seen:
                        continue
                    seen.add(key)
                    fh.write('%03X %X %s %s\n' % (iden, cmd, m['data'], m['fcs']))
        print('  语料已写 %s(%d 条去重)' % (p, len(seen)))

    if search:
        search_fit(fam)
    return 0 if fcs_ok == tot else 1


def search_fit(fam):
    """约定对不上时重搜:枚举 15 位多项式 × 初值 × 位序 × 覆盖范围。

    只在短帧上初筛,再拿**留出帧**(每个报文族各取若干)复核 ——
    单帧命中的组合有几百个,不复核等于没做。
    """
    def bits_of(m):
        return [int(c) for c in '{:012b}{:04b}{}'.format(m['iden'], m['cmd'], m['data'])]

    def targets(m):
        fb = [int(c) for c in m['fcs']]
        return {'f15+0': fold(fb[:15]), '0+f15': fold(fb[1:]), 'f16': fold(fb)}

    def crc(bits, poly, init):
        m = 0x7FFF
        c = init
        for b in bits:
            t = ((c >> 14) & 1) ^ b
            c = (c << 1) & m
            if t:
                c ^= poly
        return c

    def mirror(v):
        r = 0
        for i in range(15):
            if (v >> i) & 1:
                r |= 1 << (14 - i)
        return r

    allf = [(len(m['data']), m) for v in fam.values() for m in v]
    allf.sort(key=lambda x: x[0])
    if not allf:
        return
    fit = [allf[0][1]]
    test = [allf[i][1] for i in range(1, min(len(allf), 400), 7)]
    cov = {'iden+cmd+data': bits_of, 'data': lambda m: [int(c) for c in m['data']]}
    for name, fn in cov.items():
        hits = []
        b0 = fn(fit[0])
        t0 = targets(fit[0])
        for poly in range(1, 0x8000):
            for init in (0, 0x7FFF):
                for d in ('msb', 'lsb'):
                    v = crc(b0 if d == 'msb' else list(reversed(b0)),
                            poly if d == 'msb' else mirror(poly), init)
                    for tn, tv in t0.items():
                        for xo in (0, 0x7FFF):
                            if v ^ xo == tv:
                                hits.append((poly, init, d, tn, xo))
        keep = []
        for (poly, init, d, tn, xo) in hits:
            ok = True
            for m in test:
                b = fn(m)
                v = crc(b if d == 'msb' else list(reversed(b)),
                        poly if d == 'msb' else mirror(poly), init)
                if v ^ xo != targets(m)[tn]:
                    ok = False
                    break
            if ok:
                keep.append((poly, init, d, tn, xo))
        print('  [search] 覆盖=%s 初筛 %d 个 → 留出帧复核后 %d 个 %s'
              % (name, len(hits), len(keep), [hex(k[0]) for k in keep[:4]]))


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    path = argv[1]
    dump = None
    for i, a in enumerate(argv):
        if a == '--dump':
            dump = argv[i + 1] if i + 1 < len(argv) and not argv[i + 1].startswith('-') else 'fcs_corpus.txt'
    return report(path, dump, '--search' in argv)


if __name__ == '__main__':
    sys.exit(main(sys.argv))
