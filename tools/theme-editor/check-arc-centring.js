/* ============================================================
 * check-arc-centring.js —— 红区(爆转)/超速那条**红弧必须和灰轨道同一条中线**
 *
 *   node tools/theme-editor/check-arc-centring.js [--frames <dir>]
 *     默认读 preview/frames（由 `pio run -e pcpreview` 落帧产生）
 *   退出码：0 = 全绿；1 = 有判据红；2 = 没落帧（先跑预览，不是"通过"）
 *
 * ------------------------------------------------------------
 * 这判据在防什么（车主 2026-10-17："爆转和超速的红色表盘弧没有居中"）
 * ------------------------------------------------------------
 * `arc_apply_red()` 原来**只把 INDICATOR 那一半加粗**到 1.25 倍。LVGL 的弧
 * `radius` 是**外沿**、线宽只往**里**长（`lvgl/src/draw/lv_draw_arc.c` 里
 * `rin = radius - w`；`lv_draw_sw_arc.c` 的 mask 也是"外圆 radius 挖掉内圆
 * radius-w"）⇒ 两个 part 共用外沿，谁加粗谁就往圆心方向变厚：
 *     灰轨道（宽 24）：r ∈ [181,205)   中线 193
 *     红弧  （宽 30）：r ∈ [175,205)   中线 190   ← **比轨道靠里 3 px**
 * 这就是"看着歪"的来源：不是弧画错了角度，是**红带的中线不在轨道中线上**。
 *
 * 修法（`src/dash_ui.cpp` 的 `arc_apply_red`）：红区时**两个 part 一起加粗**
 * （MAIN = 轨道、INDICATOR = 点亮段）。外沿不动 ⇒ 不越出对象边界、不会被裁；
 * 两者内沿同步往里 ⇒ 红带与灰带**逐像素落在同一条带上**。
 *
 * ------------------------------------------------------------
 * 纪律
 * ------------------------------------------------------------
 * · BMP 解码用 `check-face-image.js` 导出的 `readBmp`（不在这里再写第二套）。
 * · 判据是**覆盖率加权**的径向统计（见 measureLit / measureTrack），不是
 *   "沿一条半径数像素"：后者受坐标取整影响，边缘能差 1 px 以上。
 * · 第 ① 节是**自检**：用解析几何合成"修复前/后"两张图，断言同一套度量能
 *   把它们分开（修复前必须被判红）——否则第 ②③ 节可能是空转。
 * · 修复前的真帧读数记在下面 DEFECT_* 常量里（2026-10-17 实测，不是推算）。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");
const { readBmp } = require(path.join(__dirname, "check-face-image.js"));

// ---- 转盘几何（主题默认值，也是车主在用的那份）----
const CX = 240, CY = 240;          // 480×480 屏心
const R_OUT = 205;                 // ArcStyle.radius = **外沿**
const W_NORMAL = 24;               // ArcStyle.width
const W_RED = 30;                  // round(24 × 1.25)
const RED_SCALE = 1.25;

// ---- 落帧里的实际颜色（RGB565 回程后的值，实测）----
const BG = [16, 20, 16];           // 0x141414 底图
const RED = [255, 76, 74];         // THEME_REDLINE_ARC_COLOR 0xFF4D4D
const TRACK = [24, 24, 24];        // track_color 0x2A2A2A @opa150 压在底图上
const LIT_ORANGE = [255, 93, 93];  // 默认主题转速/速度弧的点亮色 0xFF5C5C

// 修复前实测（2026-10-17，`preview/frames` 改前那一批）：
//   超速屏：红弧 [175.50,205.93] 中线 190.71 / 灰轨道 [180.79,204.60] 中线 192.69
//   ⇒ 内沿差 5.29 px、宽度差 6.62 px、中线差 1.98 px
const DEFECT_INNER_GAP = 5.29;
const DEFECT_WIDTH_GAP = 6.62;

let pass = 0, fail = 0;
const failures = [];
function ok(cond, what) {
  if (cond) pass++;
  else { fail++; failures.push(what); console.log("  ✗ " + what); }
}
function near(a, b, tol, what) {
  ok(Math.abs(a - b) <= tol, what + "  (" + fmt(a) + " vs " + fmt(b) + "，容差 ±" + tol + ")");
}
function atMost(a, lim, what) {
  ok(a <= lim, what + "  (" + fmt(a) + "，上限 " + lim + ")");
}
function fmt(v) { return typeof v === "number" ? v.toFixed(2) : String(v); }
function section(t) { console.log("\n== " + t); }

// ------------------------------------------------------------
// 覆盖率：把像素投影到 (bg → ref) 这条线上，残留大的（换色了）直接排除。
// 这样绿色的水温弧、白色的数字都不可能被算成"红弧的一部分"。
// ------------------------------------------------------------
function covProj(pixel, bg, ref) {
  const d = [ref[0] - bg[0], ref[1] - bg[1], ref[2] - bg[2]];
  const v = [pixel[0] - bg[0], pixel[1] - bg[1], pixel[2] - bg[2]];
  const dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
  if (!dd) return 0;
  const t = (v[0] * d[0] + v[1] * d[1] + v[2] * d[2]) / dd;
  if (t < 0.10) return 0;
  const res = [v[0] - t * d[0], v[1] - t * d[1], v[2] - t * d[2]];
  const rn = Math.sqrt(res[0] * res[0] + res[1] * res[1] + res[2] * res[2]);
  const dn = Math.sqrt(dd) * Math.max(t, 1e-6);
  if (rn / dn > 0.22) return 0;
  return Math.min(1, t);
}

// 灰轨道：半透明灰压在底图上，只有 ~7 级亮度差 ⇒ 用亮度做覆盖率，另加"必须是灰"的闸门
const BG_L = (BG[0] + BG[1] + BG[2]) / 3;
const TRK_L = (TRACK[0] + TRACK[1] + TRACK[2]) / 3;
function covTrack(pixel) {
  const [r, g, b] = pixel;
  if (Math.abs(r - g) > 8 || Math.abs(g - b) > 8) return 0;      // 不是灰的就不是轨道
  const l = (r + g + b) / 3;
  if (l > TRK_L + 3) return 0;                                    // 比轨道亮 = 别的东西
  const c = (l - BG_L) / (TRK_L - BG_L);
  return c <= 0.05 ? 0 : Math.min(1, c);
}

// 角度窗口：只取"这一段里只有我们要量的那条带"的范围（避开端帽、读数、表情）
function inWindows(deg, windows, degUnwrapped) {
  const d = degUnwrapped === undefined ? deg : degUnwrapped;
  for (const [a, b] of windows) if (d >= a && d <= b) return true;
  return false;
}

// ------------------------------------------------------------
// 径向统计：对圆环内**所有**像素做"覆盖率加权"的一阶/二阶矩
//   均匀带 [a,b] ⇒ 均值 = (a+b)/2、宽度 = sqrt(12·方差) = b-a
// 比"沿一条半径数连续像素"稳得多（后者受取整影响，边缘能差 1 px 以上）。
// kind = "lit"（亮起段，用颜色投影）| "track"（灰轨道，用亮度）
// ------------------------------------------------------------
function measure(img, kind, windows, rlo, rhi) {
  const ref = kind === "lit" ? RED : TRACK;
  let sw = 0, sr = 0, sr2 = 0, n = 0;
  for (let y = CY - rhi; y <= CY + rhi; y++) {
    for (let x = CX - rhi; x <= CX + rhi; x++) {
      if (x < 0 || y < 0 || x >= img.w || y >= img.h) continue;
      const dx = x - CX, dy = y - CY;
      const r = Math.sqrt(dx * dx + dy * dy);
      if (r < rlo || r > rhi) continue;
      let deg = (Math.atan2(dy, dx) * 180) / Math.PI;
      if (deg < 0) deg += 360;
      if (!inWindows(deg, windows)) continue;
      const p = img.px(x, y);
      const c = kind === "lit" ? covProj(p, BG, ref) : covTrack(p);
      if (c <= 0) continue;
      sw += c; sr += c * r; sr2 += c * r * r; n++;
    }
  }
  if (sw === 0) return null;
  const mean = sr / sw;
  const varr = Math.max(0, sr2 / sw - mean * mean);
  const width = Math.sqrt(12 * varr);
  return { centre: mean, width, inner: mean - width / 2, outer: mean + width / 2, px: n, weight: sw };
}

// ------------------------------------------------------------
// ① 自检：解析几何合成两张图（修复前 / 修复后），本文件的度量必须能分开它们
//
// ★ 合成图必须**照真帧的样子画**：灰轨道是整圈（MAIN 弧 135°→405° = 一整圈），
//   红弧只在点亮的那一段角度上出现。要是把红弧画成整圈，"量轨道"的那个角度
//   窗口里就只剩红弧的淡边被当成"灰轨道"，量出来的宽度能到 40 px（第一次就
//   是这么写的，自检当场把它抓出来了 —— 这也是自检存在的理由）。
// ------------------------------------------------------------
function synthBand(trackWidth, redWidth, redWindow) {
  return {
    w: 480, h: 480,
    px(x, y) {
      const dx = x - CX, dy = y - CY;
      const r = Math.sqrt(dx * dx + dy * dy);
      let deg = (Math.atan2(dy, dx) * 180) / Math.PI;
      if (deg < 0) deg += 360;
      const inRed = deg >= redWindow[0] && deg <= redWindow[1];
      let col = BG.slice();
      // 先画灰轨道（整圈）
      {
        const inner = R_OUT - trackWidth;
        const cov = Math.max(0, Math.min(1, Math.min(r - inner + 0.5, R_OUT - r + 0.5)));
        if (cov > 0) col = [0, 1, 2].map(i => Math.round(col[i] + cov * (TRACK[i] - col[i])));
      }
      // 再画红弧（只在点亮的那一段角度上）
      if (inRed) {
        const inner = R_OUT - redWidth;
        const cov = Math.max(0, Math.min(1, Math.min(r - inner + 0.5, R_OUT - r + 0.5)));
        if (cov > 0) col = [0, 1, 2].map(i => Math.round(col[i] + cov * (RED[i] - col[i])));
      }
      return col;
    }
  };
}

// 角度窗口：亮起段取 150~210°，未点亮的轨道取 350~360° ∪ 0~30°（避开两端端帽）
const WINDOW_LIT = [[150, 210]];
const WINDOW_TRACK = [[350, 360], [0, 30]];
const SYNTH_RED_WINDOW = [140, 220];   // 合成图里红弧盖住的角度（把量测窗口包在里面）

section("① 自检：同一套度量能分开「修复前」和「修复后」的几何");
{
  // 修复前：红弧宽 30、轨道宽 24（两个 part 不同宽）
  const before = synthBand(W_NORMAL, W_RED, SYNTH_RED_WINDOW);
  const bLit = measure(before, "lit", WINDOW_LIT, 168, 212);
  const bTrk = measure(before, "track", WINDOW_TRACK, 168, 212);
  ok(!!bLit && !!bTrk, "修复前的合成图里两条带都量到");
  if (bLit && bTrk) {
    near(bLit.width, W_RED, 1.5, "自检·修复前：红弧宽度 = 30");
    near(bTrk.width, W_NORMAL, 1.5, "自检·修复前：轨道宽度 = 24");
    near(bLit.inner, R_OUT - W_RED, 1.5, "自检·修复前：红弧内沿 ≈ 175");
    near(bTrk.inner, R_OUT - W_NORMAL, 1.5, "自检·修复前：轨道内沿 ≈ 181");
    near(bTrk.inner - bLit.inner, DEFECT_INNER_GAP, 1.5,
      "自检·修复前：**内沿差 ≈ 5.3 px（这就是车主看到的『没居中』）**");
    near(bLit.centre - bTrk.centre, -3, 1.0, "自检·修复前：红弧中线比轨道靠里 3 px");
  }

  // 修复后：两个 part 一起加粗（都 30）
  const after = synthBand(W_RED, W_RED, SYNTH_RED_WINDOW);
  const aLit = measure(after, "lit", WINDOW_LIT, 168, 212);
  const aTrk = measure(after, "track", WINDOW_TRACK, 168, 212);
  ok(!!aLit && !!aTrk, "修复后的合成图里两条带都量到");
  if (aLit && aTrk) {
    near(aLit.width, W_RED, 1.5, "自检·修复后：红弧宽度 = 30");
    near(aTrk.width, W_RED, 1.5, "自检·修复后：轨道宽度也 = 30（一起加粗）");
    near(aLit.inner - aTrk.inner, 0, 1.0, "自检·修复后：内沿对齐（差 0）");
    near(aLit.centre - aTrk.centre, 0, 1.0, "自检·修复后：中线对齐（差 0）");
  }
  // 反向对照：修复前那张图必须**过不了**"中线对齐 ≤1 px"这条
  if (bLit && bTrk) {
    ok(Math.abs(bLit.centre - bTrk.centre) > 1,
      "自检·反向对照：修复前的合成图**过不了**『中线差 ≤ 1 px』（证明本判据不是空转）");
  }
}

// ------------------------------------------------------------
// ②③ 真帧：两层都要量（角度窗口按每屏的点亮进度取，见 FRAMES 表）
// ------------------------------------------------------------
const framesDir = (() => {
  const i = process.argv.indexOf("--frames");
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : path.join(__dirname, "..", "..", "preview", "frames");
})();

const CASES = [
  {
    name: "超速（右屏 · 速度弧）",
    file: "r_0030.bmp",
    litWindows: [[150, 210]],
    trackWindows: [[350, 360], [0, 30]],
    expectRed: true,
  },
  {
    name: "爆转（左屏 · 转速弧）",
    file: "l_0140.bmp",
    // 转速进红区时点亮段已经快到终点 ⇒ 未点亮的轨道只剩末端几个度，
    // 量不到可靠的轨道宽度；这一条只钉"红弧自身的几何"，与 ② 的红弧逐项对照。
    litWindows: [[150, 210]],
    trackWindows: null,
    expectRed: true,
  },
  {
    name: "常态对照（左屏 · 未进红区）",
    file: "l_0100.bmp",
    litWindows: [[150, 210]],
    trackWindows: [[350, 360], [0, 30]],
    expectRed: false,
  },
];

const loaded = [];
for (const c of CASES) {
  const p = path.join(framesDir, c.file);
  if (!fs.existsSync(p)) {
    console.log("\n没找到落帧：" + p);
    console.log("先跑一遍预览把帧落下来（会写 preview/frames）：");
    console.log("  powershell -ExecutionPolicy Bypass -File tools\\build\\pio.ps1 -Cmd 'run -e pcpreview'");
    console.log("  然后跑 C:\\206dash-build\\pcpreview\\program.exe（工作目录 = 仓库根）");
    process.exit(2);
  }
  loaded.push({ c, img: readBmp(p) });
}

const redBands = [];
for (const { c, img } of loaded) {
  section((c.expectRed ? "② " : "③ ") + c.name + "　(" + c.file + ")");
  const lit = measure(img, "lit", c.litWindows, 168, 212);
  ok(!!lit, c.name + "：量到了点亮段（红弧/常态弧）");
  if (!lit) continue;
  console.log("  点亮段：中线 " + fmt(lit.centre) + "、宽 " + fmt(lit.width) +
    "、带 [" + fmt(lit.inner) + "," + fmt(lit.outer) + "]、" + lit.px + " 像素");

  if (c.expectRed) {
    near(lit.width, W_RED, 1.5, c.name + "：红弧宽度 = 1.25 × 24 = 30");
    near(lit.outer, R_OUT + 0.6, 1.5, c.name + "：红弧外沿仍在 205（外沿不许动）");
    redBands.push({ name: c.name, band: lit });
  } else {
    // 常态：红区逻辑**不许**影响没进红区的那一屏
    near(lit.width, W_NORMAL, 1.5, c.name + "：常态弧宽度仍是 24（红区逻辑不碰常态）");
  }

  if (c.trackWindows) {
    const trk = measure(img, "track", c.trackWindows, 168, 212);
    ok(!!trk, c.name + "：量到了灰轨道");
    if (trk) {
      console.log("  灰轨道：中线 " + fmt(trk.centre) + "、宽 " + fmt(trk.width) +
        "、带 [" + fmt(trk.inner) + "," + fmt(trk.outer) + "]、" + trk.px + " 像素");
      near(trk.outer, R_OUT, 1.5, c.name + "：灰轨道外沿 = 205");
      if (c.expectRed) {
        near(trk.width, W_RED, 1.5, c.name + "：红区时灰轨道**也**加粗到 30（两个 part 同步）");
        atMost(Math.abs(lit.inner - trk.inner), 2, c.name + "：红弧内沿与灰轨道内沿对齐（修复前是 " + DEFECT_INNER_GAP + " px）");
        atMost(Math.abs(lit.width - trk.width), 1.5, c.name + "：红弧与灰轨道**同宽**（修复前差 " + DEFECT_WIDTH_GAP + " px）");
        atMost(Math.abs(lit.centre - trk.centre), 1.5, c.name + "：红弧中线与灰轨道中线对齐（修复前差 1.98 px）");
      } else {
        near(trk.width, W_NORMAL, 1.5, c.name + "：常态轨道宽度 24");
        atMost(Math.abs(lit.centre - trk.centre), 1.2, c.name + "：常态下点亮段与轨道本来就同心");
      }
    }
  }
}

// ------------------------------------------------------------
// ④ 两屏的红弧必须是同一套几何（同一条代码路径 ⇒ 转速屏与速度屏逐项相同）
// ------------------------------------------------------------
if (redBands.length === 2) {
  section("④ 两块屏的红弧逐项相同（同一段代码、同一套几何）");
  const [a, b] = redBands;
  near(a.band.inner, b.band.inner, 0.5, "红弧内沿：爆转屏 vs 超速屏");
  near(a.band.outer, b.band.outer, 0.5, "红弧外沿：爆转屏 vs 超速屏");
  near(a.band.centre, b.band.centre, 0.5, "红弧中线：爆转屏 vs 超速屏");
  near(a.band.width, b.band.width, 0.5, "红弧宽度：爆转屏 vs 超速屏");
  near(a.band.width / W_NORMAL, RED_SCALE, 0.1,
    "红弧宽度 = 常态 × " + RED_SCALE + "（加粗量没变，只是两个 part 一起加）");
}

// ------------------------------------------------------------
console.log("\n" + "=".repeat(60));
if (fail === 0) {
  console.log("全部通过:" + pass + " 项判据（红弧与灰轨道同心；像素级，不靠眼睛）");
  console.log("=".repeat(60));
  process.exit(0);
} else {
  console.log("失败 " + fail + " 项（通过 " + pass + " 项）：");
  for (const f of failures) console.log("  · " + f);
  console.log("=".repeat(60));
  process.exit(1);
}
