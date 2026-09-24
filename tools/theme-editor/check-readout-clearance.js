/* ============================================================
 * 读数压不压弧：从 pcpreview 落的 BMP 上量**主弧带被数字盖住多少像素**。
 *
 * 为什么要有它（2026-09-24 车主："外弧被数字挡住" / 评审："读数下移，别压外弧"）：
 *   层序是"读数在弧之上"（最高层），所以**数字盖住的那一段弧在帧上就是数字色**
 *   —— 于是"压了多少"是一个能直接数出来的像素事实，不必靠几何推算（推算会
 *   漏掉抗锯齿、字宽随位数变化、单位那一行的位置等等）。
 *
 * 口径（480 基准，与 ui_theme.h / check-layer-order.js 同一套）：
 *   ① **主弧带** = 到圆心距离 r ∈ [181, 205] 的那一圈（外半径 205、带宽 24）；
 *   ② **读数墨迹** = 近白像素（读数色 0xFFFFFF；弧的点亮色 0xFF5C5C / 0x39C5FF
 *      与轨道的暗灰都**不是**近白 ⇒ 两者不会互相误判）；
 *   ③ `arcHit` = 同时满足 ①② 的像素数 = **被数字压住的弧像素**；
 *   ④ `inkTop/inkBot` = 读数墨迹在**大数字带**里的上下沿（用来判断"下移了多少"）。
 *   ★ 采样是**逐像素扫全屏**（不是取几个点）：位数变化时墨迹宽度会变，取点会漏。
 *
 * 用法：
 *   node tools/theme-editor/check-readout-clearance.js <bmp> [<bmp2> ...]
 *   node tools/theme-editor/check-readout-clearance.js --ab <before.bmp> <after.bmp>
 * 退出码：0 = 每个文件都算出来了（它**不判 PASS/FAIL**：这一条是"越小越好"的
 *         连续量，前后值才是判据；`--ab` 会额外给出差值）。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");

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
      return { b: b[o], g: b[o + 1], r: b[o + 2] };
    },
  };
}

// 主弧带（480 基准的半径；换档只需改 scale）
const ARC_IN = 181, ARC_OUT = 205;
// 读数墨迹的判据：近白
const isWhite = (c) => c.r >= 200 && c.g >= 200 && c.b >= 200;
// 弧色（点亮段）：红 / 蓝 / 绿几档 —— 只用来报告"弧带里有多少弧"，不参与判据
const isArcLit = (c) =>
  (c.r > 150 && c.g < 140 && c.b < 140) ||
  (c.b > 150 && c.r < 140 && c.g > 130) ||
  (c.g > 150 && c.r < 160 && c.b < 160);

function measure(file) {
  const img = readBmp(file);
  const scale = img.w / 480;
  const cx = img.w / 2, cy = img.h / 2;
  const rIn = ARC_IN * scale, rOut = ARC_OUT * scale;
  // 大数字带：480 基准 y ∈ [30, 135]、|x-240| ≤ 200（避开副表与灯条）
  const bandY0 = Math.round(30 * scale), bandY1 = Math.round(135 * scale);
  const bandX0 = Math.round(40 * scale), bandX1 = Math.round(440 * scale);

  let arcHit = 0;          // 弧带里的**读数墨迹**像素（= 被数字压住的弧）
  let arcLit = 0;          // 弧带里的弧色像素（参考）
  let inkTop = -1, inkBot = -1, inkCount = 0;
  let inkL = -1, inkR = -1;
  for (let y = 0; y < img.h; ++y) {
    for (let x = 0; x < img.w; ++x) {
      const dx = x - cx, dy = y - cy;
      const r = Math.sqrt(dx * dx + dy * dy);
      const c = img.px(x, y);
      if (r >= rIn && r <= rOut) {
        if (isWhite(c)) ++arcHit;
        else if (isArcLit(c)) ++arcLit;
      }
      if (x >= bandX0 && x <= bandX1 && y >= bandY0 && y <= bandY1 && isWhite(c)) {
        ++inkCount;
        if (inkTop < 0 || y < inkTop) inkTop = y;
        if (y > inkBot) inkBot = y;
        if (inkL < 0 || x < inkL) inkL = x;
        if (x > inkR) inkR = x;
      }
    }
  }
  // 弧带内沿在 x=cx 处的 y（= 圆顶那一带的判据线）
  const innerTopY = Math.round(cy - rIn);
  return { file, w: img.w, h: img.h, scale, arcHit, arcLit, inkTop, inkBot, inkL, inkR, inkCount, innerTopY };
}

function show(m) {
  const nm = path.basename(m.file);
  console.log(
    `${nm}: ${m.w}x${m.h} scale=${m.scale} | ` +
    `arcHit(读数墨迹落在弧带)=${m.arcHit} arcLit(弧带里的弧)=${m.arcLit} | ` +
    `digit ink y=[${m.inkTop}..${m.inkBot}] x=[${m.inkL}..${m.inkR}] n=${m.inkCount} | ` +
    `弧带内沿在 x=中心 处 y=${m.innerTopY}`);
}

function main() {
  const args = process.argv.slice(2);
  if (args.length === 0) {
    console.error("用法: check-readout-clearance.js <bmp> [...] | --ab <before.bmp> <after.bmp>");
    process.exit(2);
  }
  if (args[0] === "--ab") {
    if (args.length < 3) { console.error("--ab 需要两个文件"); process.exit(2); }
    const a = measure(args[1]), b = measure(args[2]);
    console.log("== BEFORE =="); show(a);
    console.log("== AFTER  =="); show(b);
    console.log(`== 差: arcHit ${a.arcHit} -> ${b.arcHit} (${b.arcHit - a.arcHit}); ` +
                `digit ink top ${a.inkTop} -> ${b.inkTop} ` +
                `(下移 ${b.inkTop - a.inkTop}px); ` +
                `ink bottom ${a.inkBot} -> ${b.inkBot} ==`);
    process.exit(0);
  }
  for (const f of args) show(measure(f));
  process.exit(0);
}

main();
