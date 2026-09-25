/* ============================================================
 * 开机**角色标签**的像素判据（2026-09-25 新增）—— "主片 / 副片"区分那件事的证据。
 *
 * 为什么必须有这个脚本（为什么不能"看一眼截图就行"）：
 *   车主的需求是「两块一模一样的板，烧的固件得能区分主/副」。屏上那行
 *   `MASTER (RIGHT)` / `SLAVE (LEFT)` 落在**开机动画那一秒多**里，而这一条有
 *   两个方向都会出错、且都**没有任何编译期信号**：
 *     ① 标签**没画出来**（字号/颜色/坐标写错、或者写成了中文 —— 本构建只使能
 *        Montserrat，中文是**一个字形都画不出来**）⇒ 屏上什么都没有；
 *     ② 标签**常驻**（忘了在动画结束时收掉）⇒ 表盘上永远挂着一行字（车主点名
 *        "常驻就是 bug"）。
 *   ⇒ 所以判据要**两个方向都量**，而且都要落在**像素**上。
 *
 * 用法（两次跑 pcpreview，各取一帧）：
 *   node tools/theme-editor/check-role-label.js <boot.bmp> <steady.bmp>
 *     boot.bmp   = 落帧时"开机窗口还开着"的那一帧（`hold=1` 或第一帧）
 *     steady.bmp = 落帧时"开机窗口已经关了"的那一帧（`hold=0` 之后）
 * 退出码：0 = 判据成立（开机有标签、之后没有），2 = 不成立。
 *
 * ★ 量什么：**标签带**（y = 56..115，见 src/dash_ui.cpp 的 build_role_label）
 *   里"琥珀色文字像素"的个数。取琥珀是因为它是标签**独有**的颜色
 *   （0xFFB020；扫表弧是主题色、背景是深色、表情图是素材）——
 *   判据因此不依赖"那一帧的动画走到哪儿了"。
 *   ★ 只数**圆内**的点（屏是圆的；圆外那圈是预览遮罩的标注，不是画面）。
 * ============================================================ */
"use strict";

const fs = require("fs");

// ---- 极简 BMP 读取（24/32 位，自下而上）——与 check-diag-page.js 同一份实现 ----
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

const lum = (c) => 0.299 * c.r + 0.587 * c.g + 0.114 * c.b;

// 标签带：480 基准下 `build_role_label()` 把标签放在 y = ts(56)，24 号字两行
// ⇒ 大约 y = 56..112。这里留一点余量（到 116），并且**不**碰 181..205 那条弧带
// （扫表弧的颜色会随主题变，把它算进来这条判据就不独立了）。
const BAND = { y0: 50, y1: 118 };
// ★ 取整行宽度（0..479）而不是"标签那点宽度"：字号/文案改动都不用改脚本。
const ROW = { x0: 0, x1: 479 };

function inside(px, py, cx, cy, r) {
  const dx = px - cx, dy = py - cy;
  return dx * dx + dy * dy <= r * r;
}

// "琥珀文字像素"：标签用的是 **0xFFB020**（红 255 / 绿 176 / 蓝 32）。
// ★ 判据必须是"这一族暖色"，但**不能**宽到把别的暖色也吃进来 —— 第一版就是那么栽的：
//   左屏（转速表）的**红**读数/弧是 (255,93,90)，它也满足"红高、蓝低、红-蓝≥70"
//   ⇒ 稳态那一帧数出 5508 个"琥珀"像素，判据当场假红（而右屏是对的）。
//   ⇒ 加一条"**绿必须明显高**（G ≥ 130）"把红挡在外面；再要求蓝更低（B ≤ 90）
//     把白色文字（255,255,255）挡在外面。
//   ★ 抗锯齿：文字的边缘像素是"琥珀 → 底色"的中间色，会偏暗 ⇒ 允许 r ≥ 140 但要
//     **保持色相**（绿/红 ≈ 0.62~0.78）—— 这一条比"亮度门限"稳（底色深浅换了也成立）。
function isAmber(c) {
  if (c.g < 120 || c.b > 100) return false;          // 绿太低 = 红；蓝太高 = 白/浅蓝
  if (c.r < 130) return false;                       // 太暗 = 底色
  if ((c.r - c.b) < 60) return false;                // 暖色相
  const ratio = c.g / c.r;                           // 0xFFB020 ⇒ 0.69
  return ratio >= 0.55 && ratio <= 0.85;
}

function amberCount(img) {
  const cx = img.w / 2, cy = img.h / 2, r = Math.min(img.w, img.h) / 2 - 1;
  let n = 0, amber = 0;
  for (let y = BAND.y0; y <= BAND.y1; ++y) {
    for (let x = ROW.x0; x <= ROW.x1; ++x) {
      if (x >= img.w || y >= img.h) continue;
      if (!inside(x, y, cx, cy, r)) continue;
      ++n;
      const c = img.px(x, y);
      if (isAmber(c)) ++amber;
    }
  }
  return { n, amber };
}

// 顺带报一个"这一带有多亮"（给人看的旁证：标签在的时候明显更亮）
function bandStats(img) {
  const cx = img.w / 2, cy = img.h / 2, r = Math.min(img.w, img.h) / 2 - 1;
  let n = 0, sum = 0, bright = 0;
  for (let y = BAND.y0; y <= BAND.y1; ++y) {
    for (let x = ROW.x0; x <= ROW.x1; ++x) {
      if (x >= img.w || y >= img.h) continue;
      if (!inside(x, y, cx, cy, r)) continue;
      const l = lum(img.px(x, y));
      ++n; sum += l;
      if (l >= 90) ++bright;
    }
  }
  return { n, mean: n ? sum / n : 0, bright };
}

function main() {
  const [bootPath, steadyPath] = process.argv.slice(2);
  if (!bootPath || !steadyPath) {
    console.error("用法: check-role-label.js <boot.bmp> <steady.bmp>");
    process.exit(2);
  }
  const boot = readBmp(bootPath);
  const steady = readBmp(steadyPath);
  const a = amberCount(boot);
  const b = amberCount(steady);
  const sa = bandStats(boot);
  const sb = bandStats(steady);
  const base = (p) => require("path").basename(p);

  console.log(`role-label boot   ${base(bootPath)}: ${boot.w}x${boot.h} band y=${BAND.y0}..${BAND.y1}`);
  console.log(`  amber=${a.amber} (n=${a.n})  mean=${sa.mean.toFixed(1)} bright(>=90)=${sa.bright}`);
  console.log(`role-label steady ${base(steadyPath)}: ${steady.w}x${steady.h}`);
  console.log(`  amber=${b.amber} (n=${b.n})  mean=${sb.mean.toFixed(1)} bright(>=90)=${sb.bright}`);

  // ---- 判据 ----
  //   ① 开机那一帧：**必须有**琥珀文字像素（一行 "MASTER (RIGHT)" 24 号字 ≈ 200+ px）
  const shownOk = a.amber >= 100;
  //   ② 之后那一帧：**必须没有**（留 20 px 的余量给 JPEG 无关的量化噪声/主题弧色误判）
  const goneOk = b.amber <= 20;
  //   ③ 两帧的差异要**显著**（"有 → 无"这条变化本身要看得出来）
  const dropOk = a.amber >= b.amber + 80;

  const ok = shownOk && goneOk && dropOk;
  console.log(`  verdict: shown=${shownOk ? "OK" : "FAIL(<100)"} ` +
              `gone=${goneOk ? "OK" : "FAIL(>20 常驻?)"} ` +
              `drop=${dropOk ? "OK" : "FAIL"} => ${ok ? "LABEL-OK" : "LABEL-BAD"}`);
  process.exit(ok ? 0 : 2);
}

main();
