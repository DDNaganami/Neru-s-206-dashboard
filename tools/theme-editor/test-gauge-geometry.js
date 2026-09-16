/* ============================================================
 * test-gauge-geometry.js —— 表盘朝向:预览 == 固件(Node 运行)
 *
 *   node tools/theme-editor/test-gauge-geometry.js
 *
 * 为什么值得单独一条:
 *   "角度约定"这种东西错了**不会报错**，只会让表盘看着不对劲 ——
 *   而"看着不对劲"是最难自查的一类问题（没有基准可对比）。
 *   这里实测踩过一次:两个编辑器的 canvas 角度换算多减了 90°
 *   （`(d - 90)`），于是预览里的表盘整体被**逆时针转了 90°**:
 *   主题里 135° 起步(7:30)画到了 4:30，缺口从正下方跑到正右方。
 *   固件一直是对的(有落帧实测:135° 处开始点亮、60°~120° 是缺口)。
 *   用户一眼看出来:"表的方向是否需要向右旋转 90 度" —— 正是这个偏移。
 *
 * 三条断言:
 *   1. 两个编辑器的角度换算**没有任何偏移**（LVGL 与 canvas 的约定本来就一样）
 *   2. 默认主题的满量程弧:缺口在**正下方**(这是汽车仪表的惯例)
 *   3. 起点落在左下(135°)，值顺时针增长 —— 与固件落帧一致
 *
 * 固件那一侧由 check-preview-frame.js 的"表盘朝向"检查兜着(读真实 BMP)。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");

const TJ = require("./theme-json.js");

let pass = 0, fail = 0;
const failures = [];

function ok(cond, what) {
  if (cond) pass++;
  else { fail++; failures.push(what); console.log("  ✗ " + what); }
}
function eq(a, b, what) {
  ok(a === b, what + "  (期望 " + JSON.stringify(b) + ",得到 " + JSON.stringify(a) + ")");
}
function section(t) { console.log("\n== " + t); }

// 去掉注释再断言。
// ★ 这一步是必需的:注释里常常**引用**那些不该出现的写法
//   ("曾经写成 (d - 90)"、"要画在 r-w/2 上"),照原文匹配就会假通过 ——
//   实测踩过:把 `r - w / 2` 改回 `r` 之后,断言仍然被注释里的 "r-w/2" 满足了。
// 只处理行注释与块注释,不处理字符串里的 //(本文件要检查的代码里没有)。
function stripComments(src) {
  return src.replace(/\/\*[\s\S]*?\*\//g, "").replace(/\/\/[^\n]*/g, "");
}

// LVGL 的约定(见 lv_arc.c):x = r·sin(deg+90), y = r·sin(deg)  → 0°=3点钟、顺时针
function pointAt(deg, r, cx = 240, cy = 240) {
  const rad = deg * Math.PI / 180;
  return { x: cx + r * Math.cos(rad), y: cy + r * Math.sin(rad) };
}
// 屏幕方位名(只用于把失败信息说得让人看懂)
function clockName(deg) {
  const names = [[0, "3点钟(右)"], [45, "4:30(右下)"], [90, "6点钟(正下方)"],
                 [135, "7:30(左下)"], [180, "9点钟(左)"], [225, "10:30(左上)"],
                 [270, "12点钟(正上方)"], [315, "1:30(右上)"]];
  let best = names[0], bestD = 999;
  for (const [d, n] of names) {
    const diff = Math.min(Math.abs(deg - d), 360 - Math.abs(deg - d));
    if (diff < bestD) { bestD = diff; best = [d, n]; }
  }
  return best[1] + "(≈" + Math.round(deg) + "°)";
}

// ------------------------------------------------------------
section("两个编辑器的角度换算没有偏移(LVGL 与 canvas 约定一致)");
{
  for (const f of ["index.html", "image-editor.html"]) {
    const src = fs.readFileSync(path.join(__dirname, f), "utf8");
    // 找到角度换算函数本体(两个页面各写了一份)
    const fn = /function degToRad\(d\)\s*\{\s*return([^}]*)\}/.exec(src);
    ok(!!fn, f + " 里有 degToRad()");
    if (fn) {
      const body = fn[1].replace(/\s+/g, " ");
      ok(/Math\.PI\s*\/\s*180/.test(body), f + " 的换算是 角度→弧度");
      ok(!/[+-]\s*90/.test(body),
         f + " 的角度换算**不能**有 ±90 偏移(有的话预览会整体转 90°):" + body);
    }
    // 也不许在别处偷偷做偏移(比如 drawArc 里再减一次)
    const bad = /\(\s*[a-zA-Z_$][\w$]*\s*[-+]\s*90\s*\)\s*\*\s*Math\.PI/.exec(src);
    ok(!bad, f + " 里没有别处再做 ±90 偏移");
    // 确保 degToRad 真的被用来画弧(别是定义了却没用,那样测试就是空转)。
    // 两个页面的写法不同:index.html 直接调 degToRad();image-editor.html 在
    // drawArc 里写成 `const rad = degToRad;` 再调 rad()。所以查两件事:
    //   ① 文件里至少引用 degToRad 两次(定义 + 使用)
    //   ② drawArc 内部确实用到了角度换算
    const refs = (src.match(/degToRad/g) || []).length;
    ok(refs >= 2, f + " 里 degToRad 至少被引用 2 次(定义+使用,实际 " + refs + ")");
    const drawArcBody = /function drawArc\([^)]*\)\s*\{([\s\S]*?)\n\}/.exec(src);
    ok(!!drawArcBody, f + " 里有 drawArc()");
    if (drawArcBody) {
      const body = stripComments(drawArcBody[1]);   // 注释里会引用这些写法,必须去掉
      ok(/degToRad|rad\(/.test(body),
         f + " 的 drawArc() 里用了角度换算(没有的话就是空转)");
      ok(/ctx\.arc\(/.test(body), f + " 的 drawArc() 真的画了圆弧");
      // ★ 弧带要画在 r - w/2 上:LVGL 的外沿在 radius、带宽往里长,
      //   而 canvas 描边以路径为中线 —— 不减 w/2 就整体外移半个带宽
      //   (实测固件:radius=205/width=24 → 182..205;不减的话预览画在 193..217,
      //    差 12 像素,足以把"表情压没压到弧"看错)。
      ok(/-\s*w\s*\/\s*2/.test(body),
         f + " 的 drawArc() 要把描边半径取 r - w/2(与 LVGL 的外沿语义一致)");
      ok(!/ctx\.arc\([^)]*,\s*r\s*,/.test(body),
         f + " 的 drawArc() 不该直接用 r 描边(那会把弧带画到盒子外面去)");
      // ★ 预览必须认 reverse(镜像)—— 不然固件镜像了、预览没镜像,
      //   用户看到的还是反的(这一轮他报的就是"涨幅方向反了")。
      ok(/reverse/.test(body), f + " 的 drawArc() 要认 reverse(涨幅方向/镜像)");
      const sig = /function drawArc\(([^)]*)\)/.exec(src);
      ok(sig && /reverse/.test(sig[1]), f + " 的 drawArc() 签名要带 reverse 参数");
      ok(/a\.reverse|\.reverse\b/.test(src), f + " 调用 drawArc 时要传 reverse");
    }
  }
}

// ------------------------------------------------------------
section("默认主题:满量程弧的缺口在正下方");
{
  const t = TJ.themeObject(TJ.parseThemeJson(
    fs.readFileSync(path.join(__dirname, "theme-default.json"), "utf8")));
  const arcs = [];
  for (let s = 0; s < 2; s++) {
    (t.screens[s].arcs || []).forEach((a, k) => arcs.push({ s, k, a }));
  }
  ok(arcs.length >= 3, "默认主题里有 3 条弧(实际 " + arcs.length + ")");

  for (const { s, k, a } of arcs) {
    const span = a.end_deg - a.start_deg;
    const tag = "第" + s + "屏 #" + k + "(kind=" + a.kind + ")";
    // 满量程弧(≥240°)的缺口必须朝下 —— 汽车仪表都是这样:
    // 起点在左下、顺时针经过上方、终点在右下,下方留出缺口给指针轴/里程
    if (span >= 240) {
      const gapCenter = (((a.end_deg + a.start_deg + 360) / 2) % 360);
      const diff = Math.min(Math.abs(gapCenter - 90), 360 - Math.abs(gapCenter - 90));
      ok(diff <= 20, tag + " 缺口应朝正下方:实测缺口中心 " + clockName(gapCenter));
      // 起点必须在左下象限(90°~180°)
      ok(a.start_deg > 90 && a.start_deg < 180,
         tag + " 起点应在左下:" + clockName(a.start_deg));
      // 顺时针增长:end 必须大于 start(负角度或 end<start 都是写错了)
      ok(a.end_deg > a.start_deg, tag + " end 必须大于 start(顺时针增长)");
    }
    // 内圈弧半径必须小于外圈(否则两条弧会叠在一起)
    if (k > 0) {
      const outer = t.screens[s].arcs[0].radius;
      ok(a.radius < outer, tag + " 内圈半径要小于外圈(" + a.radius + " < " + outer + ")");
    }
  }

  // 起始角对应的屏幕坐标必须在左半边、下半边(用真实几何算一遍,不靠肉眼看数字)
  {
    const outer = arcs.find(x => (x.a.end_deg - x.a.start_deg) >= 240);
    const p = pointAt(outer.a.start_deg, outer.a.radius);
    ok(p.x < 240 && p.y > 240,
       "起点坐标该落在大盘左下:" + clockName(outer.a.start_deg) +
       " → (" + Math.round(p.x) + "," + Math.round(p.y) + ")");
    const gapMid = pointAt(90, outer.a.radius);
    ok(gapMid.y > 240 && Math.abs(gapMid.x - 240) < 1,
       "缺口中心(90°)该在正下方:(" + Math.round(gapMid.x) + "," + Math.round(gapMid.y) + ")");
  }
}

// ------------------------------------------------------------
// ★ 水温弧(内圈)的开口在**正上方** —— 与转速弧刻意相反
//
// 用户提的:"水温表可以做成上方开口吗，跟转速表同向感觉有点怪"。
// 于是外圈拱在上面(缺口朝下)、内圈兜在下面(缺口朝上),一眼分得清。
// 这条是**产品决定**,不是实现细节,所以钉住:
// 谁要把水温弧改回和转速表同向,得先想清楚为什么。
section("水温弧:缺口在正上方(与转速弧相反)");
{
  const t = TJ.themeObject(TJ.parseThemeJson(
    fs.readFileSync(path.join(__dirname, "theme-default.json"), "utf8")));
  const coolantArcs = [];
  for (let s = 0; s < 2; s++) {
    (t.screens[s].arcs || []).forEach((a, k) => {
      if (a.kind === 2) coolantArcs.push({ s, k, a });   // kind 2 = 水温
    });
  }
  eq(coolantArcs.length, 1, "默认主题里只有一条水温弧(在转速表上)");
  const a = coolantArcs[0].a;
  const span = a.end_deg - a.start_deg;
  eq(span, 180, "水温弧是下半圆(跨度 180°)");
  eq(a.start_deg, 0, "从 3 点钟开始");
  eq(a.end_deg, 180, "顺时针绕到 9 点钟");

  // 缺口 = 覆盖区的补集,中心应在正上方(270°)
  const gapCenter = ((a.end_deg + a.start_deg + 360) / 2) % 360;
  const diff = Math.min(Math.abs(gapCenter - 270), 360 - Math.abs(gapCenter - 270));
  ok(diff <= 20, "水温弧缺口应朝正上方:实测缺口中心 " + clockName(gapCenter));

  // 用真实几何复核:弧的中点(值填满时最亮的地方)在正下方,缺口中心在正上方
  const midDeg = a.start_deg + span / 2;
  const mid = pointAt(midDeg, a.radius);
  ok(Math.abs(mid.x - 240) < 1 && mid.y > 240,
     "水温弧的中点该在正下方:(" + Math.round(mid.x) + "," + Math.round(mid.y) + ")");
  const gapPt = pointAt(270, a.radius);
  ok(Math.abs(gapPt.x - 240) < 1 && gapPt.y < 240,
     "水温弧缺口中心该在正上方:(" + Math.round(gapPt.x) + "," + Math.round(gapPt.y) + ")");

  // 与转速弧必须**反向**:一个缺口朝下、一个朝上(这正是"不同向")
  const outer = t.screens[0].arcs[0];
  const outerGap = ((outer.end_deg + outer.start_deg + 360) / 2) % 360;
  const outerDiff = Math.min(Math.abs(outerGap - 90), 360 - Math.abs(outerGap - 90));
  ok(outerDiff <= 20, "转速弧缺口朝正下方(实测 " + clockName(outerGap) + ")");
  ok(Math.abs(gapCenter - outerGap - 180) < 40 || Math.abs(gapCenter - outerGap + 180) < 40,
     "两条弧的缺口方向应当相反(现在 " + Math.round(outerGap) + "° vs " + Math.round(gapCenter) + "°)");

  // ★ 涨幅方向:水温弧必须是 reverse=1(从左端起涨),转速弧是 0
  eq(a.reverse, 1, "水温弧要镜像(从左端 9 点钟起涨)");
  eq(outer.reverse || 0, 0, "转速弧保持默认方向(从 start 端起涨)");

  // 镜像的几何含义:点亮区从 end 端往 start 端长。
  // 温度偏低时(比如 85℃ → t≈0.36)点亮区该落在**左半边**,
  // 而不是右半边 —— 这一条把"镜像到底镜像了什么"说清楚。
  const tCold = (85 - 60) / (130 - 60);
  const litFrom = a.end_deg - span * tCold;      // reverse=1:点亮 [litFrom, end]
  const litMid = (litFrom + a.end_deg) / 2;
  const litPt = pointAt(litMid, a.radius);
  ok(litPt.x < 240, "85℃ 时点亮区该在左半边(实测中心 x=" + Math.round(litPt.x) + ")");
}

// ------------------------------------------------------------
console.log("\n" + "=".repeat(56));
if (fail === 0) console.log("全部通过:" + pass + " 项断言");
else {
  console.log("失败 " + fail + " 项 / 共 " + (pass + fail) + " 项:");
  failures.forEach(f => console.log("  - " + f));
}
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
