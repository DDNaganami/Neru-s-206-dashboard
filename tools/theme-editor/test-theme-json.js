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
console.log("\n" + "=".repeat(56));
if (fail === 0) console.log("全部通过:" + pass + " 项断言");
else {
  console.log("失败 " + fail + " 项 / 共 " + (pass + fail) + " 项:");
  failures.forEach(f => console.log("  - " + f));
}
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
