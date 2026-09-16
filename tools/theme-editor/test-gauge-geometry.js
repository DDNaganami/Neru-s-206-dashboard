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
      ok(/degToRad|rad\(/.test(drawArcBody[1]),
         f + " 的 drawArc() 里用了角度换算(没有的话就是空转)");
      ok(/ctx\.arc\(/.test(drawArcBody[1]), f + " 的 drawArc() 真的画了圆弧");
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
console.log("\n" + "=".repeat(56));
if (fail === 0) console.log("全部通过:" + pass + " 项断言");
else {
  console.log("失败 " + fail + " 项 / 共 " + (pass + fail) + " 项:");
  failures.forEach(f => console.log("  - " + f));
}
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
