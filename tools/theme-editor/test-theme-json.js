/* ============================================================
 * test-theme-json.js —— 主题文件方言的读取测试(Node 运行)
 *
 *   node tools/theme-editor/test-theme-json.js
 *
 * 为什么值得单测:
 *   主题文件是固件与编辑器**共读**的同一个文件。固件解析器接受
 *   `0xRRGGBB`(十进制也接受),而浏览器只有 JSON.parse,不认识 0x。
 *   两边一旦不一致,表现是"固件读得进去、编辑器导入报语法错" ——
 *   而报错信息指向 JSON 语法,根本看不出是颜色写法的问题。
 *
 * 这里同时用**真实的 theme-default.json** 做回归:那个文件就是给用户
 * 当模板用的,它必须能被编辑器导入(这条曾经是坏的)。
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

// ------------------------------------------------------------
section("0x 颜色 → 十进制");
// 0x141414 = 1315860(手算容易错 —— 第一版这里写成了 1326100,是测试先红才发现)
eq(TJ.stripHex('{"a": 0x141414}'), '{"a": ' + 0x141414 + '}', "小写 0x");
eq(TJ.stripHex('{"a": 0XFFFFFF}'), '{"a": ' + 0xFFFFFF + '}', "大写 0X");
eq(TJ.stripHex('{"a":0x00FF00}'), '{"a":65280}', "冒号后无空格");
eq(TJ.stripHex('{"a": [0x1, 0x2]}'), '{"a": [0x1, 0x2]}', "数组里的 0x 不替换(主题文件里没有这种写法)");
eq(TJ.stripHex('{"name": "0x1234"}'), '{"name": "0x1234"}', "字符串值里的 0x 不动");

{
  const t = TJ.parseThemeJson('{"theme":{"bg_color": 0x101820, "face_size": 220}}');
  eq(t.theme.bg_color, 0x101820, "解析出的颜色值");
  eq(t.theme.face_size, 220, "十进制字段原样");
}
{
  const t = TJ.parseThemeJson('{"theme":{"track_color": 0x232323, "track_opa": 153}}');
  eq(t.theme.track_color, 0x232323, "轨道色");
  eq(t.theme.track_opa, 153, "轨道透明度");
}

// ------------------------------------------------------------
section("真实的 theme-default.json 必须能被导入");
{
  const file = path.join(__dirname, "theme-default.json");
  const text = fs.readFileSync(file, "utf8");

  // 先证明"直接 JSON.parse 会失败" —— 这正是当初的 bug
  let directFailed = false;
  try { JSON.parse(text); } catch { directFailed = true; }
  ok(directFailed, "直接 JSON.parse 应当失败(否则这条测试就没意义了)");

  const t = TJ.themeObject(TJ.parseThemeJson(text));
  eq(t.bg_color, 0x141414, "背景色");
  eq(t.readout.digit_cy, 72, "读数位置");
  eq(t.readout.coolant_cy, 384, "水温读数位置");
  eq(t.screens.length, 2, "两屏");
  eq(t.screens[0].arcs.length, 2, "左屏两条弧");
  eq(t.screens[0].arcs[0].kind, 1, "左屏外弧 = 转速(法系车左=转速表)");
  eq(t.screens[0].arcs[1].kind, 2, "左屏内弧 = 水温");
  eq(t.screens[1].arcs[0].kind, 0, "右屏 = 车速");
  eq(t.screens[0].arcs[0].value_color, 0xFF5C5C, "转速弧点亮色");
  eq(t.screens[1].arcs[0].value_color, 0x39C5FF, "车速弧点亮色");
}

// ------------------------------------------------------------
section("裸根写法(不带 theme 外壳)也接受");
{
  const t = TJ.themeObject(TJ.parseThemeJson('{"bg_color": 0x123456}'));
  eq(t.bg_color, 0x123456, "裸根字段");
  // 有 theme 外壳时取里面的
  const t2 = TJ.themeObject(TJ.parseThemeJson('{"theme":{"bg_color": 0x654321}}'));
  eq(t2.bg_color, 0x654321, "theme 外壳里的字段");
}

// ------------------------------------------------------------
section("坏输入要报错,不能静默返回半个对象");
{
  let threw = false;
  try { TJ.parseThemeJson("{ 这不是 JSON"); } catch { threw = true; }
  ok(threw, "语法错必须抛异常");
}

// ------------------------------------------------------------
// ★ 主题默认值有**三处**副本,必须一致 —— 这一节就是给它们对账的。
//
// 为什么值得单独测:这三处不一致**不会报错**,只会表现成
// "预览里的颜色和真车不一样"或者"照参考文件刷进去，两个表左右颠倒"。
// 后者真实发生过:theme-default.json 里左右屏还是旧布局(左车速右转速),
// 而固件早已改成 左=转速表(法系车)。
//
//   1) lib/themetool/ui_theme.h  的 theme_set_defaults()  ← 固件的事实来源
//   2) tools/theme-editor/theme-default.json             ← 给人当模板的手改参考
//   3) image-editor.html 的 ARC_FALLBACK                 ← 图片编辑器没读主题时的兜底
//      (读了 theme.json 就用主题里的,兜底只是"参考值",但参考值错了照样误导人)
section("三处默认主题必须一致(ui_theme.h / theme-default.json / ARC_FALLBACK)");
{
  const repo = path.resolve(__dirname, "..", "..");
  const hSrc = fs.readFileSync(path.join(repo, "lib", "themetool", "ui_theme.h"), "utf8");
  const jsonSrc = fs.readFileSync(path.join(__dirname, "theme-default.json"), "utf8");
  const imgSrc = fs.readFileSync(path.join(__dirname, "image-editor.html"), "utf8");

  // ---- 1) ui_theme.h:把默认主题的几条弧抠出来 ----
  const arcRe = /([LR])\.arcs\[(\d+)\]\s*=\s*ArcStyle\{\s*ArcKind::(\w+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*lv_color_hex\((0x[0-9A-Fa-f]+)\)\s*,\s*(\d+)\s*,\s*lv_color_hex\((0x[0-9A-Fa-f]+)\)\s*\}/g;
  const kindOf = { Speed: 0, Rpm: 1, Coolant: 2 };
  const hArcs = [];
  let m;
  while ((m = arcRe.exec(hSrc)) !== null) {
    hArcs.push({
      screen: m[1] === "L" ? 0 : 1,
      slot: Number(m[2]),
      kind: kindOf[m[3]],
      start_deg: Number(m[4]),
      end_deg: Number(m[5]),
      radius: Number(m[6]),
      width: Number(m[7]),
      track_color: Number(m[8]),
      track_opa: Number(m[9]),
      value_color: Number(m[10])
    });
  }
  ok(hArcs.length === 3, "ui_theme.h 里解析出 3 条默认弧(得到 " + hArcs.length + ")");
  ok(hArcs.every(a => a.kind !== undefined), "弧类型都认识");

  // 屏与表的对应是产品契约,顺手也钉一下(左=转速表+水温,右=速度表)
  {
    const left = hArcs.filter(a => a.screen === 0).sort((a, b) => a.slot - b.slot);
    const right = hArcs.filter(a => a.screen === 1).sort((a, b) => a.slot - b.slot);
    eq(left.length, 2, "左屏两条弧");
    eq(left[0].kind, 1, "左屏外弧 = 转速(法系车左=转速表)");
    eq(left[1].kind, 2, "左屏内弧 = 水温");
    eq(right.length, 1, "右屏一条弧");
    eq(right[0].kind, 0, "右屏 = 车速");
  }

  // 与 theme-default.json 对照(展开成同样的顺序:左屏外/内 → 右屏)
  const t = TJ.themeObject(TJ.parseThemeJson(jsonSrc));
  const jsonArcs = [];
  for (let s = 0; s < 2; s++) {
    (t.screens[s].arcs || []).forEach((a, k) => jsonArcs.push({
      screen: s, slot: k, kind: a.kind,
      start_deg: a.start_deg, end_deg: a.end_deg,
      radius: a.radius, width: a.width,
      track_color: a.track_color, track_opa: a.track_opa, value_color: a.value_color
    }));
  }
  const hOrdered = hArcs.slice().sort((a, b) => (a.screen - b.screen) || (a.slot - b.slot));
  eq(jsonArcs.length, hOrdered.length, "弧的条数一致");
  for (let i = 0; i < Math.min(jsonArcs.length, hOrdered.length); i++) {
    const h = hOrdered[i], j = jsonArcs[i];
    const tag = "弧 " + i + "(第" + h.screen + "屏 #" + h.slot + ")";
    eq(j.kind, h.kind, tag + " 类型");
    eq(j.start_deg, h.start_deg, tag + " 起始角");
    eq(j.end_deg, h.end_deg, tag + " 结束角");
    eq(j.radius, h.radius, tag + " 半径");
    eq(j.width, h.width, tag + " 线宽");
    eq(j.track_color, h.track_color, tag + " 轨道色");
    eq(j.track_opa, h.track_opa, tag + " 轨道不透明度");
    eq(j.value_color, h.value_color, tag + " 点亮色");
  }
  // 背景色/表情大小也要一致
  eq(t.bg_color, Number(/t\.bg_color\s*=\s*(0x[0-9A-Fa-f]+)/.exec(hSrc)[1]), "背景色");
  eq(t.face_size, Number(/t\.face_size\s*=\s*(\d+)/.exec(hSrc)[1]), "表情大小");

  // ---- 3) image-editor.html 的 ARC_FALLBACK ----
  const fbBlock = /const ARC_FALLBACK\s*=\s*\{([\s\S]*?)\n\};/.exec(imgSrc);
  ok(!!fbBlock, "在 image-editor.html 里找到 ARC_FALLBACK");
  const fbRe = /kind:\s*(\d+),\s*start_deg:\s*(-?\d+),\s*end_deg:\s*(-?\d+),\s*radius:\s*(\d+),\s*width:\s*(\d+),\s*track_color:\s*(0x[0-9A-Fa-f]+),\s*track_opa:\s*(\d+),\s*value_color:\s*(0x[0-9A-Fa-f]+)/g;
  const fbArcs = [];
  while ((m = fbRe.exec(fbBlock ? fbBlock[1] : "")) !== null) {
    fbArcs.push({
      kind: Number(m[1]), start_deg: Number(m[2]), end_deg: Number(m[3]),
      radius: Number(m[4]), width: Number(m[5]),
      track_color: Number(m[6]), track_opa: Number(m[7]), value_color: Number(m[8])
    });
  }
  eq(fbArcs.length, hOrdered.length, "ARC_FALLBACK 的弧数 = 默认主题的弧数");
  for (let i = 0; i < Math.min(fbArcs.length, hOrdered.length); i++) {
    const f = fbArcs[i], h = hOrdered[i];
    const tag = "ARC_FALLBACK 弧 " + i;
    eq(f.kind, h.kind, tag + " 类型");
    eq(f.start_deg, h.start_deg, tag + " 起始角");
    eq(f.end_deg, h.end_deg, tag + " 结束角");
    eq(f.radius, h.radius, tag + " 半径");
    eq(f.width, h.width, tag + " 线宽");
    eq(f.track_color, h.track_color, tag + " 轨道色");
    eq(f.track_opa, h.track_opa, tag + " 轨道不透明度");
    eq(f.value_color, h.value_color, tag + " 点亮色");
  }
  // 三条弧的点亮色必须互不相同(不然后面"看颜色认哪条弧"就失效了)
  const lit = fbArcs.map(a => a.value_color);
  eq(new Set(lit).size, lit.length, "三条弧的点亮色互不相同");
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
