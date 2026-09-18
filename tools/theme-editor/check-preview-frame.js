/* ============================================================
 * 读 pcpreview 落的 BMP,按坐标核对颜色 —— 验证图层顺序与透明通道。
 *
 * 为什么靠读 BMP 而不是"看着像":桩驱动丢弃画面、pcpreview 落帧,
 * 唯一能自动判定的就是像素值。这和当初验主题用的是同一套办法。
 *
 * ★ 采样点的选取很讲究(第一版就是栽在这上面):
 *   画面是 480×480,测试底图**满屏 480×480**(原来只有 240×240 居中),
 *   表情 100×100 居中(偏移 190)。
 *   弧线半径按**外沿**语义(见 ui_theme.h 的 ArcStyle):radius 205 / width 24
 *   → 弧带落在 **181..205**(不是 193..217 —— 那是"带宽居中"的旧模型,
 *   实测固件后已改正)。所以采样点要避开:
 *     · 弧带(到圆心距离 181..205 那一圈)
 *     · 表情的不透明中心(屏幕 220..260)
 *   否则读到的是"弧线颜色"或"表情颜色",会把正确的图层顺序误判成失败
 *   (第一版就踩了:在 (125,125) 读到弧线色,以为是背景没画上)。
 *
 * ★ 底图为什么改成满屏(外部复审指出后改的):
 *   240×240 的底图离圆心最远 169.7(在角上),与弧带 181..205 **不重叠** ——
 *   于是"弧压在背景图之上"这条**根本验不到**(当时只在文档里记了一笔)。
 *   铺满整屏之后弧带上每个点都在底图上,这条就成了可断言的像素事实:
 *   点亮处必须是**弧色**,不是底图色(见第 3 组)。
 *   代价:弧**缺口**处读到的也变成底图色(不再是被主题底色填的),
 *   朝向检查跟着改成"缺口=底图色"。
 *
 * 用法: node tools/theme-editor/check-preview-frame.js <bmp> <表情> [表情可见] [左/右] [有读数]
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

// ★ 8 个状态各一个颜色,和 make-test-blob.js 写进去的一一对应。
//   **两屏的状态集合不一样**(左有红区、右有超速),所以两张表分开放 ——
//   这也正是"每屏一套独立表情"要验的东西:
//   看左屏时如果读到右屏的颜色,说明左右串了。
const FACE_L = {
  idle:    { r: 0xff, g: 0x00, b: 0x00 },           // 左屏·常态 = 红
  cruise:  { r: 0xff, g: 0xff, b: 0x00 },           // 左屏·巡航 = 黄
  sport:   { r: 0x00, g: 0xff, b: 0x00 },           // 左屏·运动 = 绿
  redline: { r: 0x00, g: 0xff, b: 0xff }            // 左屏·红区 = 青
};
const FACE_R = {
  idle:     { r: 0x00, g: 0x80, b: 0xff },          // 右屏·常态 = 浅蓝
  cruise:   { r: 0xff, g: 0x80, b: 0x00 },          // 右屏·巡航 = 橙
  sport:    { r: 0x80, g: 0x00, b: 0xff },          // 右屏·运动 = 紫
  overspeed: { r: 0xff, g: 0xff, b: 0xff }         // 右屏·超速 = 白(号 8,当年是"惊喜")
};
// 弧线色(主题默认):转速弧(左屏**外**弧)点亮 #FF5C5C、轨道 #232323,轨道 opa=153/255。
// ★ 这里是"参考值"不是判定:采样点落在弧的**轨道**上(值没涨到那儿),
//   而轨道是半透明的 —— 实际颜色 = 轨道色按 153/255 与屏幕底色混合,
//   手算会差几个数(实测 #181818)。精确值属于实现细节,断言只看
//   "不是底色、不是纯黑"(见下面第 2 组检查)。
// ★ 曾经有个 ARC_LIT = 车速蓝 的常量:它既没参与判定,颜色也说错了屏
//   (左屏外弧是转速红,车速蓝在右屏),已删 —— 免得下次有人照它写断言。
// 轨道压在**主题底色**(#101410)上的期望值,仅用于失败时打印对比
const ARC_TRACK_OVER_BG = {
  r: Math.round(0x23 * 153 / 255 + 0x10 * (1 - 153 / 255)),
  g: Math.round(0x23 * 153 / 255 + 0x14 * (1 - 153 / 255)),
  b: Math.round(0x23 * 153 / 255 + 0x10 * (1 - 153 / 255))
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
//   数字 0xFFFFFF(白)、单位 0x9AA0A6(灰)、水温 0x7CFF6B(绿)、进气温度 0xFFB020(琥珀)
// ------------------------------------------------------------
const READOUT_DIGIT   = { r: 0xFF, g: 0xFF, b: 0xFF };
const READOUT_UNIT    = { r: 0x9A, g: 0xA0, b: 0xA6 };
const READOUT_COOLANT = { r: 0x7C, g: 0xFF, b: 0x6B };
const READOUT_INTAKE  = { r: 0xFF, g: 0xB0, b: 0x20 };

// 副表(内圈)弧的几何(默认主题):两条都是 radius 168、width 10,
// LVGL 把带宽往里长 → 弧带占半径 [158, 168]。
// ★ 它和"副表数字"是同一个颜色,所以扫数字时必须按**半径**把它分开,
//   光用矩形框不行 —— 实测踩过:弧从数字两侧绕过来,外接框从 40×13 变成 81×19。
const COOLANT_ARC_INNER_R = 168 - 10;   // = 158
const COOLANT_ARC_OUTER_R = 168;        // = 168
// 进气温度弧与水温弧几何完全相同(这是产品契约,两条副表左右对称),
// 所以共用同一组半径;分开起名只是让读数处读起来清楚。
const INTAKE_ARC_INNER_R = COOLANT_ARC_INNER_R;
const INTAKE_ARC_OUTER_R = COOLANT_ARC_OUTER_R;

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
//
// radiusFilter(可选):只统计"到圆心距离满足条件"的像素。
// ★ 水温数字与水温弧是**同一个绿色**,而弧在数字下方两侧绕过来 ——
//   用矩形框扫必然把弧框进去(实测:盒子从 40×13 变成 81×19)。
//   所以按**半径**分开:数字在弧的内半径以内,弧在弧带上。
function inkBox(img, x0, y0, x1, y1, want, tol, radiusFilter) {
  let minX = 1e9, maxX = -1, minY = 1e9, maxY = -1, n = 0;
  for (let y = y0; y <= y1; y++) {
    for (let x = x0; x <= x1; x++) {
      if (radiusFilter) {
        const dx = x - 240, dy = y - 240;
        if (!radiusFilter(Math.sqrt(dx * dx + dy * dy))) continue;
      }
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
//   水温中心 y=384。x 取中间一段,避开弧带(外沿语义下是 181..205 那一圈)。
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
  if (!path) { console.error("用法: check-preview-frame.js <bmp> [idle|cruise|sport|redline|overspeed] [yes|no] [left|right] [yes|no]"); process.exit(2); }

  const table = (side === "right") ? FACE_R : FACE_L;
  const face = table[which];
  if (!face) {
    console.error("未知表情: " + which + "（" + (side === "right" ? "右屏" : "左屏") +
                  " 只有 " + Object.keys(table).join("/") + "）");
    process.exit(2);
  }

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
  // 左中 / 右中边缘:距圆心 112 —— 在弧带(181..205)**之内侧**,且落在测试底图上
  add("背景左中边缘", 128, 240, BG);
  add("背景右中边缘", 352, 240, BG);

  // --- 2. 弧带上确实画了弧 ---
  // 对角线上取半径 200 的点(≈141,141 偏移)落在弧带**内侧**(带是 181..205)。
  //
  // ★ 半径为什么不是 205:205 正好是对象的**外沿**,边界像素会被裁剪/半亮,
  //   读出来可能是底色(实测过 —— 朝向检查那边也专门注明了这件事)。
  //   采在带内才是在验弧本身。
  // ★ 口径:**不断言精确颜色**。弧轨道是半透明的(track_opa=153/255),
  //   最终颜色取决于 LVGL 的合成实现与量化,手算的期望值会差几个数
  //   (实测 #181818,我手算 #1D1D1D);精确值是实现细节,不是契约。
  //   这里要证的是"这一圈有东西(不是底图色、不是纯黑)"。
  //   ※ 45° 正好是转速弧的**端点**(135..405 的末端),所以这里读到的是
  //     轨道/端点混合,不能当"弧色"用 —— 要验弧色看第 3 组的 12 点钟。
  {
    const d = Math.round(200 / Math.SQRT2);
    const got = img.px(240 + d, 240 + d);
    const isBg = dist(got, BG) <= 4;
    const isBlack = (got.r + got.g + got.b) <= 6;
    const lum = got.r + got.g + got.b;
    checks.push({
      name: "弧带点(半径 200 处有弧,非底图色/非纯黑)", x: 240 + d, y: 240 + d,
      got, expect: ARC_TRACK_OVER_BG,
      ok: !isBg && !isBlack && lum < 0x40 * 3      // 暗色但不是纯黑
    });
  }

  // --- 3. 表盘朝向(契约:满量程弧的缺口在正下方)+ 弧压在底图之上 ---
  //
  // ★ 朝向这条是踩过坑才加的:两个编辑器画弧时多减了 90°(`(d-90)`),
  //   预览里的表整体被逆时针转了 90° —— 起点从 7:30 跑到 4:30、
  //   缺口从正下方跑到正右方。固件一直是对的,用户却先看到预览,
  //   于是问"表的方向是否需要向右旋转 90 度"。
  //   这里用**真实落帧**把固件的朝向钉住(预览那份由 test-gauge-geometry.js 管):
  //     沿弧带半径 200 扫一圈,正下方(90°)必须是**缺口**(底图色),
  //     正上方(270°)必须有弧(轨道或点亮色)。
  //   半径取 200(而不是 205):弧带宽 24,205 正好在对象边界上,
  //   边界像素会被裁剪/半亮,读出来不可靠。
  // ★ 底图改成满屏之后,这一组同时成了"弧压在背景图**之上**"的**直接**证据:
  //   12 点钟那个点在底图范围内(480×480 铺满),它读出来是**弧的点亮色**
  //   (#FF5C5C 系,红通道占绝对多数)—— 说明弧把底图盖住了,而不是被底图盖住
  //   (那样会读到 #000062)。以前这条验不了,因为底图够不到弧带。
  {
    const at = (deg) => {
      const rad = deg * Math.PI / 180;
      return img.px(Math.round(240 + 200 * Math.cos(rad)),
                    Math.round(240 + 200 * Math.sin(rad)));
    };
    const bottom = at(90);      // 6 点钟
    const top = at(270);        // 12 点钟
    // 缺口:底图铺满整屏 → 应当读到**底图色**(暗蓝 #000062)
    const bottomIsBg = dist(bottom, BG) <= 6;
    // 有弧:弧在这里(轨道或点亮色),总之**不是**底图色
    const topIsArc = dist(top, BG) > 6;
    checks.push({
      name: "表盘朝向(缺口在正下方)", x: 240, y: 440,
      got: bottom, expect: BG,
      ok: bottomIsBg,
      text: "6 点钟 " + hex(bottom) + (bottomIsBg ? " = 缺口(露出底图) ✓"
                                                 : " ≠ 缺口(说明表被转过了)")
    });
    checks.push({
      name: "表盘朝向(正上方有弧,非底图色)", x: 240, y: 40,
      got: top, expect: { r: 0xff, g: 0x5c, b: 0x5c },
      ok: topIsArc,
      text: "12 点钟 " + hex(top) + (topIsArc ? " = 有弧 ✓" : " = 底图色(说明表被转过了)")
    });
  }

  // --- 3b. 弧压在背景图**之上**(外部复审指出唯一没验到的一条,补上) ---
  //
  // ★ 为什么以前验不了:老夹具的底图只有 240×240,离圆心最远 169.7,
  //   而弧带在 181..205 —— 两者不重叠,怎么采都采不到"弧 + 底图"的同一像素。
  //   现在底图铺满 480×480,弧带上每个点都在底图上。
  // ★ 判定方式:在弧的角度范围内(135°..405°,避开 45°..135° 的缺口)
  //   沿半径 200 采一圈。**每一个**点都必须不是底图色 ——
  //   不管是点亮的红还是半透明的轨道,都说明弧是画在底图**上面**的;
  //   反过来,如果弧被底图盖住,这一圈会读到底图色 #000062。
  //   这条同时把"这一圈真的画了弧"钉住了(比上面 45° 那个单点强得多 ——
  //   45° 正好是弧的端点,读到的是边界混合色)。
  // ★ 与数值无关:满量程时这一圈是点亮色,怠速时大部分是轨道色,两种都算通过。
  {
    const angles = [150, 180, 210, 240, 270, 300, 330, 360, 390];
    const miss = [];
    for (const a of angles) {
      const rad = a * Math.PI / 180;
      const p = img.px(Math.round(240 + 200 * Math.cos(rad)),
                       Math.round(240 + 200 * Math.sin(rad)));
      if (dist(p, BG) <= 6) miss.push(a + "°=" + hex(p));
    }
    checks.push({
      name: "弧压在背景图之上(整圈弧带都不是底图色)", x: 240, y: 40,
      got: { r: angles.length - miss.length, g: angles.length, b: 0 },
      expect: { r: angles.length, g: angles.length, b: 0 },
      ok: miss.length === 0,
      text: miss.length === 0
        ? angles.length + " 个角度全部读到弧(点亮或轨道),底图被盖在下面 ✓"
        : miss.length + " 个角度读到底图色(弧可能被底图盖住): " + miss.join("  ")
    });
  }

  // --- 4. 表情在最上层 / 或者开机阶段应当还看不见 ---
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
      // 左屏**不该有进气温度数字**:两条副表各在自己屏上,互串就是屏↔表映射错了
      addBand("左屏不该有进气温度数字", BAND_COOLANT, READOUT_INTAKE, 0x30, undefined, 0);

      // 数字:只统计**水温弧内半径以内**的绿像素(弧和数字同为绿色,几何分家)
      const insideArc = (r) => r <= COOLANT_ARC_INNER_R - 3;
      const cBox = inkBox(img, 120, 340, 360, 430, READOUT_COOLANT, 0x40, insideArc);
      checks.push({
        name: "水温墨迹范围(弧内,表盘底部)", x: cBox.minX, y: cBox.minY,
        got: { r: cBox.w, g: cBox.h, b: cBox.n },
        expect: { r: 0, g: 0, b: 0 },
        ok: cBox.n > 0 && cBox.minY >= 360 && cBox.maxY <= 402,
        text: "外框 x[" + cBox.minX + ".." + cBox.maxX + "] y[" + cBox.minY + ".." + cBox.maxY +
              "] 宽" + cBox.w + " 高" + cBox.h + " 命中" + cBox.n
      });

      // 水温弧:同色但落在弧带上 —— 它从数字两侧绕过去,不能和数字咬在一起。
      // 这一项同时证明"弧确实画在水温数字外面那一圈"。
      const onArc = (r) => r >= COOLANT_ARC_INNER_R - 1 && r <= COOLANT_ARC_OUTER_R + 1;
      const aBox = inkBox(img, 120, 340, 360, 430, READOUT_COOLANT, 0x40, onArc);
      const arcInnerY = 240 + COOLANT_ARC_INNER_R;   // 正下方弧内沿的 y
      checks.push({
        name: "水温弧在数字外侧绕行", x: 240, y: arcInnerY,
        got: { r: aBox.n, g: cBox.maxY, b: 0 },
        expect: { r: 0, g: 0, b: 0 },
        ok: aBox.n > 0 && cBox.maxY < arcInnerY,
        text: "弧带上命中 " + aBox.n + " 像素；数字最低点 y=" + cBox.maxY +
              " < 弧内沿 y=" + arcInnerY + "（留 " + (arcInnerY - cBox.maxY) + " 像素）"
      });

      // ★ 涨幅方向(镜像):水温弧是 reverse=1 —— 点亮区从**左端(9 点钟)**起涨。
      //   实测法:在弧带上取左端附近与右端附近各一点,
      //   左端应是**点亮色**(水温数字同色,但按半径已经分开)、右端应只是**轨道**。
      //   取样角度选得离两端有点余量:仿真水温 77~93℃ → t=0.24~0.47 →
      //   点亮区 [180-180t, 180] = 至少 [95°,180°],所以 170° 一定亮、15° 一定不亮。
      {
        const arcPt = (deg) => {
          const rad = deg * Math.PI / 180;
          const r = (COOLANT_ARC_INNER_R + COOLANT_ARC_OUTER_R) / 2;
          return img.px(Math.round(240 + r * Math.cos(rad)),
                        Math.round(240 + r * Math.sin(rad)));
        };
        const leftPt = arcPt(170);    // 靠近 9 点钟(左端)
        const rightPt = arcPt(15);    // 靠近 3 点钟(右端)
        const isLit = (c) => dist(c, READOUT_COOLANT) <= 0x50;
        const litLeft = isLit(leftPt), litRight = isLit(rightPt);
        checks.push({
          name: "水温弧从左端起涨(镜像)", x: 240, y: 240,
          got: leftPt, expect: READOUT_COOLANT,
          ok: litLeft && !litRight,
          text: "左端(170°) " + hex(leftPt) + (litLeft ? " = 点亮" : " = 未亮") +
                "；右端(15°) " + hex(rightPt) + (litRight ? " = 点亮(方向反了)" : " = 轨道") 
        });
      }
    } else {
      // 右屏(速度表)**不该有水温数字** —— 两条副表各在自己屏上:
      // 左屏水温室温、右屏进气温度。互串了就说明屏↔表映射错了。
      addBand("右屏不该有水温数字", BAND_COOLANT, READOUT_COOLANT, 0x40, undefined, 0);

      // ---- 进气温度(右屏副表,2026-09 加,颜色琥珀 0xFFB020)----
      addBand("进气温度带(琥珀字已画出)", BAND_COOLANT, READOUT_INTAKE, 0x30, 8);

      const insideArcIn = (r) => r <= INTAKE_ARC_INNER_R - 3;
      const iBox = inkBox(img, 120, 340, 360, 430, READOUT_INTAKE, 0x30, insideArcIn);
      checks.push({
        name: "进气温度墨迹范围(弧内,表盘底部)", x: iBox.minX, y: iBox.minY,
        got: { r: iBox.w, g: iBox.h, b: iBox.n },
        expect: { r: 0, g: 0, b: 0 },
        ok: iBox.n > 0 && iBox.minY >= 360 && iBox.maxY <= 402,
        text: "外框 x[" + iBox.minX + ".." + iBox.maxX + "] y[" + iBox.minY + ".." + iBox.maxY +
              "] 宽" + iBox.w + " 高" + iBox.h + " 命中" + iBox.n
      });

      const onArcIn = (r) => r >= INTAKE_ARC_INNER_R - 1 && r <= INTAKE_ARC_OUTER_R + 1;
      const iaBox = inkBox(img, 120, 340, 360, 430, READOUT_INTAKE, 0x30, onArcIn);
      const arcInnerYIn = 240 + INTAKE_ARC_INNER_R;
      checks.push({
        name: "进气温度弧在数字外侧绕行", x: 240, y: arcInnerYIn,
        got: { r: iaBox.n, g: iBox.maxY, b: 0 },
        expect: { r: 0, g: 0, b: 0 },
        ok: iaBox.n > 0 && iBox.maxY < arcInnerYIn,
        text: "弧带上命中 " + iaBox.n + " 像素；数字最低点 y=" + iBox.maxY +
              " < 弧内沿 y=" + arcInnerYIn + "（留 " + (arcInnerYIn - iBox.maxY) + " 像素）"
      });

      // ★ 镜像:进气温度弧也是 reverse=1 —— 点亮区从**左端(9 点钟)**起涨。
      //   仿真进气温度 22~37℃ → t=(22..37-0)/80 = 0.275..0.46 →
      //   点亮区 [180-180t, 180] = 至少 [97°,180°],所以 170° 一定亮、15° 一定不亮。
      {
        const arcPtIn = (deg) => {
          const rad = deg * Math.PI / 180;
          const r = (INTAKE_ARC_INNER_R + INTAKE_ARC_OUTER_R) / 2;
          return img.px(Math.round(240 + r * Math.cos(rad)),
                        Math.round(240 + r * Math.sin(rad)));
        };
        const leftPtIn = arcPtIn(170);
        const rightPtIn = arcPtIn(15);
        const isLitIn = (c) => dist(c, READOUT_INTAKE) <= 0x50;
        const litLeftIn = isLitIn(leftPtIn), litRightIn = isLitIn(rightPtIn);
        checks.push({
          name: "进气温度弧从左端起涨(镜像)", x: 240, y: 240,
          got: leftPtIn, expect: READOUT_INTAKE,
          ok: litLeftIn && !litRightIn,
          text: "左端(170°) " + hex(leftPtIn) + (litLeftIn ? " = 点亮" : " = 未亮") +
                "；右端(15°) " + hex(rightPtIn) + (litRightIn ? " = 点亮(方向反了)" : " = 轨道")
        });
      }
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
