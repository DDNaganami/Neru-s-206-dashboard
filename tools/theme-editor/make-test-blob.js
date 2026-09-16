/* ============================================================
 * 造一个"颜色可辨认"的测试 image.bin,用来在 pcpreview 里逐像素验证图层。
 *
 * 为什么需要它:pcpreview 把真实 LVGL 的渲染结果落成 BMP 帧,所以只要测试图
 * 的每个区域颜色是已知的,就能反查:
 *   · 背景图有没有被画上去(暗蓝底)
 *   · 是不是衬在弧线下面(四角与中心标记的颜色不能被弧线盖掉)
 *   · 表情图在不在最上层、位置对不对(中心方块的纯色)
 *   · 透明通道有没有生效(表情图四角透明 → 应透出背景的暗蓝)
 *
 * 用法: node tools/theme-editor/make-test-blob.js <输出路径>
 * ============================================================ */
"use strict";

const fs = require("fs");
const IB = require("./image-blob-build.js");

// 造一张纯色 RGBA
function solid(w, h, r, g, b, a) {
  const out = new Uint8Array(w * h * 4);
  for (let i = 0; i < out.length; i += 4) {
    out[i] = r; out[i + 1] = g; out[i + 2] = b; out[i + 3] = a;
  }
  return out;
}

function fillRect(rgba, w, h, x0, y0, x1, y1, r, g, b, a) {
  for (let y = y0; y < y1; y++) {
    for (let x = x0; x < x1; x++) {
      const i = (y * w + x) * 4;
      rgba[i] = r; rgba[i + 1] = g; rgba[i + 2] = b; rgba[i + 3] = a;
    }
  }
}

// 背景:暗蓝底 + 中心品红标记(位置写死在注释里,读取脚本按同一坐标核对)
const BG = 240, BG_MARK = { x0: 120, y0: 120, x1: 132, y1: 132, r: 255, g: 0, b: 255 };
function makeBackground() {
  const rgba = solid(BG, BG, 0, 0, 96, 255);            // 暗蓝
  fillRect(rgba, BG, BG, BG_MARK.x0, BG_MARK.y0, BG_MARK.x1, BG_MARK.y1,
           BG_MARK.r, BG_MARK.g, BG_MARK.b, 255);
  return rgba;
}

// 表情:100×100,四周 30px 透明,中心 40×40 纯色
// → 透明区应透出背景的暗蓝;中心区应盖住背景
const FACE = 100, FACE_BORDER = 30, FACE_CENTER = 40;
function makeFace(r, g, b) {
  const rgba = solid(FACE, FACE, 0, 0, 0, 0);           // 全透明
  const c0 = FACE_BORDER, c1 = FACE_BORDER + FACE_CENTER;
  fillRect(rgba, FACE, FACE, c0, c0, c1, c1, r, g, b, 255);
  return rgba;
}

function mk(name, role, order, rgba, w, h, alpha) {
  return {
    name, w, h, role, order,
    cf: alpha ? IB.CF.RGB565A8 : IB.CF.RGB565,
    pixels: alpha ? IB.rgbaToRgb565A8(rgba, w, h) : IB.rgbaToRgb565(rgba, w, h)
  };
}

const items = [
  mk("testbg", IB.ROLE.Background, 0, makeBackground(), BG, BG, false),
  // 左屏(车速表)三态
  mk("faceL0", IB.ROLE.FaceIdle,     0, makeFace(255, 0, 0),   FACE, FACE, true),  // 红
  mk("faceL1", IB.ROLE.FaceRedline,  0, makeFace(255, 255, 0), FACE, FACE, true),  // 黄
  mk("faceL2", IB.ROLE.FaceSurprise, 0, makeFace(0, 255, 0),   FACE, FACE, true),  // 绿
  // 右屏(转速表)三态 —— 用不同颜色,便于验证"左右屏取的是各自那一套"
  mk("faceR0", IB.ROLE.FaceIdleR,     0, makeFace(0, 128, 255),   FACE, FACE, true),  // 浅蓝
  mk("faceR1", IB.ROLE.FaceRedlineR,  0, makeFace(255, 128, 0),   FACE, FACE, true),  // 橙
  mk("faceR2", IB.ROLE.FaceSurpriseR, 0, makeFace(128, 0, 255),   FACE, FACE, true)   // 紫
];

const res = IB.build(items);
const outPath = process.argv[2] || "test-image.bin";
fs.writeFileSync(outPath, Buffer.from(res.blob));

console.log("已生成 " + outPath + "  (" + res.totalBytes + " 字节, " + res.entries.length + " 张)");
console.log("背景: 大小 " + BG + "x" + BG + " 暗蓝(0,0,96);中心标记 " +
            (BG_MARK.x1 - BG_MARK.x0) + "x" + (BG_MARK.y1 - BG_MARK.y0) +
            " 品红(255,0,255) @ (" + BG_MARK.x0 + "," + BG_MARK.y0 + ")");
console.log("表情: 大小 " + FACE + "x" + FACE + ",四周 " + FACE_BORDER +
            "px 透明,中心 " + FACE_CENTER + "x" + FACE_CENTER + " 纯色");
