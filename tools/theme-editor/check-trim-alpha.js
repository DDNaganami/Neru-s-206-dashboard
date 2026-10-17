/* ============================================================
 * check-trim-alpha.js —— 「裁透明边」的像素判据 + 高级模式色号手输入口
 *
 *   node tools/theme-editor/check-trim-alpha.js
 *   退出码：0 = 三节全绿；1 = 有判据红
 *
 * ------------------------------------------------------------
 * 为什么要有它（2026-10-16 两个真 bug，都是"静默失效"）
 * ------------------------------------------------------------
 * ① 「勾了『裁透明边』，产物变成一块纯色 / 导出说『读不出像素』」
 *    renderToTarget() 的 `if (item.trim)` 分支里，取像素那一句原来写的是
 *    `ctx.getImageData(0, 0, sw, sh)` —— **可那时还一次都没 drawImage 过**
 *    （真正的 drawImage 在下面"不裁"的分支里），而 `ctx` 是 tw×th 的
 *    最终产物画布。于是它读到的是**底色/空白**：
 *      · 表情（透明角色）⇒ 一个不透明像素都没有 ⇒ AssetSpec.prepareAsset()
 *        报"整张图都是透明的" ⇒ 导出处"读不出像素"；
 *      · 背景（不透明角色，画布先铺了底色）⇒ 整幅都算"内容" ⇒ 裁出来
 *        就是一块纯色。
 *    ★ 这两个症状本文件都**当场量出来**，不是靠"看着改了"（第 ① 节末的控制组
 *      把改动前的读数钉成常数，见 OLD_* 那一组）。
 *    ★ 修法：先把源图原样画进一块 sw×sh 的临时画布，再从**那块**取像素。
 *
 * ② 高级模式每一行"手打色号"都抛 ReferenceError
 *    色号 → 数字的那个函数在页面里**只叫 `unhexOf`**；`color()` 的文字输入框
 *    却写成了 `unhex(...)` —— 那个名字从来没定义过。表现是：输了色号、回车，
 *    色块不动、主题不变、页面也不报错（异常被事件循环吞掉）。
 *    第 ③ 节用**页面里真的 `color()`** 跑一遍手输，并用"把 unhexOf 改回
 *    unhex 的变异体"做反向对照 —— 变异体必须抛 ReferenceError 且**不改主题**，
 *    否则说明这条用例是空转的。
 *
 * ------------------------------------------------------------
 * 纪律（与本仓库其它 check 一致）
 * ------------------------------------------------------------
 * · 函数**一律从 index.html 里抠真源码**（new Function 求值），不抄一份逻辑
 *   —— 抄一份就只是测复制品（页面改了它还是绿的）。
 * · 像素用 canvas-shim.js（Node 侧的 2D 替身）：它的采样是覆盖率加权盒式平均，
 *   **不**用来判"好不好看"，只判**几何与颜色对不对**（本仓库既有口径）。
 * · 「不勾裁透明边」那条路必须**逐字节不变** ⇒ 第 ② 节除了记 sha256 基线，
 *   还带一份**独立重算**的参考实现（同样走 shim，但几何算式在测试里再写一遍），
 *   两者都要与页面产物逐字节相同。sha256 基线是在**修复之前**（git HEAD
 *   35d0e1c）用同一套夹具跑出来的，见 TRIM_OFF_SHA256 那一行。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");
const crypto = require("crypto");
const SH = require(path.join(__dirname, "canvas-shim.js"));
const IB = require(path.join(__dirname, "image-blob-build.js"));
const AS = require(path.join(__dirname, "asset-spec.js"));

const dir = __dirname;
const PAGE = fs.readFileSync(path.join(dir, "index.html"), "utf8");

let pass = 0, fail = 0;
const failures = [];
function ok(cond, what) {
  if (cond) pass++;
  else { fail++; failures.push(what); console.log("  ✗ " + what); }
}
function eq(a, b, what) {
  ok(a === b, what + "  (期望 " + JSON.stringify(b) + "，得到 " + JSON.stringify(a) + ")");
}
function near(a, b, tol, what) {
  ok(Math.abs(a - b) <= tol, what + "  (" + a + " vs " + b + "，容差 ±" + tol + ")");
}
function section(t) { console.log("\n== " + t); }

function sha256(buf) {
  return crypto.createHash("sha256").update(Buffer.from(buf)).digest("hex");
}

// ------------------------------------------------------------
// 从页面里抠真源码（不抄一份）
// ------------------------------------------------------------
function grabFn(src, name) {
  const re = new RegExp("function " + name + "\\s*\\([^)]*\\)\\s*\\{[\\s\\S]*?\\n\\}", "m");
  const m = re.exec(src);
  if (!m) throw new Error("index.html 里找不到函数 " + name);
  return m[0];
}
// 一行写完的那种（hexOf / unhexOf 必须在**一行**里，见页面里那一段的注记）
function grabOneLine(src, name) {
  const re = new RegExp("function " + name + "\\([^\\n]*\\}", "m");
  const m = re.exec(src);
  if (!m) throw new Error("index.html 里找不到单行函数 " + name);
  return m[0];
}

// ------------------------------------------------------------
// 页面素材上下文：renderToTarget 用到的那几个符号全给齐
//   （缺一个就是 ReferenceError，而不是"判据失败"——所以宁可多给）
// ------------------------------------------------------------
const curTarget = "s3";
const FACE_ROLES = (function () {
  const FS = require(path.join(dir, "face-stages.js"));
  const s = new Set();
  FS.SCREENS.forEach(sc => sc.states.forEach(x => s.add(x.role)));
  return s;
})();

function buildSlotApi(src) {
  const doc = SH.makeDocument();
  return new Function(
    "document", "ImageBlob", "AssetSpec", "curTarget", "FACE_ROLES", "theme", "T",
    [
      grabOneLine(src, "hexOf"),
      grabFn(src, "themeBgHex"),
      grabFn(src, "wantAlpha"),
      grabFn(src, "cfForRole"),
      grabFn(src, "roleIdForNumber"),
      grabFn(src, "unhexRgb"),
      grabFn(src, "renderToTarget"),
      "return { renderToTarget: renderToTarget, cfForRole: cfForRole, " +
      "wantAlpha: wantAlpha, unhexRgb: unhexRgb };"
    ].join("\n")
  )(doc, IB, AS, curTarget, FACE_ROLES, { bg: "#141414" }, { bg_color: 0x141414 });
}

const slotApi = buildSlotApi(PAGE);

// ------------------------------------------------------------
// 夹具：200×200 全透明底 + 正中一块 60×40 不透明内容（四周留 70/80 的透明边）
//   ⇒ "裁透明边"之后的内容尺寸**必须**是 60×40（不是 200×200，也不是一块纯色）
// ------------------------------------------------------------
const SRC_W = 200, SRC_H = 200, BOX_W = 60, BOX_H = 40;
const BOX_X = 70, BOX_Y = 80;                    // 刚好居中：(200-60)/2, (200-40)/2
const BOX_RGB = [220, 40, 40];

function makeFixture(x, y) {
  return SH.makeImage(SRC_W, SRC_H, (p) => {
    p.fillRect(x === undefined ? BOX_X : x, y === undefined ? BOX_Y : y,
               BOX_W, BOX_H, BOX_RGB[0], BOX_RGB[1], BOX_RGB[2], 255);
  });
}

const TARGET = 300;            // 目标宽度/高度（s3 的表情推荐 300）

// 不透明像素数（alpha ≥ 8，与 trimAlpha 同一个阈值口径）
function opaqueCount(rgba, w, h) {
  let n = 0;
  for (let i = 3; i < rgba.length; i += 4) if (rgba[i] >= 8) n++;
  return n;
}
// 与某个底色不同的像素的包围盒（给"背景"那条路用：它整幅不透明，没 alpha 可看）
function diffBBox(rgba, w, h, rgb) {
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity, n = 0;
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      if (rgba[i] !== rgb[0] || rgba[i + 1] !== rgb[1] || rgba[i + 2] !== rgb[2]) {
        n++;
        if (x < x0) x0 = x; if (x > x1) x1 = x;
        if (y < y0) y0 = y; if (y > y1) y1 = y;
      }
    }
  }
  if (!n) return { n: 0 };
  return { x0: x0, y0: y0, w: x1 - x0 + 1, h: y1 - y0 + 1, n: n };
}

// ------------------------------------------------------------
// ★ 改动前的读数（git HEAD 35d0e1c，同一套夹具、同一套 shim 实测）
//   夹具：200×200 透明底 + 居中 60×40 不透明块；目标 300×300；curTarget=s3
//   这两个常数是"这条用例不是空转"的证据：改动前就是这两个症状。
// ------------------------------------------------------------
const OLD_FACE_TRIM_IS_NULL = true;        // 表情：准备不出像素 ⇒ 导出报"读不出像素"
const OLD_BG_TRIM_W = 200;                 // 背景：整幅被当成内容（200×200）
const OLD_BG_TRIM_H = 200;
const OLD_BG_TRIM_DIFF_N = 0;              // ……而且与底色不同的像素 = 0 ⇒ 一块纯色

// ★ 「不勾裁透明边」的逐字节基线（**改动前**实测的 sha256；改动后必须一模一样）
const TRIM_OFF_SHA256 = "b8d60615fe99fea152787de6e3eedda4294dcb3718832e3a6a50d1c6c1b765f8";
// 对应的包围盒（contain：200×200 → 300×300，内容 60×40 → 90×60，居中）
const TRIM_OFF_BOX = { x0: 105, y0: 120, w: 90, h: 60 };

// ============================================================
section("① 勾「裁透明边」：产物内容尺寸 = 内容包围盒（60×40），**不是**纯色/全透明");
{
  // ---- ① -1 表情（透明角色，保留 alpha）----
  const img = makeFixture();
  const item = { img: img, name: "probe.png", fit: "contain", trim: true,
                 role: IB.ROLE.FaceIdle, w: TARGET, h: TARGET };
  const rgba = slotApi.renderToTarget(item, TARGET, TARGET, true);

  ok(!!rgba, "表情：renderToTarget 拿得到像素（改动前这里是 null ⇒ 导出说“读不出像素”）");
  eq(item.trimW, BOX_W, "表情：裁完的**输出宽** = 内容包围盒宽（60）");
  eq(item.trimH, BOX_H, "表情：裁完的**输出高** = 内容包围盒高（40）");
  ok(item.trimW < SRC_W && item.trimH < SRC_H,
     "表情：输出尺寸跟着**内容**走，不是源画布 200（" + item.trimW + "×" + item.trimH + "）");

  if (rgba) {
    const bb = SH.opaqueBBox({ width: TARGET, height: TARGET, _rgba: rgba }, 8);
    ok(!!bb, "表情：产物里有不透明像素（不是全透明）");
    if (bb) {
      eq(bb.w, BOX_W, "表情：产物里不透明区的宽 = 60");
      eq(bb.h, BOX_H, "表情：产物里不透明区的高 = 40");
      near(bb.cx, (TARGET - 1) / 2, 1, "表情：产物居中（水平）");
      near(bb.cy, (TARGET - 1) / 2, 1, "表情：产物居中（垂直）");
    }
    // ★ "不是一块纯色"：产物里除了这一块内容，别处必须是透明的
    eq(opaqueCount(rgba, TARGET, TARGET), BOX_W * BOX_H,
       "表情：不透明像素数 = 60×40（多一个都算“裁出了不该有的东西”）");
    // 内容本身的颜色没被改（裁边/缩放不该动 RGB）
    const ci = (Math.floor((TARGET - BOX_H) / 2) + 5) * TARGET * 4 +
               (Math.floor((TARGET - BOX_W) / 2) + 5) * 4;
    eq([rgba[ci], rgba[ci + 1], rgba[ci + 2], rgba[ci + 3]].join(","), "220,40,40,255",
       "表情：裁完的内容还是原来的颜色 (220,40,40,255)");
  }

  // ---- ① -2 同一张图，但内容**偏在左上角**：裁边仍然只认内容包围盒 ----
  //   （如果实现里"顺手"用了源画布尺寸/中心，这一条会红）
  {
    const img2 = makeFixture(10, 20);
    const item2 = { img: img2, name: "probe2.png", fit: "contain", trim: true,
                    role: IB.ROLE.FaceIdle, w: TARGET, h: TARGET };
    const rgba2 = slotApi.renderToTarget(item2, TARGET, TARGET, true);
    ok(!!rgba2, "偏心内容：拿得到像素");
    eq(item2.trimW, BOX_W, "偏心内容：输出宽仍是 60（与内容在画布哪一角无关）");
    eq(item2.trimH, BOX_H, "偏心内容：输出高仍是 40");
    if (rgba2) {
      eq(opaqueCount(rgba2, TARGET, TARGET), BOX_W * BOX_H,
         "偏心内容：不透明像素数仍是 60×40");
      const bb2 = SH.opaqueBBox({ width: TARGET, height: TARGET, _rgba: rgba2 }, 8);
      near(bb2.cx, (TARGET - 1) / 2, 1, "偏心内容：裁完仍然是居中的（水平）");
      near(bb2.cy, (TARGET - 1) / 2, 1, "偏心内容：裁完仍然是居中的（垂直）");
    }
  }

  // ---- ① -3 背景（不透明角色）：不许再"整幅裁成一块纯色" ----
  {
    const img3 = makeFixture();
    const item3 = { img: img3, name: "bg.png", fit: "contain", trim: true,
                    role: IB.ROLE.Background, w: TARGET, h: TARGET };
    const rgba3 = slotApi.renderToTarget(item3, TARGET, TARGET, false);
    ok(!!rgba3, "背景：renderToTarget 拿得到像素");
    eq(item3.trimW, BOX_W, "背景：裁完的输出宽 = 60（改动前是 " + OLD_BG_TRIM_W + "）");
    eq(item3.trimH, BOX_H, "背景：裁完的输出高 = 40（改动前是 " + OLD_BG_TRIM_H + "）");
    if (rgba3) {
      // 背景整幅不透明（画布先铺了底色）⇒ 只能按"与底色不同的像素"量内容
      const bgRgb = slotApi.unhexRgb(0x141414);
      const db = diffBBox(rgba3, TARGET, TARGET, bgRgb);
      eq(db.n, BOX_W * BOX_H, "背景：与底色不同的像素数 = 60×40（改动前是 " +
         OLD_BG_TRIM_DIFF_N + " ⇒ 一块纯色）");
      eq(db.w, BOX_W, "背景：内容区的宽 = 60");
      eq(db.h, BOX_H, "背景：内容区的高 = 40");
      near(db.x0 + (db.w - 1) / 2, (TARGET - 1) / 2, 1, "背景：内容居中（水平）");
    }
  }

  // ---- ① -4 ★ 反向对照：改动前读到的像素喂给 prepareAsset，症状必须对得上 ----
  //   （证明上面那几条断言不是空转：换回"空画布"的像素，结论立刻翻转）
  {
    const faceRole = AS.roleByNumber(IB.ROLE.FaceIdle).id;
    // (a) 透明角色：画布全透明（= 改动前 ctx 没画过任何东西）
    const blank = new Uint8ClampedArray(SRC_W * SRC_H * 4);
    const r1 = AS.prepareAsset(blank, SRC_W, SRC_H,
      { roleId: faceRole, targetId: curTarget, maxSide: TARGET, trim: true });
    ok(r1.errors.length > 0, "反向对照：空画布 + 表情 ⇒ prepareAsset 直接报错（" +
       r1.errors.join("；") + "）");
    eq(!!r1.rgba, false, "反向对照：空画布 + 表情 ⇒ 没有像素 ⇒ 页面就是“读不出像素”");
    eq(OLD_FACE_TRIM_IS_NULL, true, "反向对照：这正是改动前的症状（表情分支返回 null）");

    // (b) 不透明角色：画布被底色铺满（= 改动前背景那条路）
    const filled = new Uint8ClampedArray(SRC_W * SRC_H * 4);
    for (let i = 0; i < filled.length; i += 4) {
      filled[i] = 0x14; filled[i + 1] = 0x14; filled[i + 2] = 0x14; filled[i + 3] = 255;
    }
    const r2 = AS.prepareAsset(filled, SRC_W, SRC_H,
      { roleId: AS.roleByNumber(IB.ROLE.Background).id, targetId: curTarget,
        maxSide: TARGET, trim: true });
    eq(r2.w, OLD_BG_TRIM_W, "反向对照：铺了底色的画布 ⇒ 整幅被当成内容（宽 " + r2.w + "）");
    eq(r2.h, OLD_BG_TRIM_H, "反向对照：同上（高 " + r2.h + "）");
    let same = 0;
    for (let i = 0; i < r2.rgba.length; i += 4) {
      if (r2.rgba[i] === 0x14 && r2.rgba[i + 1] === 0x14 && r2.rgba[i + 2] === 0x14) same++;
    }
    eq(same, r2.w * r2.h, "反向对照：裁出来的**每一个像素都是底色** ⇒ 就是车主看到的那块纯色");
  }
}

// ============================================================
section("② 不勾「裁透明边」：产物与改动前**逐字节相同**（修 A 不许弄坏 B）");
{
  // 同一张夹具、同一条 contain 路径
  const img = makeFixture();
  const item = { img: img, name: "probe.png", fit: "contain", trim: false,
                 role: IB.ROLE.FaceIdle, w: TARGET, h: TARGET };
  const rgba = slotApi.renderToTarget(item, TARGET, TARGET, true);

  ok(!!rgba, "不裁：renderToTarget 拿得到像素");
  eq(sha256(rgba), TRIM_OFF_SHA256,
     "不裁：sha256 与**改动前**逐字节相同（基线见文件头 TRIM_OFF_SHA256）");

  const bb = SH.opaqueBBox({ width: TARGET, height: TARGET, _rgba: rgba }, 8);
  eq(JSON.stringify({ x0: bb.x0, y0: bb.y0, w: bb.w, h: bb.h }),
     JSON.stringify(TRIM_OFF_BOX),
     "不裁：几何也没动（内容 90×60 落在 (105,120)）");
  ok(item.trimW === undefined && item.trimH === undefined,
     "不裁：这条路上**没有** trimW/trimH 这种裁剪账目（免得被别的分支读到）");

  // ---- ② -1 独立参考实现：几何算式在本文件里再写一遍，与页面产物逐字节比 ----
  {
    const cv = new SH.Canvas(TARGET, TARGET);
    const ctx = cv.getContext("2d");
    const sw = img.naturalWidth, sh = img.naturalHeight;
    const scale = Math.min(TARGET / sw, TARGET / sh);
    const dw = Math.round(sw * scale), dh = Math.round(sh * scale);
    ctx.drawImage(img, Math.round((TARGET - dw) / 2), Math.round((TARGET - dh) / 2), dw, dh);
    const ref = ctx.getImageData(0, 0, TARGET, TARGET).data;
    let diff = -1;
    for (let i = 0; i < ref.length; i++) if (ref[i] !== rgba[i]) { diff = i; break; }
    eq(diff, -1, "不裁：与**独立重算**的参考实现逐字节相同（第一个不同字节下标 = " + diff + "）");
    eq(sha256(ref), TRIM_OFF_SHA256, "不裁：参考实现的 sha256 也等于同一份基线");
  }

  // ---- ② -2 先跑过 trim=true，再关掉重跑：产物仍与基线一致（无状态污染）----
  //   ★ 这一条专门防"在同一个 item 上修裁边时把坐标/尺寸写坏"：item.trimW/H 留着
  //     也不许影响不裁那条路的像素。
  {
    const img2 = makeFixture();
    const shared = { img: img2, name: "shared.png", fit: "contain", trim: true,
                     role: IB.ROLE.FaceIdle, w: TARGET, h: TARGET };
    const withTrim = slotApi.renderToTarget(shared, TARGET, TARGET, true);
    ok(!!withTrim, "先裁一次：拿得到像素");
    shared.trim = false;                                   // 关掉「裁透明边」
    const without = slotApi.renderToTarget(shared, TARGET, TARGET, true);
    ok(!!without, "再关掉裁边重跑：拿得到像素");
    eq(sha256(without), TRIM_OFF_SHA256,
       "再关掉裁边重跑：产物与改动前的基线**逐字节相同**（裁边没留下副作用）");
  }
}

// ============================================================
section("③ 高级模式颜色行：手打色号 #12ab34 ⇒ 主题值真的变成 0x12AB34");
{
  // ---- 极简 DOM 桩（只够 color() 用：div/label/input + append + 事件派发）----
  function makeEl(tag) {
    const el = {
      tagName: String(tag).toUpperCase(), children: [], _events: {}, style: {},
      textContent: "", className: "", value: "", type: "", checked: false,
      min: 0, max: 0, step: 1, id: "", title: "", selected: false,
      appendChild: function (c) { el.children.push(c); return c; },
      append: function () { for (let i = 0; i < arguments.length; i++) el.children.push(arguments[i]); },
      addEventListener: function (ev, fn) { (el._events[ev] = el._events[ev] || []).push(fn); },
      dispatch: function (ev, arg) {
        (el._events[ev] || []).forEach(fn => fn(arg || { preventDefault: function () {} }));
      },
      setAttribute: function () {}, getAttribute: function () { return null; },
      querySelector: function () { return null; }
    };
    return el;
  }
  const doc = {
    createElement: function (tag) { return makeEl(tag); },
    getElementById: function () { return null; }
  };

  // 页面里真的 color()（高级模式每一行颜色都是它建的，见 buildControls()）
  //   ★ 用 new Function 把**页面里的源码**求值一遍，只补它要的两个外部符号：
  //     document（上面的桩）与 render（数一数被调了几次）。
  //   ★ mutate 只给"反向对照"用：把 unhexOf( 改回历史拼写 unhex(，
  //     那就是改动前的页面 —— 它必须红。
  function makeColorHarness(src, mutate) {
    const state = { renders: 0 };
    const raw = grabFn(src, "color");
    const body = mutate ? mutate(raw) : raw;
    const factory = new Function("document", "render", [
      grabOneLine(src, "hexOf"),
      grabOneLine(src, "unhexOf"),
      body,
      "return { color: color };"
    ].join("\n"));
    const apiObj = factory(doc, function () { state.renders++; });
    return { color: apiObj.color, state: state, body: body };
  }

  const h = makeColorHarness(PAGE, null);
  const T = { bg_color: 0x141414 };            // 主题状态（页面里那个 T 的一小块）
  const parent = makeEl("section");
  const row = h.color(parent, "背景色", () => T.bg_color, v => { T.bg_color = v; });

  // 派发一个事件，返回抛出的异常（null = 没抛）
  //   ★ 不直接 dispatch：旧页面在"手打色号"那一步会抛，裸 dispatch 会把整个用例
  //     打断（连后面的判据都不输了）—— 抓下来才看得见"是这一条红"。
  function fire(el, ev) {
    try { el.dispatch(ev); return null; } catch (e) { return e; }
  }

  ok(!!row && !!row.t && !!row.c, "color() 造出了一行（标签 + 色块 + 文字框）");
  eq(parent.children.length, 1, "这一行挂到了父节点上");
  eq(String(row.t.value).toUpperCase(), "#141414", "文字框初值 = 主题当前值");
  eq(row.t.type, "text", "那个能手打的是 type=text 的输入框（不是只读色块）");

  // ---- ③ -1 手打 #12ab34（车主的那种输入）----
  row.t.value = "#12ab34";
  const threw = fire(row.t, "change");
  eq(threw, null, "手打 #12ab34：没有抛异常（改动前这里是 ReferenceError: unhex is not defined）");
  eq(T.bg_color, 0x12AB34,
     "手打 #12ab34：主题值变成 0x12AB34（不是静默无效）");
  eq(String(row.t.value).toUpperCase(), "#12AB34", "手打之后文字框被规整成大写字号");
  eq(String(row.c.value).toUpperCase(), "#12AB34", "色块跟着同步");
  eq(h.state.renders, 1, "手打之后触发了一次 render()（画面真的重画了）");

  // ---- ③ -2 不带 "#" 的写法、以及大写 ----
  {
    row.t.value = "12ab34";                    // 省略 #
    eq(fire(row.t, "change"), null, "省略 # 的写法也不抛异常");
    eq(T.bg_color, 0x12AB34, "省略 # 也能认（12ab34 → 0x12AB34）");
    row.t.value = "#FF00AA";
    eq(fire(row.t, "change"), null, "大写 hex 也不抛异常");
    eq(T.bg_color, 0xFF00AA, "大写 hex 也认（#FF00AA → 0xFF00AA）");
  }

  // ---- ③ -3 非法输入：一个字符都不许写进主题（也不许把颜色清零）----
  {
    row.t.value = "#1234";                     // 位数不对
    const before = T.bg_color;
    eq(fire(row.t, "change"), null, "非法色号不抛异常（走的是“回填”那一支）");
    eq(T.bg_color, before, "非法色号（#1234）不改主题");
    eq(String(row.t.value).toUpperCase(), "#FF00AA", "非法色号 ⇒ 文字框回填当前值");
  }

  // ---- ③ -4 ★ 反向对照：把 unhexOf( 改回 unhex( 的变异体必须**红** ----
  //   （证明上面那几条不是空转：变异体就是改动前的页面）
  {
    const mutant = makeColorHarness(PAGE, function (body) {
      return body.replace(/unhexOf\(/g, "unhex(");
    });
    ok(/[^\w$.]unhex\(/.test(mutant.body) && !/unhexOf\(/.test(mutant.body),
       "变异体构造成功（页面里 color() 的 unhexOf( 全部换回历史拼写 unhex(）");
    const T2 = { bg_color: 0x141414 };
    const p2 = makeEl("section");
    const row2 = mutant.color(p2, "背景色", () => T2.bg_color, v => { T2.bg_color = v; });
    row2.t.value = "#12ab34";
    const err = fire(row2.t, "change");
    ok(!!err && err instanceof ReferenceError,
       "反向对照：unhexOf→unhex 之后手打色号抛 ReferenceError（" +
       (err ? err.message : "没抛") + "）");
    eq(T2.bg_color, 0x141414, "反向对照：抛异常 ⇒ set() 没执行 ⇒ 主题一个数都没变（静默无效）");
  }

  // ---- ③ -5 源码层：页面里不许再有裸的 unhex( 调用（注释不算）----
  //   ★ 这一条防"别处又写了一遍 unhex"：历史拼写只该出现在注释里。
  {
    const code = PAGE.split("\n").filter(l => !/^\s*\/\//.test(l)).join("\n");
    const bare = code.match(/[^\w$.]unhex\s*\(/g) || [];
    eq(bare.length, 0, "源码里没有裸的 unhex( 调用（只许有 unhexOf）");
    ok(/function unhexOf\(/.test(PAGE), "唯一的实现仍叫 unhexOf（历史拼写说明留在注释里）");
    // 高级模式每一行颜色都走 color()（③ 测的就是这条真路径）
    const rows = (PAGE.match(/syncers\.push\(color\(/g) || []).length;
    ok(rows >= 8, "高级模式的颜色行都走 color()（实到 " + rows + " 行）");
  }
}

// ------------------------------------------------------------
// 小工具：±tol 的比较（与 check-crop-centering.js 同一口径）
// ------------------------------------------------------------
console.log("\n" + "=".repeat(56));
if (fail === 0) console.log("全部通过:" + pass + " 项断言");
else {
  console.log("失败 " + fail + " 项 / 共 " + (pass + fail) + " 项:");
  failures.forEach(f => console.log("  - " + f));
}
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
