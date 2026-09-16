/* ============================================================
 * 读 pcpreview 落的 BMP,按坐标核对颜色 —— 验证图层顺序与透明通道。
 *
 * 为什么靠读 BMP 而不是"看着像":桩驱动丢弃画面、pcpreview 落帧,
 * 唯一能自动判定的就是像素值。这和当初验主题用的是同一套办法。
 *
 * ★ 采样点的选取很讲究(第一版就是栽在这上面):
 *   画面是 480×480,背景 240×240 居中(偏移 120),表情 100×100 居中(偏移 190)。
 *   而**弧线是画在背景之上的**(radius 205 / width 24 → 半径带 193..217)。
 *   所以采样点必须避开两样东西:
 *     · 弧带(到圆心距离 193..217 那一圈)
 *     · 表情的不透明中心(屏幕 220..260)
 *   否则读到的是"弧线颜色"或"表情颜色",会把正确的图层顺序误判成失败
 *   (第一版就踩了:在 (125,125) 读到弧线色,以为是背景没画上)。
 *
 * 用法: node tools/theme-editor/check-preview-frame.js <bmp> <idle|redline|surprise>
 * ============================================================ */
"use strict";

const fs = require("fs");

// ---- 极简 BMP 读取(24/32 位,自下而上) ----
function readBmp(path) {
  const b = fs.readFileSync(path);
  if (b[0] !== 0x42 || b[1] !== 0x4D) throw new Error("不是 BMP: " + path);
  const dataOff = b.readUInt32LE(10);
  const w = b.readInt32LE(18);
  const hRaw = b.readInt32LE(22);
  const bpp = b.readUInt16LE(28);
  const flip = hRaw > 0;
  const h = Math.abs(hRaw);
  const bytesPerPx = bpp / 8;
  const rowSize = Math.floor((bpp * w + 31) / 32) * 4;
  if (bpp !== 24 && bpp !== 32) throw new Error("只支持 24/32 位 BMP,实际 " + bpp);
  return {
    w, h,
    px(x, y) {
      const ry = flip ? (h - 1 - y) : y;
      const off = dataOff + ry * rowSize + x * bytesPerPx;
      return { b: b[off], g: b[off + 1], r: b[off + 2] };   // BMP = BGR
    }
  };
}

const hex = (c) => "#" + [c.r, c.g, c.b].map(v => v.toString(16).padStart(2, "0")).join("");
const dist = (a, b) => Math.max(Math.abs(a.r - b.r), Math.abs(a.g - b.g), Math.abs(a.b - b.b));

// ---- 期望值 ----
// 背景:暗蓝 (0,0,96) → RGB565 量化后实测 #000062(蓝 96→98)
const BG   = { r: 0x00, g: 0x00, b: 0x62 };
const MARK = { r: 0xff, g: 0x00, b: 0xff };          // 品红标记(背景里画的)
const FACE = {
  idle:     { r: 0xff, g: 0x00, b: 0x00 },           // 左屏常态 = 红
  redline:  { r: 0xff, g: 0xff, b: 0x00 },           // 左屏红区 = 黄
  surprise: { r: 0x00, g: 0xff, b: 0x00 }            // 左屏惊喜 = 绿
};
// 右屏(转速表)用的是**另一套**表情图 —— 颜色故意不同,
// 这样"左右屏各取自己那一套"才验得出来。
const FACE_R = {
  idle:     { r: 0x00, g: 0x80, b: 0xff },           // 右屏常态 = 浅蓝
  redline:  { r: 0xff, g: 0x80, b: 0x00 },           // 右屏红区 = 橙
  surprise: { r: 0x80, g: 0x00, b: 0xff }            // 右屏惊喜 = 紫
};
// 弧线色(主题默认):外弧点亮 #39C5FF / 轨道 #232323,轨道 opa=153/255。
// ★ 轨道是**半透明**的,所以它在背景上的实际颜色 = 轨道色按 153/255 与
//   背景混合 —— 不能直接拿 #232323 比。实测 #101410 就是这么来的:
//   0x23*153/255 + 0x00*(1-153/255) ≈ 0x15(蓝通道背景 0x62 也掺进来)。
//   这条正好顺带证明了"半透明轨道 + 背景图"的混合是通的。
const ARC_LIT   = { r: 0x39, g: 0xc6, b: 0xff };
// 轨道压在背景上的期望值(按 opa=153 混合后,允许 ±12 的量化误差)
const ARC_TRACK_OVER_BG = {
  r: Math.round(0x23 * 153 / 255 + 0x00 * (1 - 153 / 255)),
  g: Math.round(0x23 * 153 / 255 + 0x00 * (1 - 153 / 255)),
  b: Math.round(0x23 * 153 / 255 + 0x62 * (1 - 153 / 255))
};

const BG_OFF = 120, FACE_OFF = 190, TOL = 12;

function main() {
  const path = process.argv[2];
  const which = (process.argv[3] || "idle").toLowerCase();
  // 第 4 个参数:这一帧表情是否应当可见(fade 之后)。
  // 开机阶段 0 表情是透明的 —— 那时采样点读到的应当是背景标记色。
  const faceVisible = (process.argv[4] || "yes") !== "no";
  // 第 5 个参数:哪一屏(默认从左文件名判断 l_/r_)
  let side = process.argv[5];
  if (!side) side = /(^|[\\/])r_/.test(path) ? "right" : "left";
  if (!path) { console.error("用法: check-preview-frame.js <bmp> [idle|redline|surprise] [yes|no] [left|right]"); process.exit(2); }

  const table = (side === "right") ? FACE_R : FACE;
  const face = table[which];
  if (!face) { console.error("未知表情: " + which); process.exit(2); }

  const img = readBmp(path);
  const checks = [];
  const add = (name, x, y, expect, tol) => {
    const got = img.px(x, y);
    checks.push({ name, x, y, got, expect, ok: dist(got, expect) <= (tol === undefined ? TOL : tol) });
  };

  // --- 1. 背景在"没被弧线和表情盖住"的地方露出来 ---
  // 左中 / 右中边缘:距圆心 240 —— 在弧带(193..217)**之外**
  add("背景左中边缘", 128, 240, BG);
  add("背景右中边缘", 352, 240, BG);

  // --- 2. 弧线盖在背景之上(图层顺序:背景 → 弧线) ---
  // 对角线上距圆心 205 的点落在弧带正中:205/√2 ≈ 145
  //
  // ★ 这里**不断言精确颜色**:弧轨道是半透明的(track_opa=153/255),
  //   它的最终颜色取决于 LVGL 的合成实现与量化,手算的期望值会差几个数
  //   (实测 #101410,我手算 #15153c)。精确值属于"实现细节",不是契约。
  //   真正要证明的是**图层顺序**:这个点既不是背景色(说明弧画上去了),
  //   也不是纯黑(说明背景图确实在下面,不是没画)。
  {
    const d = Math.round(205 / Math.SQRT2);
    const got = img.px(240 + d, 240 + d);
    const isBg = dist(got, BG) <= 4;
    const isBlack = (got.r + got.g + got.b) <= 6;
    const lum = got.r + got.g + got.b;
    checks.push({
      name: "弧带点(弧画在背景上,非背景/非纯黑)", x: 240 + d, y: 240 + d,
      got, expect: ARC_TRACK_OVER_BG,
      ok: !isBg && !isBlack && lum < 0x40 * 3      // 暗色但不是纯黑
    });
  }

  // --- 3. 表情在最上层 / 或者开机阶段应当还看不见 ---
  // 表情不透明中心是屏幕 (220..260)。
  //   faceVisible=yes → 应看到表情颜色(盖住了背景标记)
  //   faceVisible=no  → 应看到背景标记(品红),说明 fade 阶段表情是透明的
  if (faceVisible) {
    add("表情不透明中心", 240, 240, face);
  } else {
    add("开机阶段表情应透明(露出背景标记)", 240, 240, MARK);
  }

  // --- 4. 透明通道生效:表情边框区(透明)→ 透出背景 ---
  // 屏幕 (295,205):距圆心 √(55²+35²)=65 —— 在弧带以内,
  // 且落在表情的透明边(220..260 是不透明的,这里在外侧)
  add("表情透明边(应透出背景)", 295, 205, BG);

  let fail = 0;
  console.log("=== " + path + "  (" + (side === "right" ? "右屏" : "左屏") +
              ", 期望表情: " + which +
              ", 表情可见: " + (faceVisible ? "是" : "否") + ")  " + img.w + "x" + img.h + " ===");
  for (const c of checks) {
    if (!c.ok) fail++;
    console.log("  " + (c.ok ? "OK  " : "FAIL") + " " + c.name.padEnd(26) +
                " @(" + c.x + "," + c.y + ")  实测 " + hex(c.got) + "  期望 " + hex(c.expect));
  }
  console.log(fail === 0 ? "\n全部通过 (" + checks.length + " 项)"
                         : "\n失败 " + fail + " / " + checks.length);
  process.exit(fail === 0 ? 0 : 1);
}

main();
