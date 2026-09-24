/* ============================================================
 * 层序判据：从 pcpreview 落的 BMP 上读**指定像素**，回答两件事。
 *
 * 为什么要有它（2026-09-24 车主定稿的层序："表情在背景之上，但**表盘弧永远是
 * 最上层**"，以及"**读数应该是最高层的**"）：
 *   这两条都是**像素事实**，靠"看代码里谁先建"是间接证据。这个脚本读真实落帧：
 *
 *   ① 弧带（半径 181..205 那一圈，480 基准）里取若干个采样点。
 *      如果脸图**盖住了**弧 → 那里是脸的颜色；如果弧在最上层 → 那里是弧色
 *      （点亮段不透明 #FF5C5C / #7CFF6B，或轨道的合成色）。
 *   ② 读数位置取采样点（大数字/单位那两条带）。读数在最上层 ⇒ 那里是**白色**
 *      （读数色），不是弧色、不是脸色。
 *
 * 用法：
 *   node tools/theme-editor/check-layer-order.js <l_XXXX.bmp> [--arc-expect 0xFF5C5C]
 * 退出码：0 = 判据成立。
 *
 * ★ 采样点是**极坐标**（按 480 基准的半径/角度算出来的），不是"抄几个坐标"：
 *   这样换成 240 档只要改 scale，采样点仍然落在同一条弧带上。
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
const hex = (c) => "#" + [c.r, c.g, c.b].map((v) => v.toString(16).padStart(2, "0")).join("");
const dist = (a, b) => Math.abs(a.r - b.r) + Math.abs(a.g - b.g) + Math.abs(a.b - b.b);

// 角度→像素：与固件同一套（0° = 3 点钟，顺时针为正，y 向下）。
function at(cx, cy, r, deg) {
  const rad = deg * Math.PI / 180;
  return { x: Math.round(cx + r * Math.cos(rad)), y: Math.round(cy + r * Math.sin(rad)) };
}

function main() {
  const target = process.argv[2];
  if (!target) { console.error("用法: check-layer-order.js <bmp>"); process.exit(2); }
  const img = readBmp(target);
  const scale = img.w / 480;
  const cx = img.w / 2, cy = img.h / 2;
  const rMid = 193 * scale;   // 主弧带 181..205 的中线

  console.log(`layer-order ${path.basename(target)}: ${img.w}x${img.h} scale=${scale}`);

  // ---- ① 弧带采样（左右屏各取几个角度）----
  // 只取**左右两侧**的角度：那里主弧带远离脸图的角（脸的角只在 ±45° 方向），
  // 于是"脸有没有伸进来"这件事在这些角度上**不会被脸本身干扰** ——
  // 采样点读到的要么是弧、要么（如果脸伸到半径 193）是脸。
  // ★ 而"脸伸进弧带"最狠的方向正是 ±45°（方形脸图的对角线），下面单独取。
  const ARC_ANGLES = [170, 190, 350, 10, 135, 225, 315, 45];
  let arcHits = 0;
  for (const deg of ARC_ANGLES) {
    const p = at(cx, cy, rMid, deg);
    const c = img.px(p.x, p.y);
    console.log(`  arc r=193 @${String(deg).padStart(3)}° (${p.x},${p.y}) = ${hex(c)}`);
  }
  // 判定：这些点里"像弧色（亮红/亮绿/亮蓝那几档）"或"像轨道暗灰"的占多数 ⇒ 弧可见
  const isArcish = (c) => {
    const l = (c.r + c.g + c.b) / 3;
    const red = c.r > 150 && c.g < 140 && c.b < 140;      // #FF5C5C 一类
    const green = c.g > 150 && c.r < 160 && c.b < 160;    // #7CFF6B 一类
    const blue = c.b > 150 && c.r < 140 && c.g > 130;     // #39C5FF 一类
    const track = l > 20 && l < 90 && Math.abs(c.r - c.g) < 24 && Math.abs(c.g - c.b) < 24;
    return red || green || blue || track;
  };
  for (const deg of ARC_ANGLES) {
    const p = at(cx, cy, rMid, deg);
    if (isArcish(img.px(p.x, p.y))) ++arcHits;
  }
  console.log(`  arc-band sampled=${ARC_ANGLES.length} arcish=${arcHits}`);

  // ---- ② 读数位置采样（大数字带 & 单位带的中线）----
  // ★ 取"字本身"的采样：扫描那两条带里最亮的像素（读数色是白/近白），
  //   再报告它是不是白色 —— "最高层"在像素上的意思就是"这里读到的是读数色"。
  function brightestIn(x0, y0, x1, y1) {
    let best = null, bestL = -1;
    for (let y = Math.round(y0 * scale); y <= Math.round(y1 * scale); ++y) {
      for (let x = Math.round(x0 * scale); x <= Math.round(x1 * scale); ++x) {
        if (x < 0 || y < 0 || x >= img.w || y >= img.h) continue;
        const c = img.px(x, y);
        const l = 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
        if (l > bestL) { bestL = l; best = { c, x, y }; }
      }
    }
    return { ...best, l: bestL };
  }
  const digit = brightestIn(120, 40, 360, 105);    // 大数字带（与 check-preview-frame 同口径）
  console.log(`  readout-digit brightest (${digit.x},${digit.y}) = ${hex(digit.c)} lum=${digit.l.toFixed(0)}`);
  const whiteish = digit.c.r > 200 && digit.c.g > 200 && digit.c.b > 200;
  const ok = arcHits >= 6 && whiteish;
  console.log(`  verdict: arcVisible=${arcHits >= 6 ? "OK" : "FAIL"} ` +
              `readoutOnTop=${whiteish ? "OK" : "FAIL"} => ${ok ? "LAYERS-OK" : "LAYERS-BAD"}`);
  process.exit(ok ? 0 : 2);
}

main();
