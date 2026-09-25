// 左屏（转速表）/ 右屏（速度表）的**像素判据** —— 2026-09-25 新增
//
// 用途：`node tools/theme-editor/check-gauge-identity.js <左屏帧.bmp> <右屏帧.bmp>`
//
// 起因（车主原话）："**副表怎么也给你刷成速度表了**" —— 从板镜像上电后显示的是
// **速度表**，而它该显示**转速表（左屏 + 水温副表）**。修法之后，"屏上到底是哪一块表"
// 需要一个**不靠人眼、也不靠日志**的判据（日志说的是"我们要求它显示哪一套"，
// 而像素说的是"它**真的**画了哪一套"）。
//
// 判据来源：默认主题里两块表的弧**颜色就是不同的**（`lib/themetool/ui_theme.h` 的
// `theme_reset_to_defaults()`）：
//     左屏（转速表）: 外圈转速弧 = 红 0xFF5C5C + 内圈水温弧 = 绿 0x7CFF6B
//     右屏（速度表）: 外圈车速弧 = 蓝 0x39C5FF + 内圈进气弧 = 琥珀 0xFFB020
// ⇒ 一帧里"红像素多不多、蓝像素多不多"就能把两块表分开，而且是**几何无关**的
//    （弧具体画到哪个角度不影响这个结论）。
//
// ★ 它**不是**几何用例（弧角度/半径那些在 `test-gauge-geometry.js` 与
//   `check-layer-order.js` 里）；这一条只回答"**这是哪一块表**"。
// ★ 它**不改任何判据的来源**：颜色取自固件源码里的那两个 `lv_color_hex(...)`
//   （本脚本把那一行**解析出来**，而不是自己抄一遍 —— 抄一遍就会有一天与固件分叉）。
'use strict';

const fs = require('fs');
const path = require('path');

// ------------------------------------------------------------
// 1. 从固件源码里解析出两块表的主弧颜色（唯一事实来源）
// ------------------------------------------------------------
function readArcColors() {
  const header = fs.readFileSync(
    path.join(__dirname, '..', '..', 'lib', 'themetool', 'ui_theme.h'), 'utf8');
  // 只看 `theme_reset_to_defaults()` 里那两行 `arcs[0] = ArcStyle{ ArcKind::X, ... }`
  const pick = (kind) => {
    const re = new RegExp('ArcKind::' + kind + ',\\s*135,\\s*405[\\s\\S]*?lv_color_hex\\(0x([0-9A-Fa-f]{6})\\),\\s*0\\s*\\}');
    const m = header.match(re);
    if (!m) throw new Error('解析不到 ' + kind + ' 的弧颜色（ui_theme.h 改过？）');
    return m[1].toUpperCase();
  };
  return { rpm: pick('Rpm'), speed: pick('Speed') };
}

// ------------------------------------------------------------
// 2. 读 24bpp BMP（`src/dash_display.cpp` 落的那种：自下而上、行 4 字节对齐）
// ------------------------------------------------------------
function readBmp(file) {
  const buf = fs.readFileSync(file);
  if (buf.length < 54 || buf[0] !== 0x42 || buf[1] !== 0x4D) throw new Error(file + ': 不是 BMP');
  const dataOff = buf.readUInt32LE(10);
  const w = buf.readInt32LE(18);
  const h = buf.readInt32LE(22);
  const bpp = buf.readUInt16LE(28);
  if (bpp !== 24) throw new Error(file + ': 只支持 24bpp（这份固件落的就是 24bpp）');
  const row = (w * 3 + 3) & ~3;
  return {
    w, h, file,
    // 返回**按屏坐标**（y 从顶到底）的采样器
    px(x, y) {
      const yy = h - 1 - y;                       // BMP 自下而上
      const o = dataOff + yy * row + x * 3;
      return { b: buf[o], g: buf[o + 1], r: buf[o + 2] };
    },
  };
}

// ------------------------------------------------------------
// 3. 一张图里某类颜色的像素统计（带一点容差：抗锯齿/端帽会让边缘混色）
// ------------------------------------------------------------
function countColor(img, hex, tol) {
  const R = parseInt(hex.slice(0, 2), 16);
  const G = parseInt(hex.slice(2, 4), 16);
  const B = parseInt(hex.slice(4, 6), 16);
  let n = 0;
  for (let y = 0; y < img.h; ++y) {
    for (let x = 0; x < img.w; ++x) {
      const c = img.px(x, y);
      if (Math.abs(c.r - R) <= tol && Math.abs(c.g - G) <= tol && Math.abs(c.b - B) <= tol) ++n;
    }
  }
  return n;
}

// 弧带上的像素（把"表情底色/读数"那几块排除掉）：只看半径 150..215 的环带。
// ★ 为什么还要分环带：转速/车速弧在半径 205±12；表情与读数都在半径 ~140 以内。
function countInArcBand(img, hex, tol) {
  const R = parseInt(hex.slice(0, 2), 16);
  const G = parseInt(hex.slice(2, 4), 16);
  const B = parseInt(hex.slice(4, 6), 16);
  const cx = img.w / 2, cy = img.h / 2;
  let n = 0;
  for (let y = 0; y < img.h; ++y) {
    for (let x = 0; x < img.w; ++x) {
      const dx = x + 0.5 - cx, dy = y + 0.5 - cy;
      const rr = Math.sqrt(dx * dx + dy * dy);
      if (rr < 150 || rr > 218) continue;
      const c = img.px(x, y);
      if (Math.abs(c.r - R) <= tol && Math.abs(c.g - G) <= tol && Math.abs(c.b - B) <= tol) ++n;
    }
  }
  return n;
}

// ------------------------------------------------------------
// 4. 主判据
// ------------------------------------------------------------
function main() {
  const args = process.argv.slice(2);
  if (args.length !== 2) {
    console.error('用法: node check-gauge-identity.js <第一帧.bmp> <第二帧.bmp>');
    console.error('（两个参数是**同一块板上、同一时刻**的两份落帧：通常 l_XXXX.bmp 与 r_XXXX.bmp）');
    process.exit(2);
  }
  const colors = readArcColors();
  const tol = 40;   // 弧是纯色 + 圆头端帽带来的少量混色；40/255 足够宽、又不会把别的颜色吃进来

  const imgs = args.map(readBmp);
  const rows = imgs.map((img) => ({
    file: path.basename(img.file),
    rpmRed: countInArcBand(img, colors.rpm, tol),
    speedBlue: countInArcBand(img, colors.speed, tol),
  }));

  let ok = true;
  console.log('gauge-identity: 主弧颜色（取自 lib/themetool/ui_theme.h） '
    + `rpm=0x${colors.rpm}(红) speed=0x${colors.speed}(蓝)；统计环带 r=150..218`);
  for (const r of rows) {
    // 一块表只该有**自己那条**主弧：转速表上红多蓝少，速度表上蓝多红少。
    const kind = (r.rpmRed > r.speedBlue) ? 'LEFT/rpm' : 'RIGHT/speed';
    const margin = Math.abs(r.rpmRed - r.speedBlue);
    const verdict = (margin > 200) ? 'OK' : 'AMBIGUOUS';
    if (verdict !== 'OK') ok = false;
    console.log(`  ${r.file}: rpmRed=${r.rpmRed} speedBlue=${r.speedBlue} `
      + `=> ${kind} (margin=${margin}) ${verdict}`);
  }
  // 两块表必须是**一块转速表 + 一块速度表**（这正是"从板也刷成速度表"那个 bug 的形态：
  // 两块都是速度表 ⇒ 这里会红）。
  const kinds = rows.map((r) => (r.rpmRed > r.speedBlue) ? 'L' : 'R').sort().join('');
  console.log(`  kinds=${kinds}  verdict: ${kinds === 'LR' ? 'ONE-RPM-ONE-SPEED OK' : 'BOTH-SAME ✗'}`);
  if (kinds !== 'LR') ok = false;
  console.log(ok ? '  => GAUGE-IDENTITY-OK' : '  => GAUGE-IDENTITY-FAIL');
  process.exit(ok ? 0 : 1);
}

main();
