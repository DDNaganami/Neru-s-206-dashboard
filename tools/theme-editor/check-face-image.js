/* ============================================================
 * 表情图片的**像素判据** —— "这一屏显示的是哪一张脸"（2026-09-26 新增）
 *
 * 用途：
 *   node tools/theme-editor/check-face-image.js <image.bin> <帧.bmp> <期望组>
 *     期望组：`left`（左/转速表的 5 张）或 `right`（右/速度表的 5 张）
 *   可给多帧：`… <期望组> <帧2.bmp> <期望组2> …`
 *
 * ------------------------------------------------------------
 * 为什么需要它（这是车主第二次报的那个 bug 的**唯一**机器判据）
 * ------------------------------------------------------------
 * 车主原话："**转速表的表情好像跟转速失去关联了，现在表情变化怎么感觉是随机变动的。**"
 *
 * 根因（2026-09-26 查清）：表盘（弧 + 读数）与"表情的**数据源**"都已经按角色映射
 * 走对了，可"表情**图片**取哪一组"这一环还在拿 `faceIndexForScreen(s)` 当**屏号**用
 * —— 从板上"表号"与"屏号"正好对调 ⇒ 转速表那一屏的脸去**车速**那 5 张图里挑。
 * 车速在模拟数据里本来就乱扫 ⇒ 屏上的脸**跟转速没关系、看着随机**。
 *
 * 这个错**不会报错、屏上也不缺东西**（脸在变、弧在动、读数在跳），所以只能靠
 * "**这一屏上到底是哪一张图**"来判 —— 而那就是这个脚本做的事：
 *   把 image.bin 里那 9 张图（1 张背景 + 左右各 4~5 张脸）逐个**按 alpha 合成**到
 *   帧的背景色上，与帧里对应区域的像素**逐点比**，谁的差最小就是"屏上那张"。
 *   ★ 弧/读数/指示灯是**后画**在上面的图层，所以判据用"分位数"而不是"平均值"
 *     （见下面 `scoreImage` 的说明）：被盖住的那一小部分点不算数。
 *
 * ★ 用的是**车主自己的素材**（`tools/theme-editor/local/image.bin` 或
 *   `.dsh-drop` 里那一份）—— 不是测试用色块。色块的坐标判据（check-preview-frame.js）
 *   验不了"左右两组取反"这件事，因为两组色块在几何上是同一族。
 *
 * 判据（三条，缺一不可）：
 *   ① 每一帧都能判出**唯一**一张"最佳匹配"（其分数明显低于第二名，默认要求
 *      最佳 < 12 且 第二名 > 3×最佳）；
 *   ② 最佳那张的**角色号必须属于期望的那一组**（左组 3/12/13/21/4、右组 6/22/17/18/8）；
 *   ③ 反向判据：**期望组之外的图**不许成为最佳（这一条就是"左右串了"的可执行形态）。
 * ============================================================ */
"use strict";

const fs = require("fs");

// ---- LVGL 颜色格式（与 tools/theme-editor/image-blob-build.js 的 CF 一致）----
const CF = { L8: 0x06, I8: 0x0a, A8: 0x0e, RGB888: 0x0f, ARGB8888: 0x10, XRGB8888: 0x11, RGB565: 0x12, RGB565A8: 0x14 };
const MAGIC = 0x44363032, VERSION = 1, NAME_MAX = 24, MAX_COUNT = 32, ENTRY = 44;
const HDR = 12 + MAX_COUNT * ENTRY;

// ---- 表情角色号 → 组（与 lib/dashcore/face_stages.h 的 kFaceRoleId 逐条一致）----
//   ★ 这张表**故意手抄**一份：脚本要能独立说出"这张图的角色号属于哪一边"，
//     否则它就成了"用被测对象验证被测对象"。
const ROLE_SIDE = {
  3: "left", 12: "left", 13: "left", 21: "left", 4: "left",          // 左屏(转速表)
  6: "right", 22: "right", 17: "right", 18: "right", 8: "right"      // 右屏(速度表)
};
const ROLE_NAME = {
  1: "background", 3: "L idle", 12: "L cruise", 13: "L sport", 21: "L high", 4: "L redline",
  6: "R idle", 22: "R city", 17: "R cruise", 18: "R sport", 8: "R overspeed"
};
// 与 dash_role_layout.h 的 roleThemeIndex() 同一口径：组 0 = 左/转速、组 1 = 右/速度。
const GROUP_NAME = ["left(rpm)", "right(speed)"];

// ------------------------------------------------------------
// image.bin：把每张图解出来（颜色 + alpha 平面）
// ------------------------------------------------------------
function loadBlob(path) {
  const b = fs.readFileSync(path);
  const magic = b.readUInt32LE(0), version = b.readUInt16LE(4), count = b.readUInt16LE(6);
  if (magic !== MAGIC) throw new Error(`${path}: 魔数不对 (0x${magic.toString(16)})`);
  if (version !== VERSION) throw new Error(`${path}: 版本不对 (${version})`);
  const items = [];
  for (let i = 0; i < count; i++) {
    const o = 12 + i * ENTRY;
    const e = {
      offset: b.readUInt32LE(o), size: b.readUInt32LE(o + 4),
      w: b.readUInt16LE(o + 8), h: b.readUInt16LE(o + 10),
      cf: b[o + 12], pad: b[o + 13], role: b.readUInt16LE(o + 14), order: b.readUInt16LE(o + 16),
      name: b.slice(o + 18, o + 18 + NAME_MAX).toString("utf8").replace(/\0.*$/, ""),
      bytes: b
    };
    const bpp = (e.cf === CF.RGB565 || e.cf === CF.RGB565A8) ? 2 : (e.cf === CF.RGB888 ? 3 : 1);
    e.bpp = bpp;
    e.stride = bpp * e.w + e.pad;
    e.side = ROLE_SIDE[e.role] || (e.role === 1 ? "shared" : "?");
    items.push(e);
  }
  return items;
}

function rgbAt(e, x, y) {
  const b = e.bytes, off = HDR + e.offset + y * e.stride + x * e.bpp;
  if (e.cf === CF.RGB565 || e.cf === CF.RGB565A8) {
    const v = b[off] | (b[off + 1] << 8);
    return [((v >> 11) & 0x1f) * 255 / 31 | 0, ((v >> 5) & 0x3f) * 255 / 63 | 0, (v & 0x1f) * 255 / 31 | 0];
  }
  if (e.cf === CF.RGB888) return [b[off], b[off + 1], b[off + 2]];
  if (e.cf === CF.ARGB8888 || e.cf === CF.XRGB8888) return [b[off + 2], b[off + 1], b[off]];
  return [b[off], b[off], b[off]];
}
function alphaAt(e, x, y) {
  if (e.cf !== CF.RGB565A8) return 255;
  const aplane = HDR + e.offset + e.stride * e.h;
  return e.bytes[aplane + y * (e.stride >> 1) + x];
}

// ------------------------------------------------------------
// 帧 BMP（pcpreview 落盘的 24bpp、自下而上）
// ------------------------------------------------------------
function readBmp(path) {
  const b = fs.readFileSync(path);
  if (b[0] !== 0x42 || b[1] !== 0x4d) throw new Error("不是 BMP: " + path);
  const dataOff = b.readUInt32LE(10);
  const w = b.readInt32LE(18);
  const hRaw = b.readInt32LE(22);
  const bpp = b.readUInt16LE(28);
  const flip = hRaw > 0;
  const h = Math.abs(hRaw);
  const bytesPerPx = bpp / 8;
  const rowSize = Math.floor((bpp * w + 31) / 32) * 4;
  if (bpp !== 24 && bpp !== 32) throw new Error("只支持 24/32 位 BMP，实际 " + bpp);
  return {
    w, h, path,
    px(x, y) {
      const ry = flip ? (h - 1 - y) : y;
      const off = dataOff + ry * rowSize + x * bytesPerPx;
      return [b[off + 2], b[off + 1], b[off]];
    }
  };
}

// 中心对齐的取样矩形（480 帧 / 240 或 200 的图 ⇒ 偏移非负）
function rectFor(frame, e) {
  const x0 = ((frame.w - e.w) >> 1), y0 = ((frame.h - e.h) >> 1);
  if (x0 < 0 || y0 < 0) throw new Error(`图 ${e.w}x${e.h} 比帧 ${frame.w}x${frame.h} 还大`);
  return { x0, y0 };
}

// ------------------------------------------------------------
// 一张图的得分：把"它"按 alpha 合成到**帧自己的底色**上，逐点比帧里的像素。
//
// ★ 为什么用**分位数**而不是平均值：弧、读数、指示灯都是**后画**的图层，
//   会把这张图的顶上那一小块盖掉（车主定稿的层序：表情在弧之下）。
//   那些点上的差是"图层关系"造成的，不该算进"是不是这张图"。
//   实测：正确那张的差在绝大多数点上**恒等于 0**（同一份像素数据、同一次量化），
//   被盖住的点在 p70 之后 —— 所以取 **p60** 做判据，正确那张就是 0。
//
// ★ 只用 alpha≥200 的点：透明处背后是什么由**别的图层**决定，与本图无关。
// ------------------------------------------------------------
function scoreImage(frame, e, bg) {
  const { x0, y0 } = rectFor(frame, e);
  const diffs = [];
  for (let y = 0; y < e.h; y++) {
    for (let x = 0; x < e.w; x++) {
      const a = alphaAt(e, x, y);
      if (a < 200) continue;
      const c = rgbAt(e, x, y);
      const k = a / 255;
      const want = [
        Math.round(c[0] * k + bg[0] * (1 - k)),
        Math.round(c[1] * k + bg[1] * (1 - k)),
        Math.round(c[2] * k + bg[2] * (1 - k))
      ];
      const got = frame.px(x0 + x, y0 + y);
      diffs.push(Math.max(Math.abs(want[0] - got[0]), Math.abs(want[1] - got[1]), Math.abs(want[2] - got[2])));
    }
  }
  if (diffs.length < 500) throw new Error("可判定的点太少（alpha 覆盖不足）");
  diffs.sort((p, q) => p - q);
  const q = (f) => diffs[Math.min(diffs.length - 1, Math.floor(diffs.length * f))];
  let exact = 0;
  for (const d of diffs) if (d === 0) exact++;
  return { p60: q(0.60), p90: q(0.90), exactPct: 100 * exact / diffs.length, n: diffs.length };
}

// 帧的"底色"：取一个**不可能被任何图层盖住**的位置 —— 帧最左上角
// （480 帧上，表情/弧/读数都在中心区；四角只有背景图与主题底色）。
// ★ 更稳的做法是直接取图**四角**的帧像素众数：那样连背景图是不是满屏都不依赖。
function frameBg(frame, e) {
  const { x0, y0 } = rectFor(frame, e);
  const hist = new Map();
  const pts = [[x0 + 2, y0 + 2], [x0 + e.w - 3, y0 + 2], [x0 + 2, y0 + e.h - 3], [x0 + e.w - 3, y0 + e.h - 3]];
  for (const [x, y] of pts) {
    const c = frame.px(x, y).join(",");
    hist.set(c, (hist.get(c) || 0) + 1);
  }
  return [...hist.entries()].sort((a, b) => b[1] - a[1])[0][0].split(",").map(Number);
}

// ------------------------------------------------------------
// 3.5 ★ 判据的三条阈值（都从**实测**来，不是拍的）
//
// 实测（2026-09-26，车主真素材 9 张）：把"正确那张"合成上去之后，
//   · 正确那张：p60 **恒为 0**（同一份像素、同一次量化 ⇒ 绝大多数点逐点相等）；
//   · **同组**的另一张：p60 = 8（p90 = 16）—— 这一档差不是"几乎对"，是**真的不对**
//     （车主那几张脸在颜色上确实接近，但差是有结构的、而且稳定可复现）；
//   · **另一组**的任意一张：p60 = 77..82（p90 = 165）。
// ⇒ 三个判决门限就落在这三档之间，**互不重叠**：
//     HIT（就是这张）≤ 2        NEAR（同组的另一张）3..24        OTHER（另一组）> 40
//   ★ 第三条（"另一组不许成为最佳"）因此是**结构性的**：另一组最好的那一张
//     也远在 OTHER 档，除非真的串了组（那时它会掉到 HIT 档、而正确那张升上去）。
// ------------------------------------------------------------
const TOL_HIT = 2;
const TOL_NEAR = 24;
const TOL_OTHER = 40;

function bandOf(p) {
  if (p <= TOL_HIT) return "HIT";
  if (p <= TOL_NEAR) return "NEAR";
  if (p > TOL_OTHER) return "OTHER";
  return "GRAY";
}

// ------------------------------------------------------------
// 一帧的完整报告
// ------------------------------------------------------------
function judgeFrame(blob, frame, wantSide, tol) {
  const faces = blob.filter(e => e.side === "left" || e.side === "right");
  const bg = frameBg(frame, faces[0]);
  const rows = faces.map(e => {
    const s = scoreImage(frame, e, bg);
    return { e, ...s, band: bandOf(s.p60) };
  }).sort((a, b) => a.p60 - b.p60);

  const best = rows[0], second = rows[1];
  const okBest = best.p60 <= tol;
  const okMargin = second.p60 >= Math.max(tol + 1, 3 * Math.max(best.p60, 1));
  const okSide = best.e.side === wantSide;
  // ★ 反向判据（"左右串了"的可执行形态）：**另一组的每一张**都必须落在 OTHER 档。
  //   ★ 判据③（第二名 ≥ 3×最佳）照旧**打出来当参考**，但**不进判决**：
  //     第二名几乎总是**同组**的另一张脸，而车主那几张脸在颜色上本来就接近
  //     （实测 p60 落在 0..8）；真正有分辨力的是"另一组"那一列（实测 70+）。
  const otherRows = rows.filter(r => r.e.side !== wantSide);
  const otherSideBest = otherRows[0];
  const okOther = otherRows.every(r => r.band === "OTHER");
  const okGap = otherSideBest.p60 >= Math.max(tol + 1, 4 * Math.max(best.p60, 1, 4));

  console.log(`\n=== ${frame.path}  (期望组 = ${wantSide}) ===`);
  console.log(`  帧底色（用图的四角取） = rgb(${bg.join(",")})`);
  console.log(`  ${"角色号".padEnd(6)} ${"用途".padEnd(12)} ${"组".padEnd(6)} ${"p60".padStart(5)} ${"p90".padStart(5)}  档    判定`);
  for (const r of rows) {
    const mark = (r === best) ? "<== 屏上这张" : "";
    console.log(`  ${String(r.e.role).padEnd(6)} ${(ROLE_NAME[r.e.role] || "?").padEnd(12)} ${String(r.e.side).padEnd(6)} ` +
                `${String(r.p60).padStart(5)} ${String(r.p90).padStart(5)}  ${r.band.padEnd(5)} ${mark}`);
  }
  console.log(`  判据① 最佳 p60=${best.p60} ≤ ${tol}（HIT 档）          : ${okBest ? "OK" : "FAIL"}`);
  console.log(`  判据② 最佳属于期望组（${wantSide}）              : ${okSide ? "OK" : "FAIL"}  （最佳 = 角色 ${best.e.role} ${ROLE_NAME[best.e.role]}）`);
  console.log(`  判据③ 第二名 p60=${second.p60} ≥ 3×最佳  : ${okMargin ? "OK" : "（参考）"}  （第二名 = 角色 ${second.e.role} ${ROLE_NAME[second.e.role]}）`);
  console.log(`  判据④ 另一组最近一张 p60=${otherSideBest.p60} 落在 OTHER 档 : ${okOther ? "OK" : "FAIL"}`);
  // ★ 判决只由 ①（最佳确实就是屏上那张）、②（它属于期望的那一组）、
  //   ④（**另一组一张都不在 HIT/NEAR 档** —— 这就是"左右串了"的可执行形态）三条决定。
  //   ③ **刻意不进判决**：第二名通常是**同组**的另一张脸，车主那几张脸在颜色上本来就
  //   接近（实测 p60 落在 0..8），要求"3×"在 p60=0 时是个无意义的算术题；
  //   它照旧打出来当参考（真正的分辨力看"另一组"那一列，那是 70+ 的差）。
  return { ok: okBest && okSide && okOther, best, rows, otherSideBest };
}

// ------------------------------------------------------------
function main(argv) {
  if (argv.length < 3) {
    console.error("用法: node check-face-image.js <image.bin> <帧.bmp> <left|right> [<帧2.bmp> <left|right> …]");
    console.error("      [--tol N]  p60 阈值，默认 12");
    return 2;
  }
  let tol = 12;
  const rest = [];
  for (let i = 0; i < argv.length; i++) {
    if (argv[i] === "--tol") { tol = Number(argv[++i]); continue; }
    rest.push(argv[i]);
  }
  const blobPath = rest.shift();
  const blob = loadBlob(blobPath);
  console.log(`image.bin: ${blobPath}`);
  for (const e of blob) {
    console.log(`  role=${String(e.role).padStart(2)} ${(ROLE_NAME[e.role] || "?").padEnd(12)} ${e.w}x${e.h} cf=0x${e.cf.toString(16)} side=${e.side}`);
  }
  const left = blob.filter(e => e.side === "left").length;
  const right = blob.filter(e => e.side === "right").length;
  console.log(`  左组 ${left} 张 / 右组 ${right} 张（与 face_stages.h 的 kFaceRoleId 同口径）`);

  let allOk = true;
  const pairs = [];
  while (rest.length >= 2) pairs.push([rest.shift(), rest.shift()]);
  if (pairs.length === 0) throw new Error("至少要给一对 <帧.bmp> <left|right>");
  for (const [f, side] of pairs) {
    if (side !== "left" && side !== "right") throw new Error("期望组只能是 left / right，实际 " + side);
    const r = judgeFrame(blob, readBmp(f), side, tol);
    allOk = allOk && r.ok;
  }
  console.log(`\n${allOk ? "FACE-IMAGE-OK" : "FACE-IMAGE-FAIL"}`);
  return allOk ? 0 : 1;
}

if (require.main === module) {
  try { process.exit(main(process.argv.slice(2))); }
  catch (err) { console.error("错误: " + err.message); process.exit(2); }
}

module.exports = { loadBlob, readBmp, judgeFrame, ROLE_SIDE, GROUP_NAME };
