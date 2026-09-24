/* ============================================================
 * 层序 A/B：把两套落帧（改前 / 改后）在同一批**弧带采样点**上逐点比对。
 *
 * 为什么需要 A/B 而不是"看改后那一帧是什么颜色"：
 *   弧有**轨道**（track，60% 不透明的暗灰）与**点亮段**（不透明彩色）两种墨迹，
 *   而"表情盖住弧"在轨道那一段上的表现是"暗灰被脸色顶掉"—— 单看一帧读不出
 *   "这是弧还是脸"。所以拿**同一份素材、同一批注入值**跑两遍，看同样的像素
 *   从什么变成了什么：**改后应当是弧色**。
 *
 * 用法：
 *   node tools/theme-editor/check-layer-ab.js <before 目录> <after 目录> [帧号...]
 * 退出码：0 = 判据成立。
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
const lum = (c) => 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;
const dist = (a, b) => Math.abs(a.r - b.r) + Math.abs(a.g - b.g) + Math.abs(a.b - b.b);

function main() {
  const [dirB, dirA, ...frameArgs] = process.argv.slice(2);
  if (!dirB || !dirA) {
    console.error("用法: check-layer-ab.js <before目录> <after目录> [帧号...]");
    process.exit(2);
  }
  const frames = frameArgs.length ? frameArgs.map(Number) : [60, 100, 140];

  // ---- 采样点（480 基准；只在**左屏**，它是转速表 + 水温弧）----
  // 主弧带 r=181..205（点亮 #FF5C5C、轨道 #232323@60%），水温弧 r=158..168
  //   （点亮 #7CFF6B、轨道同上）。
  // 角度按几何挑：±45°/±30° 是"方形脸图对角线伸得最远"的方向（脸 320×320 ⇒
  //   半对角 226 > 181 ⇒ **必然**伸进主弧带）；150°/210° 是主弧带里离脸最近的角度。
  const PT = [];
  for (const deg of [45, 30, 60, 135, 150, 210, 225, 315, 330]) {
    for (const r of [190, 200]) PT.push({ deg, r });
  }
  // 读数采样（大数字带中线与单位带）：这几点的"白"就是"读数在最上层"
  for (const p of [{ x: 240, y: 72 }, { x: 210, y: 72 }, { x: 270, y: 75 }, { x: 240, y: 107 }]) {
    PT.push({ x: p.x, y: p.y, label: "readout" });
  }

  let n = 0, changed = 0, arcAfter = 0, whiteAfter = 0, whiteBefore = 0;
  const isArcColor = (c) =>
    (c.r > 180 && c.g < 130 && c.b < 130) ||     // #FF5C5C 点亮
    (c.g > 180 && c.r < 180 && c.b < 170) ||     // #7CFF6B 点亮
    (Math.abs(c.r - c.g) < 26 && Math.abs(c.g - c.b) < 26 && lum(c) > 28 && lum(c) < 110);
  const isWhite = (c) => c.r > 200 && c.g > 200 && c.b > 200;

  for (const f of frames) {
    const name = `l_${String(f).padStart(4, "0")}.bmp`;
    const b = readBmp(path.join(dirB, name));
    const a = readBmp(path.join(dirA, name));
    console.log(`--- frame ${name} ---`);
    for (const p of PT) {
      const x = p.x !== undefined ? p.x : Math.round(240 + p.r * Math.cos(p.deg * Math.PI / 180));
      const y = p.y !== undefined ? p.y : Math.round(240 + p.r * Math.sin(p.deg * Math.PI / 180));
      const cb = b.px(x, y), ca = a.px(x, y);
      ++n;
      const ch = dist(cb, ca) > 24;
      if (ch) ++changed;
      if (p.label === "readout") {
        if (isWhite(cb)) ++whiteBefore;
        if (isWhite(ca)) ++whiteAfter;
      } else if (ch && isArcColor(ca)) ++arcAfter;
      if (ch || p.label === "readout") {
        const tag = p.label === "readout" ? "readout" : `arc r=${p.r} @${p.deg}deg`;
        console.log(`  ${tag.padEnd(18)} (${String(x).padStart(3)},${String(y).padStart(3)})  ` +
                    `before=${hex(cb)} -> after=${hex(ca)}${ch ? "  CHANGED" : ""}`);
      }
    }
  }
  const arcPts = PT.filter((p) => p.label !== "readout").length * frames.length;
  console.log(`\nsampled=${n} changed=${changed}`);
  console.log(`arc points=${arcPts}  "changed AND now arc-coloured"=${arcAfter}`);
  console.log(`readout points: white before=${whiteBefore} after=${whiteAfter} (of ${PT.filter(p=>p.label==="readout").length * frames.length})`);
  const ok = arcAfter >= 6 && whiteAfter >= whiteBefore;
  console.log(`verdict: arcOverFace=${arcAfter >= 6 ? "OK" : "FAIL"} ` +
              `readoutTop=${whiteAfter >= whiteBefore ? "OK" : "FAIL"} => ${ok ? "LAYERS-OK" : "LAYERS-BAD"}`);
  process.exit(ok ? 0 : 2);
}

main();
