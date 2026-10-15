/* ============================================================
 * canvas-shim.js —— **够用的** canvas 2D 替身（Node 侧，只给测试用）
 *
 * 为什么需要它
 * ------------------------------------------------------------
 * "表情框选之后不够居中"这条反馈，**只有量最终像素**才能回答到底是
 * "产物错了"还是"预览显示错了"。而产物那一段（框选 → renderToTarget →
 * rgbaToRgb565A8）用的是 `document.createElement("canvas")` —— 仓库里
 * 没有浏览器、也没有 jsdom/canvas 这类依赖（页面要能离线双击打开，
 * 不能引第三方库）。
 *
 * 所以在 Node 里塞一个**最小**的 2D 上下文实现：只实现那两个编辑器
 * 真的用到、且会影响像素的那几个方法：
 *   · createElement("canvas") / width / height
 *   · getContext("2d")
 *   · clearRect / fillRect（缩略图与棋盘格用）
 *   · drawImage(img, sx,sy,sw,sh, dx,dy,dw,dh) —— ★ 这句是**框选/缩放的核心**
 *   · createImageData / putImageData / getImageData
 *   · imageSmoothingEnabled / imageSmoothingQuality / globalAlpha
 *   · setTransform / save / restore / beginPath / arc / stroke / fillStyle …（**画弧那些是空操作**：
 *     本模块只保证"图像搬运"是对的，不假装会画矢量图）
 *
 * ★ 采样规则与真 canvas **不同**（这里是盒式平均，浏览器多半是双线性）——
 *   所以它**不能**用来断言"渲染好不好看"。它只用来断言**几何**：
 *   某个不透明区域的**包围盒**落在哪、有没有被平移。
 *   本仓库的既有风格也是这样：能用算法说清的事不用"看着对"来判。
 * ★ 输入可以是另一个 canvas、一个 {width,height,data} 的假图，
 *   或任何一个自带 `_rgba`（Uint8ClampedArray，RGBA 行优先）的对象。
 * ============================================================ */
"use strict";

function clamp255(v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

// 从任意"图"上取 w/h 与 RGBA 像素
function sourceOf(img) {
  if (!img) return null;
  if (img._rgba) return { w: img.width, h: img.height, rgba: img._rgba };
  if (img.data) return { w: img.width, h: img.height, rgba: img.data };
  const w = img.naturalWidth || img.width || 0;
  const h = img.naturalHeight || img.height || 0;
  if (img._rgba) return { w: w, h: h, rgba: img._rgba };
  return { w: w, h: h, rgba: null };
}

class Ctx {
  constructor(cv) {
    this.canvas = cv;
    this.fillStyle = "#000";
    this.strokeStyle = "#000";
    this.lineWidth = 1;
    this.globalAlpha = 1;
    this.font = "";
    this.textAlign = "";
    this.textBaseline = "";
    this.imageSmoothingEnabled = true;
    this.imageSmoothingQuality = "high";
  }
  // ---- 只影响像素的这几个：真实现 ----
  setTransform() {}                 // 页面只用 (1,0,0,1,0,0) 重置
  save() {} restore() {}
  clearRect(x, y, w, h) {
    const cv = this.canvas;
    const x0 = Math.max(0, Math.floor(x)), y0 = Math.max(0, Math.floor(y));
    const x1 = Math.min(cv.width, Math.ceil(x + w)), y1 = Math.min(cv.height, Math.ceil(y + h));
    for (let yy = y0; yy < y1; yy++) {
      for (let xx = x0; xx < x1; xx++) {
        const i = (yy * cv.width + xx) * 4;
        cv._rgba[i] = 0; cv._rgba[i + 1] = 0; cv._rgba[i + 2] = 0; cv._rgba[i + 3] = 0;
      }
    }
  }
  fillRect(x, y, w, h) {
    const col = parseColor(this.fillStyle);
    if (!col) return;                        // 认不出的颜色（渐变之类）就不填
    const a = col[3] * this.globalAlpha;
    const cv = this.canvas;
    const x0 = Math.max(0, Math.floor(x)), y0 = Math.max(0, Math.floor(y));
    const x1 = Math.min(cv.width, Math.ceil(x + w)), y1 = Math.min(cv.height, Math.ceil(y + h));
    for (let yy = y0; yy < y1; yy++) {
      for (let xx = x0; xx < x1; xx++) {
        const i = (yy * cv.width + xx) * 4;
        cv._rgba[i] = clamp255(col[0] * a + cv._rgba[i] * (1 - a));
        cv._rgba[i + 1] = clamp255(col[1] * a + cv._rgba[i + 1] * (1 - a));
        cv._rgba[i + 2] = clamp255(col[2] * a + cv._rgba[i + 2] * (1 - a));
        cv._rgba[i + 3] = clamp255(255 * a + cv._rgba[i + 3] * (1 - a));
      }
    }
  }
  drawImage(img) {
    const src = sourceOf(img);
    if (!src || !src.rgba) return;           // 没像素的图（桩里的 Image）⇒ 什么都不画
    const cv = this.canvas;
    // 参数（与浏览器一致）：
    //   (img, dx, dy)                          → 原尺寸画在 (dx,dy)
    //   (img, dx, dy, dw, dh)                  → 缩放着画
    //   (img, sx, sy, sw, sh, dx, dy, dw, dh)  → 取源矩形再画
    // ★ 第一版把 3 参数那种当成 (dw,dh) 了 —— 于是 `drawImage(img,0,0)` 变成
    //   "画成 0×0"（什么都画不出来），而它正是 subjectBox() 与缩略图用的那种写法。
    //   这个错**不会抛异常**，只会让像素为空 —— 所以本模块自己也得有小自检
    //   （见文件末的 selfTest()），别让"替身错了"被当成"页面错了"。
    let sx = 0, sy = 0, sw = src.w, sh = src.h, dx = 0, dy = 0, dw = src.w, dh = src.h;
    const a = arguments;
    if (a.length === 3) { dx = a[1]; dy = a[2]; }
    else if (a.length === 5) { dx = a[1]; dy = a[2]; dw = a[3]; dh = a[4]; }
    else if (a.length === 9) {
      sx = a[1]; sy = a[2]; sw = a[3]; sh = a[4];
      dx = a[5]; dy = a[6]; dw = a[7]; dh = a[8];
    }
    if (!(dw > 0) || !(dh > 0) || !(sw > 0) || !(sh > 0)) return;

    // 目标区域（整数像素网格；与浏览器一样四舍五入到像素）
    const dx0 = Math.round(dx), dy0 = Math.round(dy);
    const dwI = Math.round(dw), dhI = Math.round(dh);
    const x0 = Math.max(0, dx0), y0 = Math.max(0, dy0);
    const x1 = Math.min(cv.width, dx0 + dwI), y1 = Math.min(cv.height, dy0 + dhI);
    const kx = sw / dwI, ky = sh / dhI;
    for (let y = y0; y < y1; y++) {
      for (let x = x0; x < x1; x++) {
        // ★ 与浏览器一致的**覆盖率加权**盒式平均：
        //   目标像素覆盖的源区间是 [ux0, ux1)，只把这个区间**内**的像素按覆盖比例算进去
        //   —— 直接把 [floor(ux0), ceil(ux1)] 整块平均会让边缘像素多看一格，
        //   于是"半像素的偏移"被算成 1 px（本模块第一版就是这么错的，
        //   量出来的包围盒比真值宽 1 px、中心偏 +1 px）。
        const ux0 = sx + (x - dx0) * kx, ux1 = ux0 + kx;
        const uy0 = sy + (y - dy0) * ky, uy1 = uy0 + ky;
        const ix0 = Math.max(0, Math.floor(ux0)), ix1 = Math.min(src.w, Math.ceil(ux1));
        const iy0 = Math.max(0, Math.floor(uy0)), iy1 = Math.min(src.h, Math.ceil(uy1));
        let r = 0, g = 0, b = 0, al = 0, wsum = 0;
        for (let yy = iy0; yy < iy1; yy++) {
          const fy = Math.max(0, Math.min(uy1, yy + 1) - Math.max(uy0, yy));
          if (fy <= 0) continue;
          for (let xx = ix0; xx < ix1; xx++) {
            const fx = Math.max(0, Math.min(ux1, xx + 1) - Math.max(ux0, xx));
            if (fx <= 0) continue;
            const wgt = fx * fy;
            const i = (yy * src.w + xx) * 4;
            r += src.rgba[i] * wgt; g += src.rgba[i + 1] * wgt; b += src.rgba[i + 2] * wgt;
            al += src.rgba[i + 3] * wgt; wsum += wgt;
          }
        }
        if (wsum <= 0) continue;
        const sa = al / wsum / 255;
        const ga = sa * this.globalAlpha;
        const di = (y * cv.width + x) * 4;
        const da = cv._rgba[di + 3] / 255;
        const oa = ga + da * (1 - ga);
        if (oa <= 0) {
          cv._rgba[di] = 0; cv._rgba[di + 1] = 0; cv._rgba[di + 2] = 0; cv._rgba[di + 3] = 0;
          continue;
        }
        // 直通（非预乘）合成
        for (let c = 0; c < 3; c++) {
          const sc = [r, g, b][c] / wsum;
          cv._rgba[di + c] = clamp255((sc * ga + cv._rgba[di + c] * da * (1 - ga)) / oa);
        }
        cv._rgba[di + 3] = clamp255(oa * 255);
      }
    }
  }
  createImageData(w, h) {
    return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) };
  }
  putImageData(img, dx, dy) {
    const cv = this.canvas;
    for (let y = 0; y < img.height; y++) {
      for (let x = 0; x < img.width; x++) {
        const tx = x + dx, ty = y + dy;
        if (tx < 0 || ty < 0 || tx >= cv.width || ty >= cv.height) continue;
        const si = (y * img.width + x) * 4, di = (ty * cv.width + tx) * 4;
        for (let c = 0; c < 4; c++) cv._rgba[di + c] = img.data[si + c];
      }
    }
  }
  getImageData(x, y, w, h) {
    const cv = this.canvas;
    const out = new Uint8ClampedArray(w * h * 4);
    for (let yy = 0; yy < h; yy++) {
      for (let xx = 0; xx < w; xx++) {
        const sx = x + xx, sy = y + yy;
        if (sx < 0 || sy < 0 || sx >= cv.width || sy >= cv.height) continue;
        const si = (sy * cv.width + sx) * 4, di = (yy * w + xx) * 4;
        for (let c = 0; c < 4; c++) out[di + c] = cv._rgba[si + c];
      }
    }
    return { width: w, height: h, data: out };
  }
  // ---- 画矢量图那几个：空操作（本模块不假装会画弧） ----
  beginPath() {} closePath() {} arc() {} moveTo() {} lineTo() {}
  stroke() {} fill() {} translate() {} scale() {} rotate() {}
  roundRect() {} fillText() {} strokeText() {}
  measureText() { return { width: 0 }; }
}

function parseColor(s) {
  if (typeof s !== "string") return null;
  const m = /^#([0-9a-f]{6})$/i.exec(s.trim());
  if (!m) return null;
  const n = parseInt(m[1], 16);
  return [(n >> 16) & 255, (n >> 8) & 255, n & 255, 1];
}

class Canvas {
  constructor(w, h) {
    this._w = w || 300;
    this._h = h || 150;
    this._rgba = new Uint8ClampedArray(this._w * this._h * 4);
    this._ctx = null;
  }
  get width() { return this._w; }
  set width(v) { this._w = v | 0; this._rgba = new Uint8ClampedArray(this._w * this._h * 4); }
  get height() { return this._h; }
  set height(v) { this._h = v | 0; this._rgba = new Uint8ClampedArray(this._w * this._h * 4); }
  getContext() { if (!this._ctx) this._ctx = new Ctx(this); return this._ctx; }
  toDataURL() { return "data:image/png;base64,"; }
  getBoundingClientRect() { return { left: 0, top: 0, width: this._w, height: this._h }; }
  setAttribute() {}
  addEventListener() {}
  get style() { return {}; }
}

// 造一张"图"（给 openCropper 当 img 用：只要有 naturalWidth/Height 与 _rgba）
function makeImage(w, h, painter) {
  const rgba = new Uint8ClampedArray(w * h * 4);
  const img = {
    naturalWidth: w, naturalHeight: h, width: w, height: h, _rgba: rgba,
    draw: function (ctx) { painter(ctx); }
  };
  if (painter) painter({ put: function (x, y, r, g, b, a) { setPixel(img, x, y, r, g, b, a); },
                         fillRect: function (x, y, ww, hh, r, g, b, a) {
                           for (let yy = y; yy < y + hh; yy++)
                             for (let xx = x; xx < x + ww; xx++) setPixel(img, xx, yy, r, g, b, a);
                         } });
  return img;
}

function setPixel(img, x, y, r, g, b, a) {
  if (x < 0 || y < 0 || x >= img.width || y >= img.height) return;
  const i = (y * img.width + x) * 4;
  img._rgba[i] = r; img._rgba[i + 1] = g; img._rgba[i + 2] = b; img._rgba[i + 3] = a;
}

// 把一个 canvas / image 包成页面里 `document.createElement("canvas")` 的返回物。
// ★ `getElementById` 也得有：页面的 cropFit() 会读 `#crop-stage.clientWidth`
//   来决定预览画布多大（真实浏览器里就是那个元素的实际宽度）。
function makeDocument(opts) {
  const stageWidth = (opts && opts.stageWidth) || 460;
  const byId = {};
  const rootStyle = {
    _v: {},
    setProperty: function (k, v) { this._v[k] = String(v); },
    getPropertyValue: function (k) { return this._v[k] || ""; }
  };
  return {
    documentElement: { style: rootStyle },
    createElement: function (tag) {
      if (String(tag).toLowerCase() === "canvas") return new Canvas(300, 150);
      return { style: {}, setAttribute() {}, appendChild() {}, addEventListener() {} };
    },
    getElementById: function (id) {
      if (!byId[id]) {
        const cv = new Canvas(stageWidth, stageWidth);
        byId[id] = { id: id, clientWidth: stageWidth, clientHeight: stageWidth,
                     style: {}, classList: { add() {}, remove() {}, toggle() {} },
                     setAttribute() {}, appendChild() {}, addEventListener() {},
                     getContext: cv.getContext.bind(cv),
                     width: cv.width, height: cv.height };
      }
      return byId[id];
    }
  };
}

// 不透明像素的包围盒（alpha ≥ 阈值）。返回 null = 一个都没有。
function opaqueBBox(img, alphaMin) {
  const th = alphaMin === undefined ? 8 : alphaMin;
  const w = img.width, h = img.height, px = img._rgba || img.data;
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      if (px[(y * w + x) * 4 + 3] >= th) {
        if (x < x0) x0 = x;
        if (x > x1) x1 = x;
        if (y < y0) y0 = y;
        if (y > y1) y1 = y;
      }
    }
  }
  if (x1 < 0) return null;
  return {
    x0: x0, y0: y0, x1: x1, y1: y1,
    w: x1 - x0 + 1, h: y1 - y0 + 1,
    cx: (x0 + x1) / 2, cy: (y0 + y1) / 2
  };
}

// ------------------------------------------------------------
// 自检：替身错了会让"页面错了"这个结论**反过来**
//   （本模块第一版就把 drawImage(img,dx,dy) 当成 (dw,dh)，于是什么都画不出来，
//     而表现是"框选产物全是空的"—— 差点被当成页面的 bug）。
//   所以每次 require 都跑一遍最小的对账；不达标就直接抛，别让上面的测试拿到
//   一个"永远画不出东西"的替身还以为自己测过了。
// ------------------------------------------------------------
function selfTest() {
  const fail = [];
  const chk = (cond, what) => { if (!cond) fail.push(what); };

  // ① drawImage(img,dx,dy) = 原尺寸
  {
    const img = makeImage(2, 2, (p) => p.fillRect(0, 0, 2, 2, 10, 20, 30, 255));
    const cv = new Canvas(4, 4);
    cv.getContext("2d").drawImage(img, 1, 1);
    const d = cv._rgba;
    chk(d[(1 * 4 + 1) * 4 + 3] === 255, "drawImage(img,dx,dy) 没画在 (dx,dy)");
    chk(d[(0 * 4 + 0) * 4 + 3] === 0, "drawImage(img,dx,dy) 的 (0,0) 该是空的");
  }
  // ② drawImage(img,dx,dy,dw,dh) = 缩放
  {
    const img = makeImage(2, 2, (p) => p.fillRect(0, 0, 2, 2, 10, 20, 30, 255));
    const cv = new Canvas(4, 4);
    cv.getContext("2d").drawImage(img, 0, 0, 4, 4);
    const bb = opaqueBBox(cv);
    chk(bb && bb.w === 4 && bb.h === 4, "drawImage(img,0,0,4,4) 没铺满 4×4");
  }
  // ③ 9 参数：取子矩形 + 居中（框选器就靠这一条）
  {
    const img = makeImage(8, 8, (p) => p.fillRect(2, 3, 2, 2, 200, 0, 0, 255));
    const cv = new Canvas(4, 4);
    cv.getContext("2d").drawImage(img, 2, 3, 2, 2, 0, 0, 4, 4);
    const bb = opaqueBBox(cv);
    chk(bb && bb.w === 4 && bb.h === 4, "9 参数的 drawImage 没把源矩形铺满目标");
  }
  // ④ getImageData / clearRect
  {
    const cv = new Canvas(2, 2);
    const ctx = cv.getContext("2d");
    ctx.fillRect(0, 0, 2, 2);
    ctx.clearRect(0, 0, 1, 1);
    const d = ctx.getImageData(0, 0, 2, 2).data;
    chk(d[3] === 0, "clearRect 没清掉 (0,0) 的 alpha");
    chk(d[(1 * 2 + 1) * 4 + 3] === 0, "fillRect 没给默认色（fillStyle 认不出 ⇒ 不该乱填）");
  }
  if (fail.length) {
    throw new Error("canvas-shim 自检没过（替身本身有问题，别拿它去判页面）：\n  - " +
                    fail.join("\n  - "));
  }
}
selfTest();

module.exports = {
  Canvas: Canvas, Ctx: Ctx, makeImage: makeImage, setPixel: setPixel,
  makeDocument: makeDocument, opaqueBBox: opaqueBBox, selfTest: selfTest
};
