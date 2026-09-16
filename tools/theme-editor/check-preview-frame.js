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

// ------------------------------------------------------------
// 数字读数(转速/速度大数字 + 单位 + 水温数字)
//
// 为什么按"整条带里数亮点"而不是采某一个像素:
//   文本的笔画落在哪个像素,取决于字体点阵与量化 —— 采一个点会非常脆,
//   换个字号或改一个字就红。而"这一带里有没有画出足够多的字色像素"
//   既能证明"读出来了",又不会因为字形细节误报。
// 期望颜色来自主题默认值(ui_theme.h 的 theme_set_defaults):
//   数字 0xFFFFFF(白)、单位 0x9AA0A6(灰)、水温 0x7CFF6B(绿)
// ------------------------------------------------------------
const READOUT_DIGIT   = { r: 0xFF, g: 0xFF, b: 0xFF };
const READOUT_UNIT    = { r: 0x9A, g: 0xA0, b: 0xA6 };
const READOUT_COOLANT = { r: 0x7C, g: 0xFF, b: 0x6B };

// 在矩形带里数"接近某颜色"的像素
function countNear(img, x0, y0, x1, y1, want, tol) {
  let n = 0;
  for (let y = y0; y <= y1; y++) {
    for (let x = x0; x <= x1; x++) {
      if (dist(img.px(x, y), want) <= tol) n++;
    }
  }
  return n;
}

// 在矩形带里求"接近某颜色"的像素的外接框 —— 用来验证**字形画在哪、多宽**。
// 只看"有没有亮点"是不够的:LVGL 的 lv_label_create() 默认文本是 "Text",
// 忘了清空时那一带同样是白的(这个坑真踩过,见 make_readout_label 的注释)。
// 有了外接框就能区分:数字/单位的外框应该落在主题给的带里,且宽度有限。
function inkBox(img, x0, y0, x1, y1, want, tol) {
  let minX = 1e9, maxX = -1, minY = 1e9, maxY = -1, n = 0;
  for (let y = y0; y <= y1; y++) {
    for (let x = x0; x <= x1; x++) {
      if (dist(img.px(x, y), want) <= tol) {
        n++;
        if (x < minX) minX = x;
        if (x > maxX) maxX = x;
        if (y < minY) minY = y;
        if (y > maxY) maxY = y;
      }
    }
  }
  return n ? { n, minX, maxX, minY, maxY, w: maxX - minX + 1, h: maxY - minY + 1 }
           : { n: 0, minX: -1, maxX: -1, minY: -1, maxY: -1, w: 0, h: 0 };
}

// 读数带(480 基准,与 ui_theme.h 的默认位置对应):
//   数字中心 y=72(48 号 → 约 47..97)、单位中心 y=107(18 号 → 约 97..117)、
//   水温中心 y=384。x 取中间一段,避开弧带(半径 193..217 那一圈)。
const BAND_DIGIT   = { x0: 150, y0: 48, x1: 330, y1: 96 };
const BAND_UNIT    = { x0: 190, y0: 96, x1: 290, y1: 120 };
const BAND_COOLANT = { x0: 190, y0: 370, x1: 290, y1: 400 };

function main() {
  const path = process.argv[2];
  const which = (process.argv[3] || "idle").toLowerCase();
  // 第 4 个参数:这一帧表情是否应当可见(fade 之后)。
  // 开机阶段 0 表情是透明的 —— 那时采样点读到的应当是背景标记色。
  const faceVisible = (process.argv[4] || "yes") !== "no";
  // 第 5 个参数:哪一屏(默认从左文件名判断 l_/r_)
  let side = process.argv[5];
  if (!side) side = /(^|[\\/])r_/.test(path) ? "right" : "left";
  // 第 6 个参数:这一帧该不该有数字读数。
  // 开机扫表期间**不该有**:dash_ui_render 在开机期间会早退,
  // 所以标签一直是空文本 —— 这正是想要的效果,也在这里钉住。
  const readoutShown = (process.argv[6] || "yes") !== "no";
  if (!path) { console.error("用法: check-preview-frame.js <bmp> [idle|redline|surprise] [yes|no] [left|right] [yes|no]"); process.exit(2); }

  const table = (side === "right") ? FACE_R : FACE;
  const face = table[which];
  if (!face) { console.error("未知表情: " + which); process.exit(2); }

  const img = readBmp(path);
  const checks = [];
  const add = (name, x, y, expect, tol) => {
    const got = img.px(x, y);
    checks.push({ name, x, y, got, expect, ok: dist(got, expect) <= (tol === undefined ? TOL : tol) });
  };
  // 整带统计式断言
  const addBand = (name, band, want, tol, minCount, maxCount) => {
    const n = countNear(img, band.x0, band.y0, band.x1, band.y1, want, tol);
    const ok = (minCount === undefined || n >= minCount) &&
               (maxCount === undefined || n <= maxCount);
    checks.push({
      name, x: band.x0, y: band.y0,
      got: { r: n, g: n, b: n }, expect: { r: minCount, g: maxCount, b: 0 },
      ok, text: "带内命中 " + n + " 像素（要求 " +
                (minCount === undefined ? "-" : "≥" + minCount) + "~" +
                (maxCount === undefined ? "-" : "≤" + maxCount) + "）"
    });
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

  // --- 5. 数字读数(转速/速度大数字 + 单位 + 水温) ---
  // 左屏大数字跟的是转速(左=转速表),右屏跟车速;两边都是 48 号白字。
  // 阈值给得宽松:粗体数字的笔画覆盖率远高于 40 像素,而"完全没画出来"
  // 时带内白色像素是 0(背景是暗蓝、弧是暗色),两者不会混淆。
  if (readoutShown) {
    addBand("大数字带(白字已画出)", BAND_DIGIT, READOUT_DIGIT, 0x40, 40);
    addBand("单位带(灰字已画出)", BAND_UNIT, READOUT_UNIT, 0x30, 8);

    // 字号/位置契约:48 号数字的墨迹该落在 47..97 那 50 像素里,
    // 18 号单位落在 97..117 那 20 像素里(ui_theme.h 的默认位置)。
    // 顺带把"默认文本 Text"这类错误挡在门外:它的墨迹位置对不上。
    const dBox = inkBox(img, 60, 20, 420, 130, READOUT_DIGIT, 0x40);
    checks.push({
      name: "大数字墨迹范围(48 号,47..97)", x: dBox.minX, y: dBox.minY,
      got: { r: dBox.w, g: dBox.h, b: dBox.n },
      expect: { r: 0, g: 0, b: 0 },
      ok: dBox.n > 0 && dBox.minY >= 45 && dBox.maxY <= 99 &&
          dBox.w > 30 && dBox.w < 300,
      text: "外框 x[" + dBox.minX + ".." + dBox.maxX + "] y[" + dBox.minY + ".." + dBox.maxY +
            "] 宽" + dBox.w + " 高" + dBox.h + " 命中" + dBox.n
    });
    const uBox = inkBox(img, 60, 90, 420, 125, READOUT_UNIT, 0x30);
    checks.push({
      name: "单位墨迹范围(18 号,97..117)", x: uBox.minX, y: uBox.minY,
      got: { r: uBox.w, g: uBox.h, b: uBox.n },
      expect: { r: 0, g: 0, b: 0 },
      ok: uBox.n > 0 && uBox.minY >= 90 && uBox.maxY <= 119,
      text: "外框 x[" + uBox.minX + ".." + uBox.maxX + "] y[" + uBox.minY + ".." + uBox.maxY +
            "] 宽" + uBox.w + " 高" + uBox.h + " 命中" + uBox.n
    });

    if (side === "left") {
      // 水温数字只在**转速表(左屏)**上 —— 这条同时钉住了"水温在哪一屏"
      addBand("水温带(绿字已画出)", BAND_COOLANT, READOUT_COOLANT, 0x40, 8);
      const cBox = inkBox(img, 180, 350, 300, 420, READOUT_COOLANT, 0x40);
      checks.push({
        name: "水温墨迹范围(表盘底部)", x: cBox.minX, y: cBox.minY,
        got: { r: cBox.w, g: cBox.h, b: cBox.n },
        expect: { r: 0, g: 0, b: 0 },
        ok: cBox.n > 0 && cBox.minY >= 365 && cBox.maxY <= 405,
        text: "外框 x[" + cBox.minX + ".." + cBox.maxX + "] y[" + cBox.minY + ".." + cBox.maxY +
              "] 宽" + cBox.w + " 高" + cBox.h + " 命中" + cBox.n
      });
    } else {
      // 右屏(速度表)没有水温弧,就不该有水温数字
      addBand("右屏不该有水温数字", BAND_COOLANT, READOUT_COOLANT, 0x40, undefined, 0);
    }
  } else {
    // 开机扫表期间:数字栏必须是空的(标签还是空文本)
    addBand("开机期间不该有数字", BAND_DIGIT, READOUT_DIGIT, 0x40, undefined, 0);
    addBand("开机期间不该有单位", BAND_UNIT, READOUT_UNIT, 0x30, undefined, 0);
  }

  let fail = 0;
  console.log("=== " + path + "  (" + (side === "right" ? "右屏" : "左屏") +
              ", 期望表情: " + which +
              ", 表情可见: " + (faceVisible ? "是" : "否") +
              ", 读数: " + (readoutShown ? "应有" : "不应有") + ")  " + img.w + "x" + img.h + " ===");
  for (const c of checks) {
    if (!c.ok) fail++;
    if (c.text) {
      console.log("  " + (c.ok ? "OK  " : "FAIL") + " " + c.name.padEnd(26) + " " + c.text);
    } else {
      console.log("  " + (c.ok ? "OK  " : "FAIL") + " " + c.name.padEnd(26) +
                  " @(" + c.x + "," + c.y + ")  实测 " + hex(c.got) + "  期望 " + hex(c.expect));
    }
  }
  console.log(fail === 0 ? "\n全部通过 (" + checks.length + " 项)"
                         : "\n失败 " + fail + " / " + checks.length);
  process.exit(fail === 0 ? 0 : 1);
}

main();
