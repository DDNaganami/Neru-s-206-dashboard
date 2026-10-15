/* ============================================================
 * check-crop-centering.js —— 「框选 → 进槽 → 打出来的像素到底居中不居中」
 *
 *   node tools/theme-editor/check-crop-centering.js
 *   退出码：0 = 居中/取景两条都达标；1 = 有判据红
 *
 * ------------------------------------------------------------
 * 为什么要有它（2026-10-15 车主实测两条反馈）
 * ------------------------------------------------------------
 * ① 「表情图片现在导入位置好像不够居中」
 * ② 「主题背景图这边导入没有框选选项」
 *
 * 这两条都**只能量像素**才能回答 —— "看着挺正"不是判据，而且这个仓库里
 * 没有浏览器（页面要能离线双击打开，不能引 jsdom/canvas 这类依赖）。
 * 所以本模块带了一个**最小 canvas 替身**（canvas-shim.js：只实现会进像素的
 * 那几个方法），把页面里**真源码**抠出来跑：
 *
 *      cropRenderSquare()（页面里的框选产物）
 *        → renderToTarget() 的 contain / cover 分支（**页面里的真函数**）
 *          → rgbaToRgb565A8()（内核里的真函数）
 *            → 量**非透明像素包围盒的中心**落在哪
 *
 * ★ "抠出来跑"是刻意的：抄一份逻辑就只是测复制品（页面改了它还是绿的）。
 *   抠的还是 `new Function` 求值出来的同一段源码 —— 与 test-theme-json.js /
 *   test-image-blob-build.js 的既有做法一致。
 *
 * ★ 判据用的是**包围盒中心**（不是质心）：车主看到的是"整块图形在画布上的
 *   位置"，而质心会被抗锯齿边缘的灰度拉偏。容差 ±1 px（像素栅格上
 *   "完全居中"与"差一格"的分界）。
 * ★ canvas 替身的采样是**覆盖率加权盒式平均**（不是浏览器的双线性），
 *   所以它**不能**用来判"画得好不好看"；它只判**几何**：东西落在哪、有没有被平移。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");
const SH = require(path.join(__dirname, "canvas-shim.js"));

const dir = __dirname;
const PAGE = fs.readFileSync(path.join(dir, "index.html"), "utf8");
const IB = require(path.join(dir, "image-blob-build.js"));

let pass = 0, fail = 0;
const failures = [];
function ok(cond, what) {
  if (cond) pass++;
  else { fail++; failures.push(what); console.log("  ✗ " + what); }
}
function eq(a, b, what) {
  ok(a === b, what + "  (期望 " + JSON.stringify(b) + "，得到 " + JSON.stringify(a) + ")");
}
function section(t) { console.log("\n== " + t); }
function near(a, b, tol, what) {
  ok(Math.abs(a - b) <= tol, what + "  (" + a.toFixed(2) + " vs " + b.toFixed(2) +
                               "，容差 ±" + tol + ")");
}

// ------------------------------------------------------------
// 从页面里抠出真源码（不抄一份）
// ------------------------------------------------------------
function grabFn(name) {
  const re = new RegExp("function " + name + "\\s*\\([^)]*\\)\\s*\\{[\\s\\S]*?\\n\\}", "m");
  const m = re.exec(PAGE);
  if (!m) throw new Error("index.html 里找不到函数 " + name);
  return m[0];
}
function grabConst(name) {
  const re = new RegExp("const " + name + "\\s*=\\s*[^;]+;", "m");
  const m = re.exec(PAGE);
  if (!m) throw new Error("index.html 里找不到常量 " + name);
  return m[0];
}

// 页面里这两个要用的"环境"
const doc = SH.makeDocument();
const curTarget = "s3";
const FACE_ROLES = (function () {
  const FS = require(path.join(dir, "face-stages.js"));
  const s = new Set();
  FS.SCREENS.forEach(sc => sc.states.forEach(x => s.add(x.role)));
  return s;
})();

// ---- 框选那一半（页面里的真函数：cropFit / cropSideOut / cropRenderSquare）----
// ★ subjectBox / cropFit / cropSideOut 全从页面源码里抠出来 ——
//   抄一份逻辑就只是测复制品（页面改了它还是绿的）。
//   CROP_FILL 这类常量也一起抠（它是几何的一部分，漏了会 ReferenceError）。
const cropApi = new Function(
  "document", "ImageBlob", "curTarget", "window",
  [
    grabConst("CROP_FILL"),
    grabFn("cropStageSide"),
    grabFn("applyCropStageSize"),
    grabFn("bgSideFor"),
    grabFn("subjectBox"),
    grabFn("cropSideMax"),
    grabFn("cropSideOut"),
    grabFn("cropFit"),
    grabFn("cropRenderSquare"),
    "return { cropFit: cropFit, cropSideOut: cropSideOut, cropSideMax: cropSideMax, " +
    "cropStageSide: cropStageSide, cropRenderSquare: cropRenderSquare, " +
    "subjectBox: subjectBox, CROP_FILL: CROP_FILL, " +
    "set: function (s) { cropState = s; } };"
  ].join("\n").replace(/\bvar cropState = null;/, "")
)(doc, IB, curTarget, { innerHeight: 1040 });

// ---- 进槽那一半（renderToTarget / cfForRole / wantAlpha，全是页面里的真函数）----
// ★ FACE_ROLES 在页面里是个 IIFE（依赖 FaceStages）；这里**照样用真的
//   face-stages.js 算一遍**，不手抄角色号。
// ★ renderToTarget 会用到 themeBgHex()（背景铺底那一条）与 theme.bg，
//   两个都给它 —— 缺一个就是 ReferenceError，而不是"判据失败"。
const slotApi = new Function(
  "document", "ImageBlob", "curTarget", "FACE_ROLES", "theme", "T",
  [
    grabFn("hexOf"),
    grabFn("themeBgHex"),
    grabFn("wantAlpha"),
    grabFn("cfForRole"),
    grabFn("renderToTarget"),
    "return { renderToTarget: renderToTarget, cfForRole: cfForRole, wantAlpha: wantAlpha };"
  ].join("\n")
)(doc, IB, curTarget, FACE_ROLES, { bg: "#141414" }, { bg_color: 0x141414 });

// ------------------------------------------------------------
// 造图：**明显偏心**的主体（圆心不在图片中心）
// ------------------------------------------------------------
function offCenterSubject(w, h, cx, cy, r) {
  return SH.makeImage(w, h, (p) => {
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        if (Math.hypot(x - cx, y - cy) <= r) p.put(x, y, 220, 40, 40, 255);
      }
    }
  });
}

// 打开框选器时的初始状态 = **页面里 cropFit() 的返回值**（不在这里重写一份算术）
function openState(img, mode) {
  const fit = cropApi.cropFit(img, mode);
  return { img: img, name: "probe.png", slot: null, mode: mode,
           W: fit.W, H: fit.H, sw: fit.sw, sh: fit.sh, F: fit.F,
           k: fit.k, kCover: fit.kCover, kNeed: fit.kNeed,
           ix: fit.ix, iy: fit.iy };
}

// ------------------------------------------------------------
section("① 表情：偏心主体框选之后，**产物**的非透明包围盒中心落在画布正中 ±1 px");
{
  // 源图：360×360，主体（圆盘 r=60）中心在 (130,120) —— 明显偏左偏上
  const SW = 360, SHH = 360;
  const img = offCenterSubject(SW, SHH, 130, 120, 60);

  // 打出来的"进槽前像素"（表情走 contain）—— 与页面里 renderToTarget 的调用一致
  const sizes = [320, 300, 296, 152, 128];
  for (const target of sizes) {
    const st = openState(img, "face");
    cropApi.set(st);
    const sq = cropApi.cropRenderSquare();
    ok(!!sq, "目标 " + target + "：框选产物拿得到");
    if (!sq) continue;
    eq(sq.width, sq.height, "目标 " + target + "：框选产物是方图");
    ok(sq.width % 4 === 0, "目标 " + target + "：框选产物边长对齐到 4（" + sq.width + "）");

    // 进槽：**页面里那一句** renderToTarget(item, item.w, item.h, alpha)
    const rgba = slotApi.renderToTarget(
      { img: sq, fit: "contain", trim: false }, target, target, true);
    ok(!!rgba, "目标 " + target + "：renderToTarget 有像素");
    if (!rgba) continue;

    const bb = SH.opaqueBBox({ width: target, height: target, _rgba: rgba });
    ok(!!bb, "目标 " + target + "：产物里有不透明像素");
    if (!bb) continue;
    // 画布像素中心 = (target-1)/2（像素栅格上第 0..target-1 格的正中）
    const mid = (target - 1) / 2;
    near(bb.cx, mid, 1, "目标 " + target + "：产物**水平**居中（包围盒中心 vs 画布中心）");
    near(bb.cy, mid, 1, "目标 " + target + "：产物**垂直**居中（包围盒中心 vs 画布中心）");
  }

  // ★ 反向对照：**不做居中处理**会偏多少 —— 证明这条断言不是空转
  {
    const st = openState(img, "face");
    // 故意把方框从"对准主体"挪回"图片中心"（老行为）
    const fx = (st.W - st.F) / 2, fy = (st.H - st.F) / 2;
    st.ix = fx + st.F / 2 - st.sw / 2 * st.k;
    st.iy = fy + st.F / 2 - st.sh / 2 * st.k;
    const w = st.sw * st.k, h = st.sh * st.k;
    st.ix = Math.min(fx, Math.max(fx + st.F - w, st.ix));
    st.iy = Math.min(fy, Math.max(fy + st.F - h, st.iy));
    cropApi.set(st);
    const sq2 = cropApi.cropRenderSquare();
    const rgba2 = slotApi.renderToTarget({ img: sq2, fit: "contain", trim: false }, 300, 300, true);
    const bb2 = SH.opaqueBBox({ width: 300, height: 300, _rgba: rgba2 });
    const off = Math.abs(bb2.cx - 149.5) + Math.abs(bb2.cy - 149.5);
    ok(off > 2, "反向对照：把方框对回图片中心会明显偏（实测总偏移 " + off.toFixed(1) +
                " px）⇒ 上面那条断言不是空转");
  }
}

// ------------------------------------------------------------
section("② 背景：取景框（cover 语义）—— 框内就是最终画面，且**铺满、不露底**");
{
  // 源图：**横构图照片** 1280×960（背景素材的常见尺寸），主体偏左上
  const SW = 1280, SHH = 960;
  const img = offCenterSubject(SW, SHH, 420, 380, 220);
  const st = openState(img, "bg");
  cropApi.set(st);

  // ① 盖满：初始缩放必须 ≥ cover ⇒ 方框在源图里装得下 ⇒ 框内不会露底
  ok(st.k >= st.kCover - 1e-9,
     "背景初始缩放 ≥ cover（" + st.k.toFixed(3) + " ≥ " + st.kCover.toFixed(3) + "）");
  const coverW = st.sw * st.k, coverH = st.sh * st.k;
  ok(coverW >= st.F - 1e-9 && coverH >= st.F - 1e-9,
     "方框被图完整盖住（图 " + coverW.toFixed(1) + "×" + coverH.toFixed(1) +
     " ≥ 框 " + st.F + "）");
  const fx = (st.W - st.F) / 2, fy = (st.H - st.F) / 2;
  ok(st.ix <= fx + 1e-6 && st.ix + coverW >= fx + st.F - 1e-6 &&
     st.iy <= fy + 1e-6 && st.iy + coverH >= fy + st.F - 1e-6,
     "初始位置**没有露出图片边界**（框整个落在图里）");
  // ② 输出分辨率：框内那一片要有整屏那么多源像素（480），否则就是把小图放大
  ok(st.F / st.k >= 480 - 1e-6,
     "框内源图 ≥ 480 像素（实测 " + (st.F / st.k).toFixed(1) + "）⇒ 背景不会被放大");

  const sq = cropApi.cropRenderSquare();
  eq(sq.width, sq.height, "背景取景产物是方图");
  eq(sq.width, 480, "背景取景产物边长 = 480（整屏口径）");
  // ★ 取景产物里**不该有"我从没画过"的地方**：这一段用一张**整幅不透明的照片**
  //   做夹具（背景素材就是这样的），逐像素确认没有透明洞
  //   （用透明底的夹具去断言"无露底"是错的 —— 那 17% 的透明是素材本来就有的）。
  const photo = SH.makeImage(1280, 960, (p) => {
    for (let y = 0; y < 960; y++) {
      for (let x = 0; x < 1280; x++) {
        const inSubject = Math.hypot(x - 420, y - 380) <= 220;
        p.put(x, y, inSubject ? 220 : 30, inSubject ? 60 : 34, inSubject ? 60 : 40, 255);
      }
    }
  });
  const stP = openState(photo, "bg");
  cropApi.set(stP);
  const sqP = cropApi.cropRenderSquare();
  eq(sqP.width, 480, "整幅不透明的照片：取景产物仍是 480");
  const bbP = SH.opaqueBBox(sqP, 250);
  ok(bbP && bbP.w === sqP.width && bbP.h === sqP.height,
     "取景产物整幅不透明（无露底）：" +
     (bbP ? (100 * bbP.w * bbP.h / (sqP.width * sqP.height)).toFixed(1) : "0") + "%");

  // ③ 进槽：走**既有** renderToTarget 的 cover 分支 → 480×480 铺满
  const rgba = slotApi.renderToTarget({ img: sq, fit: "cover", trim: false }, 480, 480, false);
  ok(!!rgba, "背景 renderToTarget(cover) 有像素");
  const bb2 = SH.opaqueBBox({ width: 480, height: 480, _rgba: rgba }, 250);
  ok(bb2 && bb2.w === 480 && bb2.h === 480,
     "打出来的背景铺满 480×480（不透明区 " + (bb2 ? bb2.w + "×" + bb2.h : "无") + "）");

  // ④ 主体应当**大致**落在画面正中（取景时对准了主体）—— 容差放宽到 ±2：
  //    背景是"取景"，用户还可能手动挪；这条只防"明显跑偏"。
  const mid = 479 / 2;
  near(bb2.cx, mid, 2, "背景里的主体大致居中（水平）");
  near(bb2.cy, mid, 2, "背景里的主体大致居中（垂直）");

  // ⑤ 小图不放大：360×360 的底图取景后仍是 360 级，**不会**被撑成 480
  {
    const small = offCenterSubject(360, 360, 180, 170, 150);
    const st2 = openState(small, "bg");
    cropApi.set(st2);
    const sq2 = cropApi.cropRenderSquare();
    ok(sq2.width <= 368, "360 的底图取景后不放大（实测 " + sq2.width + " ≤ 368）");
    ok(sq2.width >= 320, "……但也不至于缩水太多（实测 " + sq2.width + "）");
  }

  // ⑥ 走内核编码：背景 = RGB565（2 字节/像素），脸 = RGB565A8（3 字节/像素）
  eq(slotApi.cfForRole(IB.ROLE.Background), IB.CF.RGB565, "背景编成 RGB565");
  eq(slotApi.cfForRole(IB.ROLE.FaceIdle), IB.CF.RGB565A8, "表情编成 RGB565A8");
  const bg565 = IB.rgbaToRgb565(rgba, 480, 480);
  eq(bg565.length, 480 * 480 * 2, "背景 RGB565 字节数 = 480×480×2");
}

// ------------------------------------------------------------
section("③ 两条路的语义没有互相污染（表情不铺底 / 背景不用 alpha）");
{
  const src = PAGE;
  // 表情那条路必须**不**要求 alpha 之外的改动：cfForRole 给 RGB565A8
  ok(/fit:\s*"cover"/.test(src), "背景那条路用 cover 进槽（页面里有 fit: \"cover\"）");
  ok(/fit:\s*"contain"/.test(src), "表情那条路仍用 contain 进槽");
  // 背景**不**用 AssetSpec.prepareAsset(trim)（那是表情"裁透明边"的语义）
  const bgBranch = /mode === "bg"[\s\S]{0,1400}?return;/.exec(src);
  ok(!!bgBranch, "找得到 cropConfirm() 里背景那一支");
  if (bgBranch) {
    ok(bgBranch[0].indexOf("prepareAsset") < 0,
       "背景那一支不碰「裁透明边」（那是表情的语义）");
    ok(bgBranch[0].indexOf("transparent") < 0,
       "背景那一支不要求透明通道（可用 JPEG）");
  }
  // 框选器只有一份（合并之后不许再有第二份实现）
  eq((src.match(/function cropRenderSquare\(/g) || []).length, 1,
     "框选器只有一份实现");
  eq((src.match(/function openCropper\(/g) || []).length, 1,
     "openCropper() 只有一份");
}

console.log("\n" + "=".repeat(56));
if (fail === 0) console.log("全部通过:" + pass + " 项断言");
else {
  console.log("失败 " + fail + " 项 / 共 " + (pass + fail) + " 项:");
  failures.forEach(f => console.log("  - " + f));
}
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
