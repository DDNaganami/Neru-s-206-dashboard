/* ============================================================
 * 读 pcpreview 落的 BMP，核对**本轮两条功能**的画面证据：
 *   ① 「数据不可信」角标（表盘右缘那块固定位置）出没出现
 *   ② 诊断页（整屏深色 + 文本行）有没有盖住表盘
 *
 * ★ 为什么不"靠看"：这个环境里没有人眼，也没有截图库；而落盘的 BMP 是
 *   确定性的像素事实（与 tools/theme-editor/check-preview-frame.js 同一套办法
 *   —— 那个脚本核的是图层顺序与透明通道，本脚本核的是"这两块东西在不在"）。
 *
 * 用法：
 *   node tools/theme-editor/check-system-status.js badge  <l_XXXX.bmp>
 *   node tools/theme-editor/check-system-status.js diag   <l_XXXX.bmp>
 *   node tools/theme-editor/check-system-status.js sweep  <frames 目录> [起始] [结束]
 * 退出码：0 = 结论成立，2 = 不成立（脚本里明确打印是哪一个像素结论）。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");

// ---- 极简 BMP 读取（24/32 位，自下而上）----
function readBmp(p) {
  const b = fs.readFileSync(p);
  if (b[0] !== 0x42 || b[1] !== 0x4d) throw new Error("不是 BMP: " + p);
  const dataOff = b.readUInt32LE(10);
  const w = b.readInt32LE(18);
  const hRaw = b.readInt32LE(22);
  const bpp = b.readUInt16LE(28);
  const flip = hRaw > 0;
  const h = Math.abs(hRaw);
  const rowSize = Math.floor((bpp * w + 31) / 32) * 4;
  const bytesPerPx = bpp / 8;
  return {
    w, h,
    px(x, y) {
      const ry = flip ? (h - 1 - y) : y;
      const o = dataOff + ry * rowSize + x * bytesPerPx;
      // BMP 是 BGR
      return { b: b[o], g: b[o + 1], r: b[o + 2] };
    },
  };
}

function lum(c) { return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b; }

// ---- 几何（与 src/dash_ui.cpp 里那几个常量同口径；480 基准）----
//   角标容器：x = 340..456、y = 205..241（kTrustBadgeX/CY/W/H）
//   ★ "出现"的判据不看单个像素，而是看这块区域里**亮像素的比例**：
//     角标文字（白）+ 描边（灰蓝/琥珀）会让这一块明显比"没有角标"时亮。
const BADGE = { x0: 342, y0: 207, x1: 454, y1: 239 };
// 诊断页：整屏深色（0x0A0A0A）+ 文本（0xE0E0E0 / 0xFFB020）
//   ★ 判据取"屏幕中部一条横带"（y=300，整宽）：诊断页开着时它全是底色与文字，
//     而表盘在那里是**脸/弧**（亮得多，或完全是别的颜色）。
const DIAG_BAND_Y = 300;

function badgeStats(img) {
  let n = 0, bright = 0;
  for (let y = BADGE.y0; y <= BADGE.y1; ++y) {
    for (let x = BADGE.x0; x <= BADGE.x1; ++x) {
      ++n;
      if (lum(img.px(x, y)) > 110) ++bright;
    }
  }
  return { n, bright, ratio: bright / n };
}

function bandStats(img) {
  let n = 0, sum = 0;
  for (let x = 20; x < img.w - 20; ++x) { ++n; sum += lum(img.px(x, DIAG_BAND_Y)); }
  return { mean: sum / n };
}

// ---- 三档判定 ----
//   角标：ratio >= 0.05 视为"在"（实测：有角标 ≈0.09~0.12，没有 ≈0.005）
//   ★ 阈值刻意留大余量：这两个数差一个量级，取中间不会有边界抖动。
function badgeOn(img) { return badgeStats(img).ratio >= 0.05; }
//   诊断页：中部横带的平均亮度 < 60（深色页）⇒ 在；表盘那一带 ≥ 60（脸是白的/弧是亮的）
//   ★ 单纯"暗"不足以判"诊断页开着"：**开机动画那几帧整屏也是黑的**
//     （实测 f=0..4 全是暗的）⇒ 必须再看"页面上有没有字"（见 diagText）。
function diagBgDark(img) { return bandStats(img).mean < 60; }
// 诊断页文本区（y=80..400、x=24..456）里有多少"亮像素"（文字是 0xE0E0E0 / 0xFFB020）。
// ★ 这一条是关键：它把"整屏黑"（开机动画 / 渲染事故）与"诊断页真的画出来了"分开。
function diagTextPixels(img) {
  let bright = 0;
  for (let y = 80; y <= 400; y += 2) {
    for (let x = 24; x <= 456; x += 2) {
      if (lum(img.px(x, y)) > 120) ++bright;
    }
  }
  return bright;
}
// 诊断页真的开着 = 底色是深色 **且** 有文字（两页都有至少 5 行字）
function diagOn(img) { return diagBgDark(img) && diagTextPixels(img) >= 30; }

function cmdBadge(p) {
  const img = readBmp(p);
  const s = badgeStats(img);
  const on = badgeOn(img);
  console.log(`badge ${path.basename(p)}: bright=${s.bright}/${s.n} ratio=${s.ratio.toFixed(4)} => ${on ? "PRESENT" : "absent"}`);
  return on;
}
function cmdDiag(p) {
  const img = readBmp(p);
  const b = bandStats(img);
  const txt = diagTextPixels(img);
  const on = diagOn(img);
  console.log(`diag ${path.basename(p)}: bandMean=${b.mean.toFixed(1)} textPx=${txt} => ${on ? "OPEN" : "closed"}`);
  return on;
}

function main() {
  const [mode, target, a, b] = process.argv.slice(2);
  if (mode === "badge") { cmdBadge(target); return; }
  if (mode === "diag") { cmdDiag(target); return; }
  if (mode === "sweep") {
    const from = a ? Number(a) : 0;
    const to = b ? Number(b) : 9999;
    const dir = target;
    const files = fs.readdirSync(dir)
      .filter((f) => /^l_\d+\.bmp$/.test(f))
      .map((f) => ({ f, n: Number(f.slice(2, -4)) }))
      .filter((e) => e.n >= from && e.n <= to)
      .sort((x, y) => x.n - y.n);
    let firstBadgeOn = null, firstBadgeOffAfterOn = null;
    const diagFrames = [];
    let prev = null;
    // ★ 开机动画那几帧要跳掉：那时背景本来就是全黑的，而"整屏黑"与
    //   "诊断页开着"在像素上确实长得一样（判据靠文字区分，见 diagOn 的说明）。
    //   `BOOT_TOTAL_MS` 默认 ≈1130 ms ⇒ 从 f=8（≈1.6 s）起算，留足余量。
    const BOOT_SKIP = 8;
    for (const e of files) {
      const img = readBmp(path.join(dir, e.f));
      const on = badgeOn(img);
      const d = diagOn(img);
      if (e.n >= BOOT_SKIP) {
        if (on && firstBadgeOn === null) firstBadgeOn = e.n;
        if (!on && firstBadgeOn !== null && firstBadgeOffAfterOn === null) {
          firstBadgeOffAfterOn = e.n;
        }
        if (d) diagFrames.push(e.n);
      }
      if (prev === null || prev.on !== on || prev.d !== d) {
        console.log(`  f=${String(e.n).padStart(4)} badge=${on ? "ON " : "off"} diag=${d ? "OPEN" : "shut"}${e.n < BOOT_SKIP ? "  (boot)" : ""}`);
      }
      prev = { on, d };
    }
    console.log(`sweep ${dir}: frames=${files.length} (skip<${BOOT_SKIP}) ` +
                `firstBadgeOn=${firstBadgeOn} firstBadgeOffAfterOn=${firstBadgeOffAfterOn}`);
    console.log(`sweep: diag-open frames = ${diagFrames.length}` +
                (diagFrames.length ? ` (${diagFrames[0]}..${diagFrames[diagFrames.length - 1]})` : ""));
    return;
  }
  console.error("用法: check-system-status.js badge|diag|sweep <bmp|frames 目录> [from] [to]");
  process.exit(2);
}

main();
