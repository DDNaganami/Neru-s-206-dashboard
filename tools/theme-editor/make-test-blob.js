/* ============================================================
 * 造一个"颜色可辨认"的测试 image.bin,用来在 pcpreview 里逐像素验证图层。
 *
 * 为什么需要它:pcpreview 把真实 LVGL 的渲染结果落成 BMP 帧,所以只要测试图
 * 的每个区域颜色是已知的,就能反查:
 *   · 背景图有没有被画上去(暗蓝底,铺满整屏)
 *   · 弧线是不是画在背景图**之上**(弧带半径 181..205 全落在底图上,
 *     点亮处必须读出弧色而不是底图色 —— 这条靠 480×480 的底图才验得了)
 *   · 表情图在不在**背景之上**、位置对不对(中心方块的纯色)
 *     ★ **弧压在表情之上**(2026-09-24 层序定稿) ⇒ 探针只该取"没被弧带扫到"
 *       的中心区;想验"弧到底压不压得住表情"要用一张**盖到弧带**的大图,
 *       见 check-layer-order.js / check-layer-ab.js
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
//
// ★ 尺寸是**满屏 480×480**,不是"够用就行":
//   弧带在半径 181..205。底图如果只有 240×240(离圆心最远 169.7 在角上),
//   它和弧带**根本不重叠** —— 于是"弧压在背景图之上"这条根本没法验
//   (外部复审指出的唯一遗漏就是这个)。铺满整屏之后,
//   弧带上的每个点都落在底图上,"点亮色盖住了底图色"就成了可断言的事实。
//   代价:弧的**缺口**处读到的也变成底图色而不是主题底色,朝向检查要跟着改
//   (见 check-preview-frame.js 第 3 组)。
const BG = 480, BG_MARK = { x0: 234, y0: 234, x1: 246, y1: 246, r: 255, g: 0, b: 255 };
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

// 8 张表情,每张一个**互不相同**的颜色 —— 这样才能逐像素验证
// "哪一屏、哪个状态,用的是哪一张图"。
// 颜色选择的两条约束:
//   · RGB565 量化后要基本不变(所以用 8 位能整除到 5/6 位的值:0/128/255)
//   · 不要用背景标记的品红(255,0,255),否则和"表情透明"分不清
// 左屏(转速表):常态红 / 巡航黄 / 运动绿 / 红区青
// 右屏(速度表):常态浅蓝 / 巡航橙 / 运动紫 / 超速白
const LEFT_FACES = [
  { name: "L-idle",    role: IB.ROLE.FaceIdle,    rgb: [255, 0, 0] },
  { name: "L-cruise",  role: IB.ROLE.FaceCruise,  rgb: [255, 255, 0] },
  { name: "L-sport",   role: IB.ROLE.FaceSport,   rgb: [0, 255, 0] },
  { name: "L-redline", role: IB.ROLE.FaceRedline, rgb: [0, 255, 255] }
];
const RIGHT_FACES = [
  { name: "R-idle",     role: IB.ROLE.FaceIdleR,     rgb: [0, 128, 255] },
  { name: "R-cruise",   role: IB.ROLE.FaceCruiseR,   rgb: [255, 128, 0] },
  { name: "R-sport",    role: IB.ROLE.FaceSportR,    rgb: [128, 0, 255] },
  { name: "R-overspeed", role: IB.ROLE.FaceOverspeedR, rgb: [255, 255, 255] }
];

const items = [mk("testbg", IB.ROLE.Background, 0, makeBackground(), BG, BG, false)]
  .concat(LEFT_FACES.map(f => mk(f.name, f.role, 0, makeFace(...f.rgb), FACE, FACE, true)))
  .concat(RIGHT_FACES.map(f => mk(f.name, f.role, 0, makeFace(...f.rgb), FACE, FACE, true)));

const res = IB.build(items);
const outPath = process.argv[2] || "test-image.bin";
fs.writeFileSync(outPath, Buffer.from(res.blob));

console.log("已生成 " + outPath + "  (" + res.totalBytes + " 字节, " + res.entries.length + " 张)");
console.log("背景: 大小 " + BG + "x" + BG + " 暗蓝(0,0,96);中心标记 " +
            (BG_MARK.x1 - BG_MARK.x0) + "x" + (BG_MARK.y1 - BG_MARK.y0) +
            " 品红(255,0,255) @ (" + BG_MARK.x0 + "," + BG_MARK.y0 + ")");
console.log("表情: 大小 " + FACE + "x" + FACE + ",四周 " + FACE_BORDER +
            "px 透明,中心 " + FACE_CENTER + "x" + FACE_CENTER + " 纯色");
for (const f of LEFT_FACES) console.log("  左屏 " + f.role + " " + f.name + " = rgb(" + f.rgb.join(",") + ")");
for (const f of RIGHT_FACES) console.log("  右屏 " + f.role + " " + f.name + " = rgb(" + f.rgb.join(",") + ")");
