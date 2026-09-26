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
  eq(t.readout.digit_cy, 88, "读数位置");
  eq(t.readout.coolant_cy, 444, "水温读数位置(实屏那一单:384 会被表情下巴压掉)");
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
// ★ 两个页面共用同一份"本机配色"
//
// 为什么值得测:弧的配色在两个页面里都能改(用户是在图片编辑器里对着表情图调色的,
// 却也要能在主题编辑器里改)。共享靠的是一个 localStorage 键 ——
// 键名写错一个字符、或者哪一页只读不写,共享就静默失效:
// 表现是"我在 A 页改了色,打开 B 页还是旧的",而不会有任何报错。
// ------------------------------------------------------------
section("两个编辑器共用同一份配色(localStorage)");
{
  const idxSrc = fs.readFileSync(path.join(__dirname, "index.html"), "utf8");
  const imgSrc2 = fs.readFileSync(path.join(__dirname, "image-editor.html"), "utf8");
  const keyOf = (s) => {
    const m = /const THEME_KEY = "([^"]+)"/.exec(s);
    return m ? m[1] : null;
  };
  const kTheme = keyOf(idxSrc);
  const kImage = keyOf(imgSrc2);
  ok(kTheme !== null, "主题编辑器里定义了 THEME_KEY");
  ok(kImage !== null, "图片编辑器里定义了 THEME_KEY");
  eq(kImage, kTheme, "两页的键名必须一致");

  for (const [name, src] of [["主题编辑器", idxSrc], ["图片编辑器", imgSrc2]]) {
    ok(src.indexOf("localStorage.setItem(THEME_KEY") >= 0, name + " 会写这份配色");
    ok(src.indexOf("localStorage.getItem(THEME_KEY") >= 0, name + " 会读这份配色");
  }
  // 图片编辑器必须真的能改色 + 导出(不然"读得到"却没有产出的路子)
  ok(/id="btn-theme-export"/.test(imgSrc2), "图片编辑器有「导出 theme.json」按钮");
  ok(/type = "color"/.test(imgSrc2) || /type:"color"/.test(imgSrc2) ||
     /type = 'color'/.test(imgSrc2) || /\.type = "color"/.test(imgSrc2),
     "图片编辑器里用了颜色选择器");
  ok(/exportThemeJson/.test(imgSrc2), "图片编辑器里有导出函数");

  // 存的形态:记录里必须**连基线一起存** —— 只存 theme 会让"改了固件默认值
  // 但用户刷新后看不到"这种事发生(用户实测撞到:水温弧 145→330 改成 0→180)。
  for (const [name, src] of [["主题编辑器", idxSrc], ["图片编辑器", imgSrc2]]) {
    ok(/v:\s*2\s*,\s*base:/.test(src), name + " 的记录要带 v:2 与 base(基线)");
    ok(/theme:\s*(T|gTheme)\b/.test(src), name + " 的记录要带 theme");
    ok(/ThemeJson\.mergeDefaults/.test(src), name + " 要用三方合并读回记录");
    ok(/THEME_KEY_V1/.test(src), name + " 要能迁移旧记录(备份键)");
  }

  // ★ 把用户的 bug 完整演一遍:用**真实的 ARC_FALLBACK**(从页面里解析出来的)
  //   当作"当前默认值",拿旧默认值(水温弧 145→330)当基线,
  //   用户只改过转速弧颜色 → 重新打开页面时水温弧必须跟着新默认值走。
  {
    const fbBlock2 = /const ARC_FALLBACK\s*=\s*\{([\s\S]*?)\n\};/.exec(imgSrc2);
    // 这段是 JS 对象字面量(键没引号、颜色是 0x…),不是 JSON —— 用 Function 求值。
    // 先去掉行注释,免得注释里的内容影响解析。
    const nowDefaults = new Function(
      "return {" + fbBlock2[1].replace(/\/\/[^\n]*/g, "") + "};")();
    const oldDefaults = JSON.parse(JSON.stringify(nowDefaults));
    oldDefaults.screens[0].arcs[1].start_deg = 145;   // 改动前的水温弧
    oldDefaults.screens[0].arcs[1].end_deg = 330;
    const usersTheme = JSON.parse(JSON.stringify(oldDefaults));
    usersTheme.screens[0].arcs[0].value_color = 0x00FF00;   // 用户只改了这一个

    const rec = { v: 2, base: oldDefaults, theme: usersTheme };
    const reloaded = TJ.mergeDefaults(rec.base, rec.theme, nowDefaults);
    eq(reloaded.screens[0].arcs[1].start_deg, nowDefaults.screens[0].arcs[1].start_deg,
       "水温弧起点跟着新默认值（用户没碰过它）");
    eq(reloaded.screens[0].arcs[1].end_deg, 180, "水温弧终点 = 180（上方开口）");
    eq(reloaded.screens[0].arcs[0].value_color, 0x00FF00, "用户改过的颜色没丢");
  }

  // 存的形态:B 页写的是**裸主题对象**,A 页用 themeObject 读(带 theme 外壳也认)。
  // 走一遍真实的往返,确认字段不丢。
  const t0 = TJ.themeObject(TJ.parseThemeJson(
    fs.readFileSync(path.join(__dirname, "theme-default.json"), "utf8")));
  const stored = JSON.stringify(t0);                 // 图片编辑器 saveTheme() 的写法
  const back = TJ.themeObject(TJ.parseThemeJson(stored));
  eq(back.bg_color, t0.bg_color, "往返后背景色");
  eq(back.screens.length, t0.screens.length, "往返后屏数");
  eq(back.screens[0].arcs[1].value_color, t0.screens[0].arcs[1].value_color, "往返后水温弧点亮色");
  eq(back.screens[1].arcs[0].radius, t0.screens[1].arcs[0].radius, "往返后车速弧半径");
}

// ------------------------------------------------------------
// ★ 三方合并:让"本机记住的配色"不再挡住新的默认值
//
// 这是用户实际撞到的 bug:"水温弧我刷新后没发现改变啊" ——
// 编辑器把整份配色记在本机,固件默认值改了(水温弧 145→330 改成 0→180),
// 记录里的旧值把它盖住了。这不是固件的问题,是"存了整份"的问题。
// 现在存记录时连**基线**一起存,读回来逐字段三方比对。
section("三方合并 / 差异:改了默认值之后,用户没碰过的字段要跟着更新");
{
  // D1 = 旧默认值(水温弧 145→330), D2 = 新默认值(0→180)
  const D1 = { bg_color: 0x141414, screens: [
    { arcs: [ { kind: 1, value_color: 0xFF5C5C }, { kind: 2, start_deg: 145, end_deg: 330 } ] },
    { arcs: [ { kind: 0, value_color: 0x39C5FF } ] }
  ] };
  const D2 = JSON.parse(JSON.stringify(D1));
  D2.screens[0].arcs[1].start_deg = 0;
  D2.screens[0].arcs[1].end_deg = 180;

  // 用户改过一个颜色(转速弧),其它都是当时的默认值
  const OURS = JSON.parse(JSON.stringify(D1));
  OURS.screens[0].arcs[0].value_color = 0x00FF00;

  // 记录 = {base: D1, theme: OURS};读到新默认值 D2 时:
  const merged = TJ.mergeDefaults(D1, OURS, D2);
  eq(merged.screens[0].arcs[1].start_deg, 0, "用户没碰过的水温弧起点 → 跟新默认值");
  eq(merged.screens[0].arcs[1].end_deg, 180, "用户没碰过的水温弧终点 → 跟新默认值");
  eq(merged.screens[0].arcs[0].value_color, 0x00FF00, "用户改过的颜色 → 保留");
  eq(merged.bg_color, D2.bg_color, "没碰过的其他字段 → 默认值");

  // 没有基线时保守:一律保留用户的值(宁可留着旧的,也不能悄悄改用户的)
  eq(TJ.mergeDefaults(undefined, OURS, D2).screens[0].arcs[1].start_deg, 145,
     "没有基线时保留用户的值");

  // 差异只记"改过的叶子",不是整份
  const patch = TJ.diffDeep(D1, OURS);
  eq(Object.keys(patch).length, 1, "差异只有一处顶层键(screens)");
  eq(Object.keys(patch.screens).length, 1, "差异只涉及第 0 屏");
  eq(JSON.stringify(patch.screens[0]),
     JSON.stringify({ arcs: { 0: { value_color: 0x00FF00 } } }),
     "差异精确到叶子:只记下序号 0 那张弧的颜色");

  // 数组的差异必须是**数字键对象**而不是稀疏数组 —— 稀疏数组经 JSON 会变成
  // null,读回来会把默认值清掉(这就是 diffDeep 不用 hole 的原因)
  const round = JSON.parse(JSON.stringify(patch));
  eq(round.screens[0].arcs[0].value_color, 0x00FF00, "差异经 JSON 往返不丢");
  eq(round.screens[0].arcs[1], undefined, "没改的下标不出现在差异里(不写 null)");
  const applied = TJ.applyPatch(D2, round);
  eq(applied.screens[0].arcs[1].end_deg, 180, "把差异贴回新默认值 → 仍是新的");
  eq(applied.screens[0].arcs[0].value_color, 0x00FF00, "把差异贴回新默认值 → 用户的颜色还在");

  // 完全相同 → 没有差异(存的就是空记录,不会把默认值钉死)
  eq(TJ.diffDeep(D1, D1), undefined, "没有改动时没有差异");
  eq(TJ.deepEqual(TJ.applyPatch(D2, TJ.diffDeep(D1, OURS) || {}), merged), true,
     "applyPatch(base, diff) 与 mergeDefaults 结果一致");
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
  // 末尾的 reverse 是可选的(C++ 聚合初始化可以少写,缺省即 0)
  const arcRe = /([LR])\.arcs\[(\d+)\]\s*=\s*ArcStyle\{\s*ArcKind::(\w+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*,\s*lv_color_hex\((0x[0-9A-Fa-f]+)\)\s*,\s*(\d+)\s*,\s*lv_color_hex\((0x[0-9A-Fa-f]+)\)\s*(?:,\s*(\d+)\s*)?\}/g;
  // kind 的数字**只能往后加**(主题 JSON 里存的就是这个数):
  //   0=车速 1=转速 2=水温 3=进气温度(2026-09 新增)
  const kindOf = { Speed: 0, Rpm: 1, Coolant: 2, Intake: 3 };
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
      value_color: Number(m[10]),
      reverse: m[11] === undefined ? 0 : Number(m[11])
    });
  }
  ok(hArcs.length === 4, "ui_theme.h 里解析出 4 条默认弧(得到 " + hArcs.length + ")");
  ok(hArcs.every(a => a.kind !== undefined), "弧类型都认识");

  // 屏与表的对应是产品契约,顺手也钉一下
  // (左 = 转速表 + 水温;右 = 速度表 + 进气温度,2026-09 起左右对称)
  {
    const left = hArcs.filter(a => a.screen === 0).sort((a, b) => a.slot - b.slot);
    const right = hArcs.filter(a => a.screen === 1).sort((a, b) => a.slot - b.slot);
    eq(left.length, 2, "左屏两条弧");
    eq(left[0].kind, 1, "左屏外弧 = 转速(法系车左=转速表)");
    eq(left[1].kind, 2, "左屏内弧 = 水温");
    eq(right.length, 2, "右屏两条弧");
    eq(right[0].kind, 0, "右屏外弧 = 车速");
    eq(right[1].kind, 3, "右屏内弧 = 进气温度(与左屏水温对称)");
    // 副表几何必须一模一样,只换屏与颜色 —— 否则两块表看着不像一套仪表
    eq(right[1].radius, left[1].radius, "进气弧半径 = 水温弧半径");
    eq(right[1].width, left[1].width, "进气弧宽度 = 水温弧宽度");
    eq(right[1].start_deg, left[1].start_deg, "进气弧起点 = 水温弧起点");
    eq(right[1].end_deg, left[1].end_deg, "进气弧终点 = 水温弧终点");
    eq(right[1].reverse, left[1].reverse, "进气弧镜像方向 = 水温弧(都从左端起涨)");
    ok(right[1].value_color !== left[1].value_color, "两条副弧颜色不同(一眼能分清)");
  }

  // 与 theme-default.json 对照(展开成同样的顺序:左屏外/内 → 右屏)
  const t = TJ.themeObject(TJ.parseThemeJson(jsonSrc));
  const jsonArcs = [];
  for (let s = 0; s < 2; s++) {
    (t.screens[s].arcs || []).forEach((a, k) => jsonArcs.push({
      screen: s, slot: k, kind: a.kind,
      start_deg: a.start_deg, end_deg: a.end_deg,
      radius: a.radius, width: a.width,
      track_color: a.track_color, track_opa: a.track_opa, value_color: a.value_color,
      reverse: a.reverse === undefined ? 0 : a.reverse
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
    eq(j.reverse, h.reverse, tag + " 涨幅方向(reverse)");
  }
  // 背景色/表情大小也要一致
  eq(t.bg_color, Number(/t\.bg_color\s*=\s*(0x[0-9A-Fa-f]+)/.exec(hSrc)[1]), "背景色");
  eq(t.face_size, Number(/t\.face_size\s*=\s*(\d+)/.exec(hSrc)[1]), "表情大小");

  // ---- 3) image-editor.html 的 ARC_FALLBACK ----
  const fbBlock = /const ARC_FALLBACK\s*=\s*\{([\s\S]*?)\n\};/.exec(imgSrc);
  ok(!!fbBlock, "在 image-editor.html 里找到 ARC_FALLBACK");
  const fbRe = /kind:\s*(\d+),\s*start_deg:\s*(-?\d+),\s*end_deg:\s*(-?\d+),\s*radius:\s*(\d+),\s*width:\s*(\d+),\s*track_color:\s*(0x[0-9A-Fa-f]+),\s*track_opa:\s*(\d+),\s*value_color:\s*(0x[0-9A-Fa-f]+)(?:,\s*reverse:\s*(\d+))?/g;
  const fbArcs = [];
  while ((m = fbRe.exec(fbBlock ? fbBlock[1] : "")) !== null) {
    fbArcs.push({
      kind: Number(m[1]), start_deg: Number(m[2]), end_deg: Number(m[3]),
      radius: Number(m[4]), width: Number(m[5]),
      track_color: Number(m[6]), track_opa: Number(m[7]), value_color: Number(m[8]),
      reverse: m[9] === undefined ? 0 : Number(m[9])
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
    eq(f.reverse, h.reverse, tag + " 涨幅方向(reverse)");
  }
  // 水温弧必须镜像(从左端起涨) —— 三处都要一致,这里再显式说一次
  eq(hOrdered[1].reverse, 1, "默认主题:水温弧 reverse = 1(镜像)");
  eq(hOrdered[0].reverse, 0, "默认主题:转速弧 reverse = 0");
  // 三条弧的点亮色必须互不相同(不然后面"看颜色认哪条弧"就失效了)
  const lit = fbArcs.map(a => a.value_color);
  eq(new Set(lit).size, lit.length, "三条弧的点亮色互不相同");
}

// ------------------------------------------------------------
// ★ 读数颜色:图片编辑器那一份副本 vs 固件
//
// 这一节是给"owner 换了白底图,屏上的字看不见"那条反馈加的。颜色字段本身
// 早就有了(固件 + 主题编辑器都支持),缺的是**图片编辑器**那一页 ——
// 用户正是在那一页换底图、对着底图看效果,所以那一页必须能改、能立刻看见。
//
// 于是页面里多出两份副本:四个默认色(ARC_FALLBACK.readout)与预览用的
// 位置/字号表(READOUT_LAYOUT / READOUT_FONT_PX)。抄错的后果与老规矩一样:
// **不报错**,只是"预览里的字和真车不是一个颜色/字号"—— 而预览正是这一页
// 唯一能看的东西。所以这里把它们逐个对到 ui_theme.h / dash_ui.cpp 上。
// ------------------------------------------------------------
section("读数颜色:图片编辑器 vs ui_theme.h / dash_ui.cpp");
{
  const repo = path.resolve(__dirname, "..", "..");
  const hSrc = fs.readFileSync(path.join(repo, "lib", "themetool", "ui_theme.h"), "utf8");
  const cSrc = fs.readFileSync(path.join(repo, "src", "dash_ui.cpp"), "utf8");
  const imgSrc = fs.readFileSync(path.join(__dirname, "image-editor.html"), "utf8");

  // ---- 1) 固件默认值(唯一事实来源) ----
  const fw = {};
  const fwRe = /t\.readout\.(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)/g;
  let r;
  while ((r = fwRe.exec(hSrc)) !== null) fw[r[1]] = Number(r[2]);
  const FW_KEYS = ["digit_color", "unit_color", "coolant_color", "intake_color",
                   "digit_font", "unit_font", "sub_font", "digit_cy", "unit_cy",
                   "coolant_cy", "intake_cy", "show_units", "show_coolant", "show_intake"];
  eq(Object.keys(fw).length, FW_KEYS.length,
     "ui_theme.h 里解析出 " + FW_KEYS.length + " 个读数默认值(得到 " +
     Object.keys(fw).length + ")");

  const COLOR_KEYS = ["digit_color", "unit_color", "coolant_color", "intake_color"];
  eq(fw.digit_color, 0xFFFFFF, "固件默认:大数字白");
  eq(fw.unit_color, 0x9AA0A6, "固件默认:单位灰");
  eq(fw.coolant_color, 0x7CFF6B, "固件默认:水温数字 = 水温弧色");
  eq(fw.intake_color, 0xFFB020, "固件默认:进气温度数字 = 进气弧色");
  // ★ 这四个数就是"向后兼容"的**定义**:老主题没有 readout 时,固件与编辑器
  //   都用它们 —— 所以它们一改,"老主题看起来和今天一样"这句话就不成立了。
  //   改这里必须是有意的(并且要重新想一遍老主题的观感)。

  // ---- 2) 固件真的把颜色用在读数标签上(dash_ui.cpp) ----
  for (const k of COLOR_KEYS) {
    ok(cSrc.indexOf("lv_color_hex(READOUT_" + k.toUpperCase() + ")") >= 0,
       "dash_ui.cpp 用 " + k + " 建读数标签");
  }

  // ---- 3) theme-default.json(给人当模板的那份) ----
  const tpl = TJ.themeObject(TJ.parseThemeJson(
    fs.readFileSync(path.join(__dirname, "theme-default.json"), "utf8")));
  ok(!!tpl.readout, "theme-default.json 里有 readout 段");
  for (const k of COLOR_KEYS) eq(tpl.readout[k], fw[k], "theme-default.json 的 " + k);

  // ---- 4) image-editor.html 的 ARC_FALLBACK.readout ----
  const fb = /const ARC_FALLBACK\s*=\s*\{([\s\S]*?)\n\};/.exec(imgSrc);
  ok(!!fb, "在 image-editor.html 里找到 ARC_FALLBACK");
  const fbObj = new Function("return {" + fb[1].replace(/\/\/[^\n]*/g, "") + "};")();
  ok(!!fbObj.readout, "ARC_FALLBACK 里有 readout 段");
  for (const k of COLOR_KEYS) eq(fbObj.readout[k], fw[k], "ARC_FALLBACK 的 " + k);
  eq(Object.keys(fbObj.readout).length, COLOR_KEYS.length,
     "ARC_FALLBACK.readout 只放四个颜色(位置/开关走 READOUT_LAYOUT,不混进导出的文件)");

  // ---- 5) 把页面里的读数逻辑抠出来真跑一遍 ----
  //   抠的必须是**页面自己那段源码**(不是在这里抄一份),否则这一节就只是
  //   在测一份复制品 —— 页面改了它却还是绿的。
  const grab = (re, what) => {
    const g = re.exec(imgSrc);
    ok(!!g, "在 image-editor.html 里找到 " + what);
    return g ? g[0] : "";
  };
  const pageSrc = [
    grab(/function hexOf\(n\)\s*\{[^\n]*\}/, "hexOf()"),
    grab(/function ensureReadout\(t\)\s*\{[\s\S]*?\n\}/, "ensureReadout()"),
    grab(/const READOUT_LAYOUT\s*=\s*\{[\s\S]*?\n\};/, "READOUT_LAYOUT"),
    grab(/const READOUT_FONT_PX\s*=\s*\{[^\n]*\};/, "READOUT_FONT_PX"),
    grab(/const AUX_OF_SCREEN\s*=\s*\[[^\n]*\];/, "AUX_OF_SCREEN"),
    grab(/function readoutLayout\(r\)\s*\{[\s\S]*?\n\}/, "readoutLayout()"),
    grab(/function readoutFontPx\(which\)\s*\{[\s\S]*?\n\}/, "readoutFontPx()"),
    grab(/function readoutText\(kind, st\)\s*\{[\s\S]*?\n\}/, "readoutText()"),
    grab(/function unitText\(kind\)\s*\{[\s\S]*?\n\}/, "unitText()"),
    grab(/function auxText\(kind, st\)\s*\{[^\n]*\}/, "auxText()"),
    grab(/function primaryKindOf\(sc\)\s*\{[\s\S]*?\n\}/, "primaryKindOf()"),
    grab(/function screenHasKind\(sc, kind\)\s*\{[\s\S]*?\n\}/, "screenHasKind()"),
    grab(/function drawReadoutLabel\(ctx, W, cy480, text, fontPx, colorHex\)\s*\{[\s\S]*?\n\}/,
         "drawReadoutLabel()"),
    grab(/function drawReadout\(ctx, W, which, sc, st\)\s*\{[\s\S]*?\n\}/, "drawReadout()"),
    "const ARC_FALLBACK = {" + fb[1] + "};"
  ].join("\n");
  // previewSide()(240 那块板还是 480 档)与 gTheme(当前主题)在页面里是全局量,
  // 这里当参数注进去 —— 于是同一段源码能在两种屏上各跑一遍。
  // apiFrom(previewSideStub, theme) → 页面那几个函数的对象。
  const apiFrom = new Function("previewSide", "gTheme",
    pageSrc + "\nreturn { ensureReadout: ensureReadout, readoutLayout: readoutLayout," +
    " readoutFontPx: readoutFontPx, readoutText: readoutText, unitText: unitText," +
    " auxText: auxText, primaryKindOf: primaryKindOf, screenHasKind: screenHasKind," +
    " drawReadout: drawReadout, READOUT_LAYOUT: READOUT_LAYOUT," +
    " READOUT_FONT_PX: READOUT_FONT_PX, ARC_FALLBACK: ARC_FALLBACK };");
  const makeApi = (boardPx, theme) => apiFrom(() => boardPx, theme);
  const P480 = makeApi(480, fbObj);
  const P240 = makeApi(240, fbObj);
  const api = P480;

  // 预览用的位置/开关 = 固件默认值(本页不提供这些控件,只求"画得一样")
  eq(Object.keys(api.READOUT_LAYOUT).length, 8, "READOUT_LAYOUT 八个字段(含副表字号)");
  for (const k of ["digit_cy", "unit_cy", "coolant_cy", "intake_cy", "sub_font",
                   "show_units", "show_coolant", "show_intake"]) {
    eq(api.READOUT_LAYOUT[k], fw[k], "READOUT_LAYOUT." + k + " = 固件默认值");
  }

  // 字号表 = ui_theme.h 末尾那张 kReadoutFontPx(480 → 48/18/24、240 → 24/10/14)
  const tbl = /kReadoutFontPx\[kReadoutResTierCount\]\[kReadoutFontTierCount\]\s*=\s*\{([\s\S]*?)\n\};/
    .exec(hSrc);
  ok(!!tbl, "解析 ui_theme.h 的 kReadoutFontPx 表");
  const rows = [];
  // ★ 列数不再是写死的 2:2026-09-27 加了第三档(副表),这里按逗号切、列数由表自己决定。
  const rowRe = /\{([^{}]*)\}/g;
  while ((r = rowRe.exec(tbl[1])) !== null) {
    rows.push(r[1].split(",").map((s) => Number(s.trim())).filter((v) => !Number.isNaN(v)));
  }
  eq(rows.length, 2, "字号表两档(480 / 240)");
  eq(JSON.stringify(api.READOUT_FONT_PX[480]), JSON.stringify(rows[0]), "480 档字号 = 固件表");
  eq(JSON.stringify(api.READOUT_FONT_PX[240]), JSON.stringify(rows[1]), "240 档字号 = 固件表");
  eq(P480.readoutFontPx(0), rows[0][0], "480 画布:大数字点数");
  eq(P480.readoutFontPx(1), rows[0][1], "480 画布:单位点数");
  eq(P240.readoutFontPx(0), rows[1][0], "240 画布:大数字点数");
  eq(P240.readoutFontPx(1), rows[1][1], "240 画布:单位点数");

  // 文本格式 = dash_ui.cpp 的 readout_value() / unit_text()
  const utBlock = /static const char\* unit_text\(ArcKind k\)\s*\{[\s\S]*?\n\}/.exec(cSrc);
  ok(!!utBlock, "在 dash_ui.cpp 里找到 unit_text()");
  ok(/ArcKind::Speed:\s*return "km\/h"/.test(utBlock[0]), "固件:车速单位 km/h");
  ok(/ArcKind::Rpm:\s*return "rpm"/.test(utBlock[0]), "固件:转速单位 rpm");
  ok(/default:\s*return ""/.test(utBlock[0]),
     "固件:水温/进气温度**没有单位行**(default 返回空串)");
  eq(api.unitText(0), "km/h", "页面:车速单位");
  eq(api.unitText(1), "rpm", "页面:转速单位");
  eq(api.unitText(2), "", "页面:水温当大数字时单位行是空的(与固件一致)");
  eq(api.unitText(3), "", "页面:进气温度当大数字时单位行也是空的");
  {
    const st = { speed: 62.4, rpm: 2734, coolant: 87.6, intake: 31.4 };
    eq(api.readoutText(0, st), "62", "车速取整到 1");
    eq(api.readoutText(1, st), "2730", "转速取到 10 位(OBD 的个位是噪声)");
    eq(api.readoutText(2, st), "88", "水温取整到 1℃");
    eq(api.readoutText(3, st), "31", "进气温度取整到 1℃");
    eq(api.auxText(2, st), "88°C", "副表那一行 = 数字 + °C(与固件 \"%d°C\" 一致)");
  }

  // ---- 6) 向后兼容:老主题(没有 readout)预览出来和今天一模一样 ----
  {
    const oldTheme = JSON.parse(JSON.stringify(fbObj));
    delete oldTheme.readout;
    const a = makeApi(480, oldTheme);
    const filled = a.ensureReadout(oldTheme);        // 页面读主题文件时就是这么补的
    for (const k of COLOR_KEYS) eq(filled[k], fw[k], "老主题补上的 " + k + " = 固件默认色");
    eq(Object.keys(filled).length, COLOR_KEYS.length,
       "只补四个颜色,不往用户文件里塞位置/字号");

    // readout 里只有位置(拿主题编辑器改过位置)时,颜色照样补齐、位置不动
    const partial = { readout: { digit_cy: 90 } };
    a.ensureReadout(partial);
    eq(partial.readout.digit_color, fw.digit_color, "只有位置的老主题也补上颜色");
    eq(partial.readout.digit_cy, 90, "用户写过的位置不动");

    // 预览画在哪:主题里没写就用固件默认值,而且**不写回主题对象**
    const lay = a.readoutLayout({});
    eq(lay.digit_cy, fw.digit_cy, "没写位置 → 预览用固件默认位置");
    eq(lay.show_units, fw.show_units, "没写开关 → 预览用固件默认开关");
    const untouched = {};
    a.readoutLayout(untouched);
    eq(Object.keys(untouched).length, 0,
       "readoutLayout() 不往主题里写东西(兜底不会混进导出的 theme.json)");
  }

  // ---- 7) 预览真的按这些颜色画字("改了颜色,预览跟着变"的机器证明) ----
  {
    const STAGE = { speed: 62.4, rpm: 2734, coolant: 87.6, intake: 31.4 };
    // 只实现 drawReadoutLabel 用到的那几个 canvas 方法 + 记下每一次 fillText。
    // canvasW = 画布宽度,boardPx = 目标板的屏宽(阶段模拟那排小图是 200 宽的
    // 画布画**设备屏**的缩略图,所以这两个数必须分开传 —— 合成一个就测不出
    // "缩两次"那个 bug)。
    const render = (theme, which, canvasW, boardPx) => {
      const calls = [];
      const ctx = {
        fillStyle: "", font: "", textAlign: "", textBaseline: "",
        save() {}, restore() {},
        fillText(text, x, y) {
          calls.push({ text: text, x: x, y: y, color: ctx.fillStyle, font: ctx.font });
        }
      };
      apiFrom(() => boardPx, theme)
        .drawReadout(ctx, canvasW, which, theme.screens[which], STAGE);
      return calls;
    };

    const def = P480.ARC_FALLBACK;
    const left = render(def, 0, 480, 480);
    eq(left.length, 3, "左屏默认画三行:大数字 + 单位 + 水温");
    eq(left[0].text, "2730", "左屏大数字 = 转速");
    eq(left[0].color, "#FFFFFF", "大数字用 digit_color");
    eq(left[1].text, "rpm", "左屏单位");
    eq(left[1].color, "#9AA0A6", "单位用 unit_color");
    eq(left[2].text, "88°C", "左屏底部 = 水温");
    eq(left[2].color, "#7CFF6B", "水温数字用 coolant_color");

    const right = render(def, 1, 480, 480);
    eq(right.length, 3, "右屏默认画三行:大数字 + 单位 + 进气温度");
    eq(right[0].text, "62", "右屏大数字 = 车速");
    eq(right[1].text, "km/h", "右屏单位");
    eq(right[2].text, "31°C", "右屏底部 = 进气温度");
    eq(right[2].color, "#FFB020", "进气温度数字用 intake_color");

    // ★ 白底图那一幕:把四个颜色改成深色 → 预览里画出来的就必须是深色
    const dark = JSON.parse(JSON.stringify(def));
    dark.readout.digit_color = 0x101010;
    dark.readout.unit_color = 0x202020;
    dark.readout.coolant_color = 0x303030;
    dark.readout.intake_color = 0x404040;
    const dl = render(dark, 0, 480, 480), dr = render(dark, 1, 480, 480);
    eq(dl[0].color, "#101010", "改成深色的大数字 → 预览里就是深色");
    eq(dl[1].color, "#202020", "改成深色的单位 → 预览里就是深色");
    eq(dl[2].color, "#303030", "改成深色的水温 → 预览里就是深色");
    eq(dr[2].color, "#404040", "改成深色的进气温度 → 预览里就是深色");

    // 位置与字号:cy 是 480 基准、按画布缩;字号是**设备屏上的点数**,只缩一次
    eq(dl[0].y, fw.digit_cy, "480 画布:数字中心 y = digit_cy");
    eq(dl[1].y, fw.unit_cy, "480 画布:单位中心 y = unit_cy");
    ok(/^48px/.test(dl[0].font), "480 画布:大数字 48 号(得到 " + dl[0].font + ")");
    ok(/^18px/.test(dl[1].font), "480 画布:单位 18 号(得到 " + dl[1].font + ")");
    const l240 = render(def, 0, 240, 240);
    eq(l240[0].y, fw.digit_cy * 240 / 480, "240 画布:数字中心 y 按 480→240 缩(36)");
    ok(/^24px/.test(l240[0].font),
       "240 画布:大数字 **24 号**(查 240 档、只缩一次;得到 " + l240[0].font + ")");
    ok(/^10px/.test(l240[1].font), "240 画布:单位 10 号(得到 " + l240[1].font + ")");
    // 阶段模拟那排小图固定 200 宽,是"设备屏缩到 200":
    //   480 档设备 → 48×200/480 = 20 号;240 档设备 → 24×200/240 = 20 号
    //   (字号缩**两次**的话这里会得到 8 号 —— 那一行就是钉这个的)
    const small480 = render(def, 0, 200, 480);
    ok(/^20px/.test(small480[0].font),
       "200 宽小图(480 档设备):48 × 200/480 = 20 号(得到 " + small480[0].font + ")");
    const small240 = render(def, 0, 200, 240);
    ok(/^20px/.test(small240[0].font),
       "200 宽小图(240 档设备):24 × 200/240 = 20 号(得到 " + small240[0].font + ")");

    // 开关关掉就不该画:show_units=0 → 只剩大数字 + 水温
    const noUnit = JSON.parse(JSON.stringify(def));
    noUnit.readout.show_units = 0;
    const nu = render(noUnit, 0, 480, 480);
    eq(nu.length, 2, "show_units=0 → 不画单位那一行");
    eq(nu[0].text, "2730", "关掉单位后第一行还是大数字");
    // show_coolant=0 → 左屏只剩大数字 + 单位
    const noCool = JSON.parse(JSON.stringify(def));
    noCool.readout.show_coolant = 0;
    eq(render(noCool, 0, 480, 480).length, 2, "show_coolant=0 → 不画水温那一行");

    // 副表弧搬到没有它的屏上就不该画(与固件的"哪屏有那条弧才画"一致)
    const onlyRpm = { screens: [ { arcs: [ { kind: 1 } ] }, { arcs: [ { kind: 0 } ] } ] };
    const one = render(onlyRpm, 0, 480, 480);
    eq(one.length, 2, "左屏只有转速弧 → 只画大数字 + 单位,没有水温行");
  }
}

// ------------------------------------------------------------
  section("告警 / 蜂鸣器默认值：alerts.h ↔ 编辑器 ↔ theme-default.json 三处一致");
  {
    const repo2 = path.resolve(__dirname, "..", "..");
    // 本段自己读一遍（上面那一段的 idxSrc 是块作用域里的，出了那个块就没了）
    const idxSrcA = fs.readFileSync(path.join(__dirname, "index.html"), "utf8");
    // 事实来源 = 固件：lib/dashcore/alerts.h 的 AlertsConfig 成员初始值。
    // ★ 与上面 ui_theme.h 那一节同一套办法：**解析真源码**，不在这里抄数字 ——
    //   抄一份就会在"改了固件忘了改这里"时静默分叉。
    const aSrc = fs.readFileSync(path.join(repo2, "lib", "dashcore", "alerts.h"), "utf8");
    const body = /struct AlertsConfig\s*\{([\s\S]*?)\n\};/.exec(aSrc);
    ok(!!body, "在 alerts.h 里找到 struct AlertsConfig");
    const FIELDS = ["overspeed_kmh", "overspeed_hyst_kmh", "redline_rpm", "redline_hyst_rpm",
                    "door_debounce_ms", "turn_signal_on_ms", "debounce_ms",
                    "beep_min_interval_ms", "beep_ms", "only_highest"];
    const fw = {};
    for (const f of FIELDS) {
      // 三种写法都要认：`120.0f`（float 后缀）、`20000`（整数）、`true`（bool）。
      // ★ 第一版正则把 `5800.0f` 整串丢给 Number() ⇒ NaN，用例报
      //   "期望 null 得到 5800"（NaN 经 JSON.stringify 就是 null）—— 记在这儿。
      const m = new RegExp("\\b" + f + "\\s*=\\s*([-0-9.]+f?|true|false)\\s*;").exec(body[1]);
      ok(!!m, "alerts.h 里 " + f + " 有默认值");
      let v = NaN;
      if (m) {
        if (m[1] === "true") v = 1;
        else if (m[1] === "false") v = 0;
        else v = Number(m[1].replace(/f$/i, ""));
      }
      fw[f] = v;
    }

    // ① 编辑器页面的 defaultTheme().alerts
    const dz = /alerts:\s*\{([\s\S]*?)\n    \}/.exec(idxSrcA);
    ok(!!dz, "index.html 的 defaultTheme() 里有 alerts 段");
    const pageDef = new Function("return {" + dz[1].replace(/\/\/[^\n]*/g, "") + "};")();
    for (const f of FIELDS) eq(pageDef[f], fw[f], "index.html 的 alerts." + f);

    // ② theme-default.json（给人当模板、也当回归基线的那份）
    const tplAlerts = TJ.themeObject(TJ.parseThemeJson(
      fs.readFileSync(path.join(__dirname, "theme-default.json"), "utf8"))).alerts;
    ok(!!tplAlerts, "theme-default.json 里有 alerts 段");
    for (const f of FIELDS) eq(tplAlerts[f], fw[f], "theme-default.json 的 alerts." + f);
    eq(Object.keys(tplAlerts).length, FIELDS.length,
       "theme-default.json 的 alerts 段不多不少正好十个字段");

    // ③ 默认值必须落在固件钳制的安全范围内（钳制常量也从 alerts.h 抠）
    const rangeOf = (name) => {
      // 常量声明带类型后缀：`static const uint32_t kAlertsTurnMinMs = 1000u;`
      // ⇒ 后缀 `u`/`f`/`l` 都要吃掉（第一版没吃，两个边界都抠成 null）。
      const m = new RegExp("kAlerts" + name + "\\s*=\\s*([-0-9.]+)[uUlLfF]*\\s*;").exec(aSrc);
      return m ? Number(m[1]) : null;
    };
    const inRange = (v, lo, hi, what) => {
      ok(lo !== null && hi !== null, what + " 的两个边界都抠到了");
      ok(v >= lo && v <= hi, what + " 的默认值 " + v + " 落在 [" + lo + "," + hi + "] 内");
    };
    inRange(fw.overspeed_kmh, rangeOf("OverspeedMinKmh"), rangeOf("OverspeedMaxKmh"), "超速");
    inRange(fw.redline_rpm, rangeOf("RedlineMinRpm"), rangeOf("RedlineMaxRpm"), "红区");
    inRange(fw.turn_signal_on_ms, rangeOf("TurnMinMs"), rangeOf("TurnMaxMs"), "转向忘关");
    inRange(fw.beep_ms, rangeOf("BeepMinMs"), rangeOf("BeepMaxMs"), "响一声");

    // ④ 两个"下限不是 0"的滑块：页面能拖出来的最小值**不许低于固件的下限**
    //    —— 否则"页面上设成 0、设备上被钳成 1"这种不一致迟早被人当 bug 报上来。
    const turnSlider = /slider\(g6, "转向忘关 s"[\s\S]*?,\s*(\d+),\s*(\d+),\s*(\d+)\)\)/.exec(idxSrcA);
    ok(!!turnSlider, "找到「转向忘关 s」滑块");
    if (turnSlider) {
      ok(Number(turnSlider[1]) * 1000 >= rangeOf("TurnMinMs"),
         "转向忘关滑块下限(" + turnSlider[1] + " s)不低于固件下限");
    }
    const beepSlider = /slider\(g6, "响一声 ms"[\s\S]*?,\s*(\d+),\s*(\d+),\s*(\d+)\)\)/.exec(idxSrcA);
    ok(!!beepSlider, "找到「响一声 ms」滑块");
    if (beepSlider) {
      ok(Number(beepSlider[1]) >= rangeOf("BeepMinMs"),
         "响一声滑块下限(" + beepSlider[1] + " ms)不低于固件下限");
    }

    // ⑤ 固件那边真的会读这一段（不是"编辑器导出了但没人用"）
    const tsSrc = fs.readFileSync(path.join(repo2, "lib", "themetool", "theme_store.cpp"), "utf8");
    ok(tsSrc.indexOf("theme_parse_alerts_json") >= 0, "theme_store.cpp 实现了 alerts 解析");
    for (const f of FIELDS) {
      ok(tsSrc.indexOf('"' + f + '"') >= 0, "theme_store.cpp 认识字段 " + f);
    }
    ok(tsSrc.indexOf("alerts_config_clamp") >= 0, "解析后调用了 alerts_config_clamp()");
    // 设备端启动路径上真的把它接上了（main.cpp 调 theme_load_alerts）
    const mainSrc = fs.readFileSync(path.join(repo2, "src", "main.cpp"), "utf8");
    ok(mainSrc.indexOf("theme_load_alerts(") >= 0, "main.cpp 在启动时应用 alerts 段");
    ok(mainSrc.indexOf("g_alerts.setConfig(") >= 0, "main.cpp 把解析结果设进 g_alerts");
  }

console.log("\n" + "=".repeat(56));
if (fail === 0) console.log("全部通过:" + pass + " 项断言");
else {
  console.log("失败 " + fail + " 项 / 共 " + (pass + fail) + " 项:");
  failures.forEach(f => console.log("  - " + f));
}
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
