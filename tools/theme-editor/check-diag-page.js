/* ============================================================
 * 诊断页的**可读性判据**：从 pcpreview 落的 BMP 上量三个数。
 *
 * 为什么需要它（2026-09-24 车主反馈的原文）：
 *   "屏幕为什么黑了" —— 诊断页打开之后**整屏看起来是黑的**。
 *   历史上这一页被判黑时的数字口径就是"96% 的采样点亮度 < 20"，
 *   而只判"底色深不深"（check-system-status.js 的 diag 那一条）**不够**：
 *   底色深**正是诊断页该有的样子**，它分不出"深色页 + 看得见的字"
 *   与"满屏近黑 + 什么都没画出来"。
 *
 * ★ 本脚本量的正是后者的反面，三个数都是**像素实测**：
 *   ① pageMean     —— 页区域（容器矩形 ∩ 可视圆）的**平均亮度**
 *                      ⇒ "这一页有没有把整屏压成近黑"
 *   ② textRatio    —— 正文框内**文字像素占框内采样点的比例**
 *                      ⇒ "字到底画出来了没有"（修前必须 > 0，否则一片黑）
 *   ③ inkLum       —— 文字像素的平均亮度（文字本身够不够亮）
 *
 * 用法：
 *   node tools/theme-editor/check-diag-page.js <l_XXXX.bmp>
 * 退出码：0 = 判据成立（页是"一页界面"），2 = 不成立。
 *
 * ★ 与 check-system-status.js 的分工（两条都要跑，不互相替代）：
 *   · check-system-status.js diag  —— "诊断页现在到底开没开"（供 sweep 用）
 *   · 本脚本                       —— "开着的那一页**能不能看清**"
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");

// ---- 极简 BMP 读取（24/32 位，自下而上）——与 check-system-status.js 同一份实现 ----
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

function lum(c) { return 0.299 * c.r + 0.587 * c.g + 0.114 * c.b; }

// 屏是**圆的**：算平均亮度时只取可视圆内的点。
// ★ 圆外那圈是 preview 的遮罩标注（被压暗/打点），算进来会把结论带偏。
//   半径取 240（= 480/2，与 panel_view.h 同一口径），减 1 px 免得吃到圈带的边。
function insideCircle(x, y, cx, cy, r) {
  const dx = x - cx, dy = y - cy;
  return dx * dx + dy * dy <= r * r;
}

// ---- 几何（与 src/dash_ui.cpp 的 build_diag 同口径；480 基准）----
//   容器：**内缩**的圆角面板（不再铺满圆屏）
//   ★ 这几个数改了要同步改这里，否则脚本量的是另一块地方。
const PAGE = { x0: 32, y0: 32, x1: 448, y1: 448 };
//   正文框：标题在它上面
const BODY = { x0: 60, y0: 116, x1: 420, y1: 428 };
//   标题带：标题 + 页码都在这一带里
const TITLE = { x0: 32, y0: 32, x1: 448, y1: 116 };

// "文字像素"的门限：正文 0xE8EDF2 亮度 ≈ 235 ⇒ 取 150 只吃字、不吃底色
//   （底色 0x16212E 亮度 ≈ 32、边框琥珀 0xFFB020 亮度 ≈ 184 —— 边框不在正文框里）。
const TEXT_LUM = 150;

function stats(img, box, skipEdges) {
  let n = 0, sum = 0, ink = 0, inkSum = 0, dark = 0;
  const cx = img.w / 2, cy = img.h / 2, r = Math.min(img.w, img.h) / 2 - 1;
  for (let y = box.y0; y <= box.y1; ++y) {
    for (let x = box.x0; x <= box.x1; ++x) {
      if (x < 0 || y < 0 || x >= img.w || y >= img.h) continue;
      // 页区域那一条要按圆裁；正文/标题框在圆内，不必裁（保持一致也无害）
      if (skipEdges && !insideCircle(x, y, cx, cy, r)) continue;
      const l = lum(img.px(x, y));
      ++n; sum += l;
      if (l < 20) ++dark;
      if (l >= TEXT_LUM) { ++ink; inkSum += l; }
    }
  }
  return {
    n,
    mean: n ? sum / n : 0,
    ink,
    inkRatio: n ? ink / n : 0,
    inkLum: ink ? inkSum / ink : 0,
    darkRatio: n ? dark / n : 0,
  };
}

function main() {
  const target = process.argv[2];
  if (!target) {
    console.error("用法: check-diag-page.js <l_XXXX.bmp>");
    process.exit(2);
  }
  const img = readBmp(target);
  const page = stats(img, PAGE, true);
  const body = stats(img, BODY, false);
  const title = stats(img, TITLE, false);
  const base = path.basename(target);

  console.log(`diag-page ${base}: ${img.w}x${img.h}`);
  console.log(`  page  mean=${page.mean.toFixed(1)} dark(<20)=${(page.darkRatio * 100).toFixed(1)}% n=${page.n}`);
  console.log(`  title ink=${title.ink} ratio=${(title.inkRatio * 100).toFixed(2)}% lum=${title.inkLum.toFixed(0)}`);
  console.log(`  body  ink=${body.ink} ratio=${(body.inkRatio * 100).toFixed(2)}% lum=${body.inkLum.toFixed(0)} n=${body.n}`);

  // ---- 判据 ----
  // ① 字必须画出来了：**这一条是关键**（"一片黑、连一个字都没有"就是它红）
  //    正文框里至少要有 300 个亮像素（11 行 × 约 30 字，一行约 30~60 个 ⇒ 余量很大）。
  const textOk = body.ink >= 300;
  // ② 标题带里也要有字（标题 + 页码，**比正文显眼**）
  const titleOk = title.ink >= 100;
  // ③ 页区域不许是"近黑满屏"：平均亮度明显高于"什么都没画"的黑
  //    （0x0A0A0A 满屏 ≈ 10、0x16212E 面板 + 描边 + 字 ≈ 30~40 ⇒ 取 20 分得开）
  const notBlackOk = page.mean >= 20;
  // ④ 页区域里"亮度 < 20"的点不许占绝大多数（历史判据：96% 就是这么来的）
  const darkOk = page.darkRatio <= 0.60;

  const ok = textOk && titleOk && notBlackOk && darkOk;
  console.log(`  verdict: text=${textOk ? "OK" : "FAIL(<300)"} ` +
              `title=${titleOk ? "OK" : "FAIL(<100)"} ` +
              `pageMean=${notBlackOk ? "OK" : "FAIL(<20)"} ` +
              `dark=${darkOk ? "OK" : "FAIL(>60%)"} ` +
              `=> ${ok ? "READABLE" : "NOT-READABLE"}`);
  process.exit(ok ? 0 : 2);
}

main();
