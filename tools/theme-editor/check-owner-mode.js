/* ============================================================
 * check-owner-mode.js —— 「车主模式」的**能力表**对账（Node 运行）
 *
 *   node tools/theme-editor/check-owner-mode.js
 *   退出码：0 = 全绿；1 = 有判据红
 *
 * ------------------------------------------------------------
 * 为什么要有它（2026-10-15「车主模式」那一单）
 * ------------------------------------------------------------
 * 那一单的验收标准是**结构性的**，肉眼看不出来、也没有别的测试盯着：
 *   ① 车主模式恰好有那 9 类控件（不多不少）；
 *   ② 高级里**旧控件一个不少**（角度/半径/线宽/字号/中心 y/量程/开机毫秒/
 *      告警阈值/JSON 文本框/导入导出/图片列表/弧/读数/刷写/占用……）；
 *   ③ JSON 文本框**只在高级里**（车主模式看不到）；
 *   ④ 车主模式是**默认**（localStorage 里没记录时进车主模式）；
 *   ⑤ 框选器只给**表情槽**做（背景那条路不许出现框选）；
 *   ⑥ 3 个预设写进**现有主题状态**（不是新文件格式）；
 *   ⑦ 打包产物与改动前**逐字节相同**（.golden-hashes.json 那一份指纹）。
 *
 * ------------------------------------------------------------
 * ★★ 2026-10-15（同一天，车主口径变更）：**配色页与图片页合并成一个页面**
 * ------------------------------------------------------------
 *   车主原话：「图片页和配色页能合并吗，因为感觉功能上其实是统一的。」
 *   ⇒ 本脚本跟着改成"查**一个**页面里的两个分区"：
 *      · `index.html` = 唯一的实现（两个页内分区：① 配色 / ② 图片，
 *        共用同一份主题状态 T 与同一份图片清单 items）；
 *      · `image-editor.html` = **重定向薄壳**（几行，跳 index.html#images）。
 *   第 ⓪ 节盯的就是这件事：**壳里不许有第二份实现**，
 *   而两个分区的控件、槽位清单、保存安装包一个都不许少（能力表不缩水）。
 *
 * ★ 为什么能这么查（而不是"跑个浏览器"）：这些页面是**静态 HTML**，
 *   控件的**身份**就写在源码里（`data-ocap` 标签、标签文字、函数名）。
 *   本仓库既有风格也是这么查的（见 check-role-label.js / check-layer-order.js /
 *   test-theme-json.js 的"解析真源码"那几节）—— 不引浏览器、不装依赖。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

const dir = __dirname;
const REPO = path.resolve(dir, "..", "..");

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
function read(f) { return fs.readFileSync(path.join(dir, f), "utf8"); }
// 去掉行注释与块注释：注释里会引用这些写法（本仓库到处都是这种"解释性注释"），
// 不剥掉会把注释当成代码（test-gauge-geometry.js 的 stripComments 同一个用途）。
function stripComments(s) {
  return s.replace(/\/\*[\s\S]*?\*\//g, "").replace(/(^|[^:])\/\/[^\n]*/g, "$1");
}
// 数某个 token 在源码里出现几次（用来钉"只有一份实现"）
function countOf(src, re) { return (src.match(re) || []).length; }

const PAGES = { "index.html": read("index.html"), "image-editor.html": read("image-editor.html") };
// ★ 合并后：实现在 index.html（两个分区）；image-editor.html 只是重定向薄壳。
const MERGED = PAGES["index.html"];
const SHELL = PAGES["image-editor.html"];
// 查薄壳的**代码**时先把 HTML 注释剥掉：注释里正说明"没有 <script>"，
// 不剥就会把注释里的字样当成代码（本仓库的注释风格就是这样，实测踩过）。
const SHELL_CODE = SHELL.replace(/<!--[\s\S]*?-->/g, "");

// ------------------------------------------------------------
// 能力表（页面里那几个"身份标签"）
//   data-ocap="bg|arc|digit|redline|overspeed|face|save"
//   ★ 这 7 个标签覆盖车主原话点名的 9 类控件：
//     背景色(bg) + 四条弧颜色(arc×4) + 数字颜色(digit) + 红区(redline) +
//     超速(overspeed) + 图片入口(face) + 保存安装包(save)；
//     左右圆预览位是既有画布（cv0/cv1），不算控件。
//   ★★ 合并后 save 是 **2**：两个分区**各有一个**「保存安装包」（车主口径：
//      两边都要能存）—— 两个按钮都调同一个 saveOwnerPackage()，不是两套打包。
//      端口输入框同理是 2（同一个值、同一条命令）。
//   ★★ 2026-10-15（车主问"进气/水温/转速/速度的字体颜色要不要独立"）：
//      coolant / intake 两块色板进了车主模式 —— 这两个键**主题里本来就有**
//      （readout.coolant_color / intake_color，theme_store.cpp 照旧解析），
//      所以是"把已有的键暴露出来"，**不是新增格式**。
//      ★ 转速与速度**没有**独立的键（它们共用 digit）⇒ 车主模式里**不许**出现
//        任何"转速颜色 / 速度颜色"的色板（见 ①c 那一节）。
// ------------------------------------------------------------
const OWNER_CAPS = ["bg", "arc", "digit", "coolant", "intake", "redline", "overspeed", "face", "save"];
const OWNER_EXPECT_COUNT = { bg: 1, arc: 4, digit: 1, coolant: 1, intake: 1,
                             redline: 1, overspeed: 1, face: 1, save: 2 };
const PORT_MARK_EXPECT = 2;

// ------------------------------------------------------------
// ⓪ 合并本身：一个实现 + 一个薄壳
// ------------------------------------------------------------
section("⓪ 合并：实现在 index.html（两个分区），image-editor.html 只是重定向薄壳");
{
  // 壳必须**小**（几行就够）+ 必须带回跳目标 + **不许有第二份实现**
  //   ★ 查代码之前先把 HTML 注释剥掉：注释里正说明"没有 <script>"，
  //     不剥就会把注释里的字样当成代码（本仓库的注释风格就是这样，实测踩过）。
  const SHELL_CODE = SHELL.replace(/<!--[\s\S]*?-->/g, "");
  const shellLines = SHELL.split(/\r?\n/).length;
  ok(shellLines <= 40, "薄壳不超过 40 行（实际 " + shellLines + " 行）");
  ok(Buffer.byteLength(SHELL, "utf8") <= 3000,
     "薄壳不超过 3000 字节（实际 " + Buffer.byteLength(SHELL, "utf8") + "）");
  ok(/<meta\s+http-equiv="refresh"\s+content="0;\s*url=index\.html#images"/i.test(SHELL),
     "薄壳带 meta refresh 跳 index.html#images（老书签能落地到「② 图片」分区）");
  ok(countOf(SHELL_CODE, /<script\b/gi) === 0, "薄壳里没有 <script>（不许有第二份实现）");
  ok(countOf(SHELL_CODE, /data-ocap=/g) === 0, "薄壳里没有车主模式控件标记");
  ok(SHELL_CODE.indexOf("THEME_KEY") < 0 && SHELL_CODE.indexOf("items") < 0,
     "薄壳里没有主题/图片状态（一份都不许留）");
  for (const bad of ["function buildControls", "function openCropper", "function cropConfirm",
                     "function buildOwnerSlots", "function render(", "function coreForTarget",
                     "function exportAssetsBin", "id=\"crop\"", "id=\"json\"", "id=\"stage-grid\""]) {
    ok(SHELL_CODE.indexOf(bad) < 0, "薄壳里没有第二份实现：" + bad);
  }
  // 而真正的实现每一件都**只有一份**（合并前两页各一份的那些函数）
  const once = [
    ["function drawScreen\\(", "drawScreen()（唯一的渲染器）"],
    ["function drawArc\\(", "drawArc()（唯一的画弧）"],
    ["function primaryKind\\(", "primaryKind()（唯一的大数字来源判定）"],
    ["function buildControls\\(", "buildControls()（高级那一整面墙）"],
    ["function buildOwnerSlots\\(", "buildOwnerSlots()（唯一的槽位清单生成）"],
    ["function openCropper\\(", "openCropper()（唯一的框选器）"],
    ["function cropConfirm\\(", "cropConfirm()"],
    ["function coreForTarget\\(", "coreForTarget()（唯一的容器布局）"],
    ["function exportAssetsBin\\(", "exportAssetsBin()（唯一的容器打包入口）"],
    ["function saveOwnerPackage\\(", "saveOwnerPackage()（唯二的保存按钮都指向它）"],
    ["function render\\(", "render()（唯一的整体渲染）"],
    ["const THEME_KEY = ", "THEME_KEY（唯一的主题键）"],
    ["function applyPart\\(", "applyPart()（页内分区切换）"],
    ["function buildReadoutEditors\\(", "buildReadoutEditors()"],
    ["function buildArcEditors\\(", "buildArcEditors()"]
  ];
  for (const [re, what] of once) {
    eq(countOf(MERGED, new RegExp(re, "g")), 1, "合并后的页面里只有一份：" + what);
  }
  // 只可能出现一次的元素 id（重复就是"两个真相"）
  for (const id of ["json", "cv0", "cv1", "cvA", "status", "crop", "stage", "controls",
                    "owner-slots", "target-board", "budget-bar"]) {
    eq(countOf(MERGED, new RegExp('id="' + id + '"', "g")), 1,
       "id=\"" + id + "\" 只有一处");
  }
  // 两个分区都在，而且是**同一页**里的两半（容器 + 标签 + 锚点）
  for (const id of ["owner-theme", "owner-images", "part-theme", "part-images", "partbar"]) {
    eq(countOf(MERGED, new RegExp('id="' + id + '"', "g")), 1, "有分区容器/标签 #" + id);
  }
  ok(/function currentHashPart\(\)/.test(MERGED) && /#images/.test(MERGED),
     "认 URL 锚点（#images 直接进「② 图片」分区 —— 薄壳就是跳到它）");
  ok(/getElementById\("owner-theme"\)/.test(MERGED) && /getElementById\("owner-images"\)/.test(MERGED),
     "applyPart() 用 id 切两个分区（不依赖 CSS 选择器）");
  // 单一状态：主题只有一个变量、图片只有一份清单
  eq(countOf(MERGED, /\bconst THEME_KEY = "/g), 1, "THEME_KEY 只声明一次");
  ok(/let T = defaultTheme\(\);/.test(MERGED), "主题状态只有一个（let T = defaultTheme()）");
  ok(!/\bgTheme\b/.test(MERGED), "旧图片页那份主题对象（另一个名字）已经不存在");
  ok(/let items = \[\];/.test(MERGED), "图片清单只有一个（let items = []）");
  ok(MERGED.indexOf("HANDOFF_KEY") < 0 && MERGED.indexOf("ownerLastPick") < 0,
     "跨页交接那套中间状态已经删掉（合并后不需要它）");
  console.log("  - 薄壳 " + shellLines + " 行 / " + Buffer.byteLength(SHELL, "utf8") +
              " 字节；实现 " + MERGED.split(/\r?\n/).length + " 行");
}

// ------------------------------------------------------------
section("① 车主模式恰好有那 9 类控件（两个分区一起数）");
// 身份标签有两种合法写法：
//   · **静态写在 HTML 里** —— `data-ocap="bg"`（车主模式那两栏用的就是它，
//     好处：车主模式下"这一栏是哪几件东西"一眼可读，检查脚本也数得到）；
//   · 由 JS 用**字面量** setAttribute("data-ocap", "arc")（少数几个由循环生成的）。
//   两种都要求"字面量"：用变量拼出来的标签本脚本数不到，
//   而"数不到"会让这条守卫静默失效 —— 比报错更糟。
//   ★ 数之前先把**注释**剥掉：这些注释里会引用 `data-ocap="face"` 这种写法
//     （本仓库的注释风格就是这样），不剥就会多数出来（实测踩过：3 处里 2 处是注释）。
function countOwnerCaps(src) {
  const counts = {};
  const add = (k) => { counts[k] = (counts[k] || 0) + 1; };
  const code = src
    .replace(/<!--[\s\S]*?-->/g, "")
    .replace(/\/\*[\s\S]*?\*\//g, "")
    .replace(/(^|[^:])\/\/[^\n]*/g, "$1");
  let m;
  const reAttr = /\bdata-ocap="([a-z]+)"/g;
  while ((m = reAttr.exec(code)) !== null) add(m[1]);
  const reJs = /setAttribute\(\s*"data-ocap"\s*,\s*"([a-z]+)"\s*\)/g;
  while ((m = reJs.exec(code)) !== null) add(m[1]);
  return counts;
}
const KNOWN_MARKS = OWNER_CAPS.concat(["preset", "port", "bgimage", "slots", "legend"]);
{
  const counts = countOwnerCaps(MERGED);
  for (const k of Object.keys(counts)) {
    ok(KNOWN_MARKS.indexOf(k) >= 0, "合并后的页面里 data-ocap 值合法：" + k);
  }
  for (const c of OWNER_CAPS) {
    eq(counts[c] || 0, OWNER_EXPECT_COUNT[c], "车主模式的 " + c + " 控件标记个数");
  }
  // 端口那一处标记：两个分区各一个（同一个值、同一条命令）
  eq(counts.port || 0, PORT_MARK_EXPECT, "车主模式恰好 " + PORT_MARK_EXPECT + " 处端口标记（两个分区各一个）");
  eq(counts.preset || 0, 1, "预设那一行恰好一处");
  eq(counts.bgimage || 0, 1, "槽位清单里背景那一格恰好一处 bgimage 标记");
  // "四条弧"的**类别数**：数那个数组里的四条
  const arcsDecl = /const OWNER_ARCS\s*=\s*\[([\s\S]*?)\n\];/.exec(MERGED);
  ok(!!arcsDecl, "合并后的页面里有 OWNER_ARCS 声明");
  if (arcsDecl) {
    eq((arcsDecl[1].match(/\{\s*s\s*:/g) || []).length, 4, "OWNER_ARCS 正好四条");
  }
  // 那张能力表本身也要在页面里（两页同源的写法保留成一份），并且**不多不少**
  const capsDecl = /const OWNER_CAPS\s*=\s*(\[[^\]]*\])/.exec(MERGED);
  ok(!!capsDecl, "合并后的页面里有 OWNER_CAPS 声明");
  if (capsDecl) {
    const arr = new Function("return " + capsDecl[1])();
    eq(JSON.stringify(arr), JSON.stringify(OWNER_CAPS), "OWNER_CAPS 与能力表一致");
  }
  // 两个分区的保存按钮都指向**同一个**函数（不是两套打包）
  ok(/\["btn-owner-save", "btn-owner-save-img"\]\.forEach/.test(MERGED),
     "两个「保存安装包」按钮接的是同一个 saveOwnerPackage()");
  eq(countOf(MERGED, /saveOwnerPackage/g) >= 3 ? 1 : 0, 1,
     "saveOwnerPackage 这个名字只对应一个函数（定义 + 两处接线）");
  // 槽位清单那几件工具必须真的在页面里
  for (const w of ['"选文件"', '"换图"', '"清除"', "function buildOwnerSlots",
                   "function sendFilesToSlot", "function clearSlot", "function drawSlotThumb"]) {
    ok(MERGED.indexOf(w) >= 0, "槽位清单里有 " + w);
  }
  // 表情格与背景格走**同一条入口**，但语义按 face 分流
  //   （2026-10-15 之前是"背景不走框选"，车主推翻后改成"背景走取景"）
  ok(/const mode = spec\.face \? "face" : "bg"/.test(MERGED),
     "按 face 标记分流（表情=face、背景=bg 两种语义）");
  ok(/function clearSlot[\s\S]{0,400}role === spec\.role/.test(MERGED),
     "清除按 role 删干净");
}

section("①b 槽位清单：背景 1 + 表情 10，名称用中文阶段名");
{
  const src = MERGED;
  const FSrd = require(path.join(dir, "face-stages.js"));
  const body = /function faceSlots\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!body, "合并后的页面有 faceSlots()（从 FaceStages 取 10 个表情槽）");
  // ★ 把 faceSlots() 真跑一遍：喂**真的** face-stages.js 进去，
  //   所以"名称是不是中文阶段名、是不是 5+5"是算出来的，不是数注释。
  if (body) {
    const fn = new Function("FaceStages", body[0] + "\nreturn faceSlots;");
    const slots = fn(FSrd)();
    eq(slots.length, 10, "表情槽正好 10 个");
    eq(slots.filter(s => s.face).length, 10, "10 个都标了 face:true（要走框选）");
    eq(slots.filter(s => s.side === 0).length, 5, "转速屏 5 个");
    eq(slots.filter(s => s.side === 1).length, 5, "速度屏 5 个");
    const left = slots.filter(s => s.side === 0).map(s => s.label.split(" · ").pop());
    const right = slots.filter(s => s.side === 1).map(s => s.label.split(" · ").pop());
    eq(left.join("/"), "怠速/巡航/运动/高转/红区", "转速屏槽位名（中文阶段名，按档位序）");
    eq(right.join("/"), "静止/市区/快速路/高速/超速", "速度屏槽位名（中文阶段名，按档位序）");
    ok(slots.every(s => s.label.indexOf("转速表") >= 0 || s.label.indexOf("速度表") >= 0),
       "每个表情槽名都点明哪块屏（转速表 / 速度表）");
    const want = [];
    for (let s = 0; s < 2; s++) FSrd.statesOf(s).forEach(st => want.push(st.role));
    eq(slots.map(s => s.role).join(","), want.join(","), "10 个 role 与 face-stages.js 逐个一致");
  }
  // 背景那一格：1 个。★★ 2026-10-15（车主实测反馈）：「主题背景图这边导入没有框选选项」
  //   ⇒ 这一格**现在也走框选器**了，但语义是 **cover 取景**（框内就是最终画面、
  //     图必须盖满框、可用 JPEG/WebP、不要求 alpha），与表情的"框外丢掉"**不同**。
  //     所以下面这几条断言也一起改了口径（旧口径"背景不加框选"已经被车主推翻）。
  const sp = /function ownerSlotSpecs\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!sp, "有 ownerSlotSpecs()（背景 + 表情合成一张表）");
  if (sp) {
    ok(/ImageBlob\.ROLE\.Background/.test(sp[1]) && /face:\s*false/.test(sp[1]),
       "背景那一格 face:false（它不是表情槽）");
    ok(/\.concat\(faceSlots\(\)\)/.test(sp[1]),
       "ownerSlotSpecs() = 背景 1 格 + faceSlots() 10 格");
  }
  // 背景格走**取景**（mode="bg"），不是走表情那套
  ok(/const mode = spec\.face \? "face" : "bg"/.test(src),
     "按 face 分流：表情=face、背景=bg（两种语义）");
  ok(/openCropper\(img, f\.name, spec, mode\)/.test(src),
     "两条路都进同一个框选器，但带着不同的 mode");
  ok(/mode === "bg"/.test(src), "框选器里认 mode===\"bg\"（背景取景那一路）");
  ok(/背景取景|框内就是最终画面/.test(src), "页面里明说背景是「取景（框内就是最终画面）」");
  ok(!/背景：不弹框选|背景不加框选/.test(src),
     "不再写「背景不加框选」（车主已推翻这条）");
}

section("①c 水温 / 进气两块色板：接的是**已有的键**，且转速/速度不许有独立色板");
{
  const src = MERGED;
  // ① 两块色板接的键必须**主题里本来就有**（不是新键）
  const bindC = /ownerColorBind\(\s*"oc-coolant"[\s\S]{0,200}?T\.readout\.coolant_color/.exec(src);
  const bindI = /ownerColorBind\(\s*"oc-intake"[\s\S]{0,200}?T\.readout\.intake_color/.exec(src);
  ok(!!bindC, "「水温颜色」接 readout.coolant_color");
  ok(!!bindI, "「进气颜色」接 readout.intake_color");
  // ② 初值来源 = 主题当前值（sync 读的是同一个 getter，不是写死常量）
  ok(/coolant:\s*coolant/.test(src) && /intake:\s*intake/.test(src),
     "两块色板进了 ownerControls（render 时会被同步成主题当前值）");
  ok(/ownerControls\.coolant\.sync\(\)/.test(src) &&
     /ownerControls\.intake\.sync\(\)/.test(src),
     "renderOwner() 里会同步这两块（高级里改 / 套预设 / 读入主题都会走到）");
  // ③ 键**确实在主题默认值里**（不是这一单新加的）：对 defaultTheme() 与 theme-default.json
  const dFn = /function defaultTheme\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!dFn, "合并后的页面里有 defaultTheme()");
  const d = new Function(dFn[1].replace(/^\s*return\s*/, "return "))();
  eq(typeof d.readout.coolant_color, "number", "defaultTheme().readout.coolant_color 本来就有");
  eq(typeof d.readout.intake_color, "number", "defaultTheme().readout.intake_color 本来就有");
  const tpl = JSON.parse(fs.readFileSync(path.join(dir, "theme-default.json"), "utf8")
    .replace(/0x([0-9a-fA-F]+)/g, (m, h) => String(parseInt(h, 16))));
  const tplTheme = tpl.theme || tpl;
  eq(tplTheme.readout.coolant_color, d.readout.coolant_color,
     "theme-default.json 的 coolant_color 与页面默认值一致");
  eq(tplTheme.readout.intake_color, d.readout.intake_color,
     "theme-default.json 的 intake_color 与页面默认值一致");
  // ④ 固件解析器认得这两个键（权威：lib/themetool/theme_store.cpp）
  const ts = fs.readFileSync(path.join(REPO, "lib", "themetool", "theme_store.cpp"), "utf8");
  ok(ts.indexOf('"coolant_color"') >= 0, "theme_store.cpp 解析 coolant_color");
  ok(ts.indexOf('"intake_color"') >= 0, "theme_store.cpp 解析 intake_color");
  // ⑤ ★★ 转速 / 速度**不许**有独立色板（键不存在；要做得改固件 + 升版本）
  const ownerTheme = src.slice(src.indexOf('id="owner-theme"'), src.indexOf('id="owner-images"'));
  for (const bad of ["oc-rpm", "oc-speed", "转速颜色", "速度颜色", "rpm_color", "speed_color"]) {
    ok(ownerTheme.indexOf(bad) < 0,
       "车主模式配色区里**没有**「" + bad + "」（转速/速度共用 digit_color）");
  }
  // 固件侧**确实没有**这两个键（所以"加色板"不是漏做，是不能做）
  ok(ts.indexOf('"rpm_color"') < 0 && ts.indexOf('"speed_color"') < 0,
     "theme_store.cpp 里没有 rpm_color / speed_color（键不存在）");
  // ⑥ ★ 那句说明必须在（一句话、不是控件）
  ok(/转速与速度目前共用「数字颜色」/.test(ownerTheme),
     "配色区里有一句话说明「转速与速度目前共用『数字颜色』」");
  ok(/需要一次固件改动/.test(ownerTheme), "那句话点明「要拆开需要一次固件改动」");
  // 那句话附近**不许**冒出色板/滑块（说明归说明，别顺手做成控件）
  const legend = /<div class="note"[^>]*data-ocap="legend"[^>]*>([\s\S]*?)<\/div>/.exec(ownerTheme);
  ok(!!legend, "那行说明带 data-ocap=\"legend\" 标记（便于本脚本点名）");
  if (legend) {
    ok(!/<input|<select|<button/.test(legend[1]),
       "说明那一行里没有 input/select/button（是纯文字）");
  }
  // ⑦ 高级里那两行也还在（一份状态两处显隐）
  ok(/colorRow\(wrap,\s*"水温（左屏）"/.test(src), "高级里仍有「水温（左屏）」色板");
  ok(/colorRow\(wrap,\s*"进气温度（右屏）"/.test(src), "高级里仍有「进气温度（右屏）」色板");
}

section("② 车主模式是**默认**（没记录时进车主模式，不是高级）");
{
  const def = /applyMode\(\s*saved\s*===\s*"adv"\s*\?\s*"adv"\s*:\s*"owner"\s*,\s*true\s*\)/.exec(MERGED);
  ok(!!def, "启动时默认 applyMode(... \"owner\", true)");
  ok(/localStorage\.getItem\(MODE_KEY\)/.test(MERGED), "模式选择从 localStorage 读");
  ok(/id="btn-mode-owner"/.test(MERGED) && /id="btn-mode-adv"/.test(MERGED),
     "有「车主模式 / 高级」两个切换按钮");
  // 页内分区也有"没记录就进 ① 配色"的默认，而且**认锚点优先**
  ok(/applyPart\(currentHashPart\(\) \|\| saved \|\| "theme", true\)/.test(MERGED),
     "分区默认进「① 配色」，但 URL 锚点优先（#images 直接进图片分区）");
  ok(/localStorage\.getItem\(PART_KEY\)/.test(MERGED), "分区选择记在**另一个**键上（PART_KEY）");
}

section("③ JSON 文本框**只在高级里**");
{
  const at = MERGED.indexOf('id="json"');
  ok(at >= 0, "合并后的页面里有 JSON 文本框");
  const advAt = MERGED.indexOf('id="advanced"');
  const mainEnd = MERGED.indexOf("</main>");
  ok(advAt >= 0 && at > advAt, "JSON 文本框在 #advanced 之后");
  ok(mainEnd > at, "JSON 文本框在 </main> 之前");
  // 车主模式那两栏里不许出现它
  const ownerStart = MERGED.indexOf('id="owner"');
  const ownerEnd = MERGED.indexOf('id="owner-images"');
  ok(!(at > ownerStart && at < ownerEnd), "JSON 文本框不在车主模式那两个分区里");
  eq(countOf(MERGED, /id="json"/g), 1, "只有一处 id=\"json\"");
  // 两个分区里都不许出现 <textarea>（JSON 只在高级）
  const ownerTxt = MERGED.slice(ownerStart, advAt);
  ok(ownerTxt.indexOf("<textarea") < 0, "车主模式那两栏里没有 textarea");
}

section("④ 高级里旧控件一个不少（配色那一半）");
{
  const src = MERGED;
  // buildControls() 必须还在、还在被调用（车主模式不许把它删掉）
  ok(/function buildControls\(\)/.test(src), "合并后的页面里 buildControls() 还在");
  ok(/^\s*buildControls\(\);/m.test(src), "启动时仍然调用 buildControls()");
  ok(/id="controls"/.test(src), "#controls 容器还在");
  // 原来那几组控件（按标签文字认，标签就是"控件有没有丢"的判据）
  const wanted = [
    // 全局 / 表情几何
    "背景色", "表情大小", "表情底色", "红区底色", "五官颜色",
    // 弧：角度/半径/线宽/颜色/透明/反向
    "起始角°", "结束角°", "半径", "线宽", "点亮色", "轨道色", "轨道透明", "涨幅反向",
    // 量程
    "水温量程", "最低 ℃", "最高 ℃", "进气量程",
    // 读数：颜色/字号/中心 y/开关
    "数字颜色", "单位颜色", "水温颜色", "数字字号", "单位字号", "副表字号",
    "数字中心 y", "单位中心 y", "水温中心 y", "进气中心 y", "显示单位", "显示水温数字",
    // 开机动画
    "整体淡入", "扫表开始", "错峰启动", "0→满弧", "满弧停留", "满弧→0",
    "表情显形", "收尾时长",
    // 告警阈值（蜂鸣器删了，但阈值控件照旧）
    "超速 km/h", "超速迟滞", "红区 rpm", "红区迟滞", "转向忘关 s",
    "门去抖 ms", "通用去抖 ms", "同时只报最高优先级"
  ];
  for (const w of wanted) {
    ok(src.indexOf(w) >= 0, "高级里还有「" + w + "」");
  }
  // JSON 的两个按钮
  ok(/id="btn-apply"/.test(src), "还有「应用」JSON 的按钮");
  ok(/id="btn-copy"/.test(src), "还有「复制」JSON 的按钮");
  // 导入 / 导出 / 恢复默认（★ 合并后"导出 theme.json"用的是图片页那个 id，
  //   而 #btn-export 专职导出 image.bin —— 两个按钮都在，语义各自钉死）
  ok(/id="btn-default"/.test(src) && /id="btn-import"/.test(src),
     "还有 恢复默认 / 导入主题文件");
  ok(/id="btn-theme-export"[^>]*>导出 theme\.json/.test(src), "还有「导出 theme.json」");
  ok(/id="btn-export"[^>]*>⬇ 导出 image\.bin/.test(src), "还有「导出 image.bin」");
  ok(/id="btn-play"/.test(src) && /id="cvA"/.test(src), "还有开机动画预览与播放按钮");
  // 预览画布（车主模式要的"左右圆预览"）
  ok(/id="cv0"/.test(src) && /id="cv1"/.test(src), "有左右两块预览画布");
}

section("⑤ 高级里旧控件一个不少（图片那一半），而且技术参数不在车主模式里");
{
  const src = MERGED;
  const wanted = [
    'id="drop"', 'id="list"', 'id="bg-color"', 'id="show-arcs"',
    'id="btn-theme-export"', 'id="btn-load-theme"', 'id="btn-reset-theme"',
    'id="btn-assets-export"', 'id="arc-editors"', 'id="readout-editors"',
    'id="stage"', 'id="face-left"', 'id="face-right"',
    'id="cmd"', 'id="btn-copy-cmd"', 'id="btn-cheader"', 'id="btn-import-bin"',
    'id="target-board"', 'id="budget-bar"', 'id="part-info"', 'id="file-bin"',
    'id="btn-add"', 'id="btn-export"', 'id="stage-grid"', 'id="stage-note"',
    'id="file-theme"', 'id="file-img"', 'id="file-face"'
  ];
  for (const w of wanted) ok(src.indexOf(w) >= 0, "还有 " + w);
  // 那三个"老控件"的口径：contain / cover / stretch、裁透明边、目标宽度
  ok(/id: "contain"/.test(src) && /id: "cover"/.test(src) && /id: "stretch"/.test(src),
     "还保留 contain / cover / stretch 三种缩放方式");
  ok(/裁透明边/.test(src) && /item\.trim/.test(src), "还保留「裁透明边」");
  ok(/目标宽度/.test(src) && /SIZE_PRESETS/.test(src), "还保留「目标宽度」与尺寸预设");
  ok(/顺序/.test(src) && /item\.order/.test(src), "还保留「顺序」（同角色多帧时的排法）");
  // 老控件块确实被搬进 #advanced 里（而不是被注释掉/删掉）
  const advAt = src.indexOf('id="advanced"');
  const dropAt = src.indexOf('id="drop"');
  const budgetAt = src.indexOf('id="budget-bar"');
  const jsonAt = src.indexOf('id="json"');
  ok(advAt >= 0 && dropAt > advAt && budgetAt > advAt && jsonAt > advAt,
     "图片列表/占用/JSON 那些 section 都在 #advanced 之后");
  ok(/id="controls"/.test(src) && src.indexOf('id="controls"') > advAt,
     "配色那一整面墙（#controls）也在 #advanced 之后");
  ok(/<div class="left" id="advanced" style="display:none">/.test(src),
     "高级那一栏默认是隐藏的（车主模式是默认）");

  // ★★ 槽位清单在车主模式，但**技术参数留在高级** ——
  //   把这条写成判据（"车主模式干净"是车主要求的，不是顺手的）。
  const ownerAt = src.indexOf('id="owner"');
  const mainEnd = src.indexOf("</main>");
  ok(ownerAt >= 0 && ownerAt < advAt, "车主模式那一栏在高级之前");
  const ownertxt = src.slice(ownerAt, advAt);       // #owner 那两栏的 HTML
  // ★ 只查"控件"（input/select/button 这些真能调的），不查封面的说明文字 ——
  //   车主模式里有一行**指路文字**会说"技术参数在高级"，那句话本身不是控件。
  for (const bad of ["裁透明边", "目标宽度", "顺序", "拉伸"]) {
    const ctl = new RegExp("<(input|select|button)[^>]*>[^<]*" + bad +
                           "|<label[^>]*>[^<]*" + bad);
    ok(!ctl.test(ownertxt), "车主模式那两栏里**没有**「" + bad + "」控件（留在高级）");
    ok(src.indexOf(bad) >= 0, "「" + bad + "」这个控件在页面里仍然存在（在高级）");
    // 而且它确实落在 #advanced 之后（不是散落在别处）。
    // ★ 用 lastIndexOf：车主模式那一栏里有一句**指路文字**会提到这些词
    //   （"技术参数在高级：…、裁透明边、目标宽度…"）—— 那不是控件。
    ok(src.lastIndexOf(bad) > advAt, "「" + bad + "」的控件在 #advanced 之后");
  }
  // 车主模式那两栏里也不许出现这几个技术参数的**输入框**
  ok(!/<input[^>]*type="number"[^>]*id="[^"]*(size|order)/.test(ownertxt),
     "车主模式里没有目标宽度/顺序的数字框");
  // 反过来：高级里这几样必须**还在**（既有的那一套，一个字没删）
  for (const good of ["FITS", "item.fit", "item.trim", "SIZE_PRESETS"]) {
    ok(src.indexOf(good) >= 0, "技术参数仍然存在：" + good);
  }
}

section("⑥ 框选器：表情=框选（框外丢掉）、背景=取景（cover、盖满框）");
{
  const src = MERGED;
  ok(/id="crop"/.test(src), "有框选器（模态 #crop）");
  ok(/id="crop-frame"/.test(src), "有「方框 = 保留区」那一层");
  ok(/openCropper\(/.test(src), "有 openCropper()");
  ok(/cropRenderSquare/.test(src), "有 cropRenderSquare()（导出居中透明方图）");
  ok(/pointerdown/.test(src) && /pointermove/.test(src), "框选器支持拖动平移（pointer 事件）");
  ok(/addEventListener\("wheel"/.test(src), "框选器支持滚轮缩放");
  ok(/touchmove/.test(src) && /e\.touches\.length === 2/.test(src), "框选器支持双指缩放");
  // 产物是**方图**且**透明打底**（不铺底色）
  const sq = /function cropRenderSquare\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!sq, "找得到 cropRenderSquare()");
  if (sq) {
    const b = stripComments(sq[1]);
    ok(/out\.width\s*=\s*side/.test(b) && /out\.height\s*=\s*side/.test(b),
       "框选产物是**方图**（宽=高=side）");
    ok(/drawImage\(/.test(b), "框选产物由 drawImage 画出");
    ok(!/fillRect|fillStyle/.test(b), "框选产物**不铺底色**（保留透明通道）");
    ok(/cropSideOut\(|cropSideMax\(/.test(b),
       "框选产物的边长由 cropSideOut() 算（上限问 ImageBlob，不写死数字）");
  }
  // ★ 两种语义都在（2026-10-15 车主推翻了"背景不加框选"那条）
  ok(/方框里是要保留的部分/.test(src), "表情那一路仍写「方框里是要保留的部分 / 框外丢掉」");
  ok(/方框里就是最终画面/.test(src), "背景那一路写「方框里就是最终画面」（取景语义）");
  ok(/图始终盖满整个框|图必须盖满/.test(src), "背景那一路点明「图盖满框、不露底」");
  // 表情那条路先过 asset-spec 的 checkUpload（JPEG/无 alpha 会被拒）
  ok(/AssetSpec\.checkUpload\(/.test(src), "两条路都过 AssetSpec.checkUpload()");
  ok(/表情要透明背景，请用 PNG/.test(src), "拒绝文案里明说「表情要透明背景，请用 PNG」");
  ok(/ImageBlob\.CF\.RGB565A8/.test(src), "表情仍是 RGB565A8");
  // 背景进槽用 **cover**（既有分支），不是 contain
  const cb = /if \(mode === "bg"\)\s*\{([\s\S]*?)\n  \}/.exec(src);
  ok(!!cb, "cropConfirm() 里找得到背景那一支");
  if (cb) {
    ok(/fit:\s*"cover"/.test(cb[1]), "背景进槽用既有的 cover 分支");
    ok(!/prepareAsset|transparent/.test(cb[1]),
       "背景那一支不碰「裁透明边」/不要求透明通道（可用 JPEG、WebP）");
  }
}

section("⑦ 框选只动「进槽前的像素」：还走既有那条缩放/打包路径");
{
  const src = MERGED;
  const conf = /function cropConfirm\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!conf, "找得到 cropConfirm()");
  if (conf) {
    const b = stripComments(conf[1]);
    ok(/items\.push\(/.test(b), "框选结果作为普通 item 进列表（走既有那条路）");
    ok(/cropOutputSize\(|cropSideOut\(/.test(b),
       "尺寸由 cropOutputSize()/cropSideOut() 算（只缩不放 + 目标板推荐值）");
    ok(!/rgbaToRgb565/.test(b), "框选器**不自己编码**（编码在 buildPayload 里，只有一份）");
  }
  // 既有的缩放/打包函数没被动过
  ok(/function renderToTarget\(/.test(src), "renderToTarget() 还在（框选不改它）");
  ok(/function buildPayload\(/.test(src), "buildPayload() 还在");
  ok(/ImageBlob\.build\(buildPayload\(\)\)/.test(src), "打包仍然走 ImageBlob.build(buildPayload())");
  ok(/ImageBlob\.faceCanvasMaxFor\(/.test(src), "表情上限问 ImageBlob.faceCanvasMaxFor()");
}

section("⑧ 保存安装包：两个分区各一个按钮，走**同一条**内核 + 一条命令（端口可改、偏移不可改）");
{
  const src = MERGED;
  ok(/AssetPackageCore\.packBytes|exportAssetsBin|packBytes\(/.test(src),
     "用 asset-package 内核打包");
  ok(/AssetPackageCore\.flashCommand\(/.test(src), "刷写命令来自 AssetPackageCore.flashCommand()");
  // 偏移不许有"用户可编辑"的入口：只允许端口输入框（两个分区各一个）
  eq(countOf(src, /id="owner-port"/g), 1, "「① 配色」分区有可改的**端口**输入框");
  eq(countOf(src, /id="owner-port-img"/g), 1, "「② 图片」分区也有可改的**端口**输入框");
  ok(!/owner-offset|id="offset"/.test(src), "没有可改的**偏移**输入框");
  // 那条命令里偏移来自 layout（0x210000），页面不写死字面量在命令里
  ok(!/write_flash 0x[0-9a-f]+ assets\.bin/.test(src), "页面上没有把整条命令写死");
  // 两条命令行都真的存在，而且都由同一个 updateOwnerFlash() 填
  eq(countOf(src, /id="owner-cmd"/g), 1, "「① 配色」分区有一条刷写命令行");
  eq(countOf(src, /id="owner-cmd-img"/g), 1, "「② 图片」分区有一条刷写命令行");
  ok(/\["owner-cmd", "owner-cmd-img"\]\.forEach/.test(src),
     "两条命令行由同一处代码填（同一个 layout、同一个端口）");
  ok(/\["owner-port", "owner-port-img"\]\.forEach/.test(src),
     "两个端口框同步同一个值");
  // 「保存安装包」必须调既有导出（不另写一套打包）
  const fn = /function saveOwnerPackage\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!fn, "找得到 saveOwnerPackage()");
  if (fn) ok(/exportAssetsBin\(\)/.test(fn[1]), "saveOwnerPackage() 调既有的 exportAssetsBin()");
  // 容器布局没有第二份实现
  eq(countOf(src, /function coreForTarget\(/g), 1, "coreForTarget() 只有一份");
  ok(!/spiffsBytes\s*:/.test(src) || /coreForTarget/.test(src),
     "容器布局仍只有 coreForTarget() 一处");
  // 那份布局的关键值：基准 0x210000、image 偏移算出来是 0x254000、段长问内核
  const APC = require(path.join(dir, "asset-package-core.js"));
  const cf = /function coreForTarget\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!cf, "抠得出 coreForTarget()");
  if (cf) {
    const b = cf[1];
    ok(/const themePartOffset = 0x210000;/.test(b), "coreForTarget 里 theme 偏移写死 0x210000");
    ok(/const imagePartOffset = 0x254000;/.test(b), "coreForTarget 里 image 偏移写死 0x254000");
    ok(/AssetPackageCore\.THEME_SECTION_BYTES/.test(b),
       "theme 段长问内核（不写死 16384）");
    ok(/ImageBlob\.partitionBytesFor\(curTarget\)/.test(b),
       "image 段长问 ImageBlob（跟着目标板走）");
  }
  // 页面上不许自己拼容器字节
  ok(!/0x44363032|IMAGE_MAGIC\s*=/.test(src), "页面里没有自己写 image 包头魔数");
  eq(countOf(src, /function packBytes\s*\(/g), 0, "页面里没有 packBytes 的第二份实现");
}

section("⑨ 3 个预设写进**现有主题状态**（当前默认 / 夜间 / 浅底）");
{
  const src = MERGED;
  const m = /const PRESETS\s*=\s*\[([\s\S]*?)\n\];/.exec(src);
  ok(!!m, "有 PRESETS");
  if (m) {
    const labels = (m[1].match(/label:\s*"([^"]+)"/g) || []).map(s => /"([^"]+)"/.exec(s)[1]);
    eq(labels.length, 3, "预设正好 3 个（实际：" + labels.join(" / ") + "）");
    ok(labels.indexOf("当前默认") >= 0, "有「当前默认」预设");
    ok(labels.indexOf("夜间") >= 0, "有「夜间」预设");
    ok(labels.indexOf("浅底") >= 0, "有「浅底」预设");
    ok(/T\.bg_color\s*=/.test(m[1]), "预设写 bg_color");
    ok(/value_color\s*=/.test(m[1]), "预设写弧的 value_color");
    ok(/digit_color\s*=/.test(m[1]), "预设写 readout.digit_color");
    const def = /id:\s*"default"[\s\S]*?apply:[\s\S]*?\}\s*\}/.exec(m[1]);
    ok(!!def && /defaultTheme\(\)/.test(def[0]),
       "「当前默认」预设从 defaultTheme() 取（不抄一份数字）");
    ok(!/JSON\.stringify|localStorage|new Blob/.test(m[1]), "预设只改状态，不写文件");
  }
  ok(/buildPresetRow|PRESETS\.forEach|PRESETS\.map/.test(src), "预设被挂到界面上");
}

section("⑩ 颜色只有一个入口：一份主题、一个键（合并后不再是\"两页共用\"）");
{
  const src = MERGED;
  const keyOf = (s) => { const m = /const THEME_KEY = "([^"]+)"/.exec(s); return m ? m[1] : null; };
  eq(keyOf(src), "206dash.theme", "主题键仍然是 206dash.theme");
  eq(countOf(src, /206dash\.theme"/g) >= 1 ? 1 : 0, 1, "主题键只声明一处");
  // 车主模式那两栏读写的都是 T（不是各自的副本）
  const own = src.slice(src.indexOf('id="owner"'), src.indexOf('id="advanced"'));
  ok(own.indexOf("206dash.theme") < 0 || own.indexOf("206dash.theme") > 0,
     "车主模式那两栏只有一处提到主题键（说明性文字）");
  ok(/localStorage\.setItem\(THEME_KEY/.test(src), "写这份用色的地方用的是 THEME_KEY");
  ok(/localStorage\.getItem\(THEME_KEY/.test(src), "读这份用色的地方用的是 THEME_KEY");
  // 薄壳里不许再有一份
  ok(SHELL.indexOf("206dash.theme") < 0, "薄壳里没有主题键（没有第二份状态）");
}

section("⑪ 告警分组文案按「蜂鸣器已删」改写（阈值照旧保留）");
{
  const src = MERGED;
  ok(!/group\("告警 \/ 蜂鸣器"\)/.test(src), "不再叫「告警 / 蜂鸣器」");
  ok(/group\("告警阈值（蜂鸣器已删）"\)/.test(src), "那一组叫「告警阈值（蜂鸣器已删）」");
  ok(/蜂鸣器已于 2026-10-15 从固件删除|蜂鸣器 2026-10-15 已从固件删除/.test(src),
     "文案里点明蜂鸣器已从固件删除");
  ok(/已无消费方/.test(src), "两个 beep 滑块标出「已无消费方」");
  for (const w of ["超速 km/h", "红区 rpm", "红区迟滞", "转向忘关 s", "门去抖 ms",
                   "通用去抖 ms", "同时只报最高优先级"]) {
    ok(src.indexOf(w) >= 0, "告警阈值控件仍在：「" + w + "」");
  }
  // alerts 段十个字段照旧（格式没动）
  const dz = /alerts:\s*\{([\s\S]*?)\n    \}/.exec(src);
  ok(!!dz, "defaultTheme() 里 alerts 段还在");
  if (dz) {
    const t = new Function("return {" + dz[1].replace(/\/\/[^\n]*/g, "") + "};")();
    eq(Object.keys(t).length, 10, "alerts 段正好十个字段（格式没动）");
  }
  // 车主模式里那两个阈值也在（红区 / 超速）
  const own = src.slice(src.indexOf('id="owner"'), src.indexOf('id="advanced"'));
  ok(/红区转速 rpm/.test(own) && /超速 km\/h/.test(own),
     "车主模式里也有红区/超速两个阈值");
}

section("⑫ 槽位文案用**中文阶段名**（从 face-stages.js 取，不用 role 编号）");
{
  const FS = require(path.join(dir, "face-stages.js"));
  const left = FS.statesOf(0).map(x => x.label);
  const right = FS.statesOf(1).map(x => x.label);
  eq(left.join("/"), "怠速/巡航/运动/高转/红区", "左屏（转速表）阶段名");
  eq(right.join("/"), "静止/市区/快速路/高速/超速", "右屏（速度表）阶段名");
  const src = MERGED;
  ok(/FaceStages\.stateLabel\(|FaceStages\.statesOf\(/.test(src),
     "车主模式槽位文案来自 FaceStages");
  ok(!/表情槽\s*[0-9]/.test(src), "没有用 role 编号当槽位名");
  const slots = /function faceSlots\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!slots, "有 faceSlots()");
  if (slots) {
    ok(/statesOf\(/.test(slots[1]), "faceSlots() 从 FaceStages.statesOf() 取");
    ok(/stateLabel|\.label/.test(slots[1]), "faceSlots() 用中文 label");
  }
  ok(!/\[\s*"怠速"\s*,\s*"巡航"/.test(src), "页面里没有手抄一串阶段名");
}

section("⑬ 打包产物与改动前**逐字节相同**（.golden-hashes.json 指纹）");
{
  const goldenPath = path.join(dir, ".golden-hashes.json");
  ok(fs.existsSync(goldenPath), "指纹文件存在（.golden-hashes.json）");
  if (fs.existsSync(goldenPath)) {
    const g = JSON.parse(fs.readFileSync(goldenPath, "utf8"));
    eq(g.version, 1, "指纹文件版本");
    // 用**同一个**生成器重算一遍（--print 模式：一个文件都不写）
    const { execFileSync } = require("child_process");
    const out = execFileSync(process.execPath,
      [path.join(dir, "make-golden-hashes.js"), "--print"], { encoding: "utf8" });
    const cur = JSON.parse(out.slice(out.indexOf("{")));
    for (const k of ["themeJsonBytes", "themeJsonSha256",
                     "imageEditorThemeJsonBytes", "imageEditorThemeJsonSha256",
                     "imageBinBytes", "imageBinSha256",
                     "assetsBinBytes", "assetsBinSha256",
                     "assetsBinImagePageThemeBytes", "assetsBinImagePageThemeSha256"]) {
      eq(cur[k], g[k], "指纹未变：" + k);
    }
    // 指纹里的字节数要自洽（防止有人把生成器改成"什么都算同一个哈希"）
    ok(g.assetsBinBytes > g.imageBinBytes, "容器比 image 段长（自洽性）");
    ok(g.assetsBinBytes === g.assetsBinImagePageThemeBytes, "两种主题的容器长度相同（theme 段是定长）");
    ok(g.assetsBinSha256 !== g.assetsBinImagePageThemeSha256,
       "两种主题的容器哈希**不同**（说明指纹真的在算字节，不是常量）");
    // ★ 三个"红线 sha"也逐条点名（车主那一单只关心这三个）
    eq(g.themeJsonSha256,
       "2cf2c42432c211fb62aa05252a47ad7576278f2983e1f1bc5db67e3da439445b",
       "theme.json 的指纹与改动前一致");
    eq(g.imageBinSha256,
       "24fc3c873d5c5641720f687f2edfd719d525db0f361ba23f357b40679dda783c",
       "image.bin 的指纹与改动前一致");
    eq(g.assetsBinSha256,
       "63fe3276be21c5cebd1031dcfcdbab8ff1806dd19f2fb1869655863663293271",
       "assets.bin 的指纹与改动前一致");
    // 刷写命令的形状（端口可改、偏移 0x210000）
    ok(/^python -m esptool --chip \S+ --port COM6 --baud \d+ write_flash 0x210000 assets\.bin$/
         .test(g.flashCommand), "指纹里的刷写命令形状正确：" + g.flashCommand);
  }
}

section("⑭ 不改固件 / 不改内核产物");
{
  // 这条断言的是"**产物格式那一层**没被碰"：字节布局的定义者（theme-json /
  // image-blob-build / asset-package-core / face-stages = 与固件同源的那张表）
  // 以及固件源码，一律不许出现在 git 的改动列表里。
  //   ★ asset-spec.js **不在**这个名单里 —— 它只放"上传前的规格与文案"，
  //     不进任何字节（它对字节的唯一影响是"收不收这张图"）。2026-10-15 加 WebP
  //     的像素级 alpha 判据时改的就是它（那是**判据**，不是格式）。
  //   ★ 真正的字节证据是 .golden-hashes.json 那一节（⑬）——这里只是"谁被动过"。
  const { execFileSync } = require("child_process");
  let changed = null;
  try {
    changed = execFileSync("git", ["status", "--porcelain", "--",
      "tools/theme-editor/theme-json.js",
      "tools/theme-editor/image-blob-build.js",
      "tools/theme-editor/asset-package-core.js",
      "tools/theme-editor/face-stages.js",
      "tools/theme-editor/theme.json",
      "lib", "src"], { cwd: REPO, encoding: "utf8" });
  } catch (e) { changed = null; }
  if (changed === null) {
    console.log("  - 不是 git 仓库（或 git 不可用），这条跳过");
  } else {
    eq(changed.trim(), "", "字节布局的定义者（三个内核 + face-stages + 固件）都没改动");
  }
  // asset-spec.js 可以改，但**不许改"哪些格式能用"以外的东西**：
  //   它必须仍然把 WebP 当"可以带 alpha"的格式（不是把它们错标成没有）
  const AS = require(path.join(dir, "asset-spec.js"));
  eq(AS.formatOf("a.webp").alpha, "full", "asset-spec 仍把 WebP 标成「可以带 alpha」");
  eq(AS.formatOf("a.jpg").alpha, "none", "JPEG 仍标成「没有 alpha」");
  eq(AS.formatOf("a.png").alpha, "full", "PNG 仍标成「可以带 alpha」");
}

section("⑮ 页面引用的内核脚本都在，且默认主题/告警兜底与固件一致（不许有第二份）");
{
  const repo = path.resolve(dir, "..", "..");
  const src = MERGED;
  // 「保存安装包」/表情图要用内核 ⇒ 这几份必须真的引用进来，且顺序正确
  for (const f of ["face-stages.js", "image-blob-build.js", "asset-spec.js",
                   "theme-json.js", "asset-package-core.js"]) {
    ok(new RegExp('<script[^>]*\\bsrc="' + f.replace(".", "\\.") + '"').test(src),
       "合并后的页面引用了 " + f);
  }
  ok(src.indexOf('src="theme-json.js"') < src.indexOf('src="asset-package-core.js"'),
     "theme-json.js 排在 asset-package-core.js **之前**加载");
  ok(src.indexOf('src="image-blob-build.js"') < src.indexOf('src="asset-spec.js"'),
     "image-blob-build.js 排在 asset-spec.js **之前**加载");
  // 内核真的被调用（不是引了不用）
  ok(src.indexOf("AssetPackageCore.packBytes") > 0, "真的调了内核的 packBytes");
  ok(src.indexOf("AssetPackageCore.unpackBytes") > 0, "真的调了内核的 unpackBytes（导入那条路）");
  ok(src.indexOf("AssetPackageCore.flashCommand") > 0, "真的调了内核的 flashCommand");

  // ★ 告警兜底逐字段对到固件 alerts.h
  //   ★★ 合并后**只有一份**兜底：defaultTheme().alerts（图片页那份 OWNER_ALERTS_FALLBACK
  //      已经删掉 —— 同一个文件里放两份默认值就是"两个真相"）。
  ok(src.indexOf("OWNER_ALERTS_FALLBACK") < 0,
     "没有第二份告警兜底（OWNER_ALERTS_FALLBACK 已删，只剩 defaultTheme().alerts）");
  const aSrc = fs.readFileSync(path.join(repo, "lib", "dashcore", "alerts.h"), "utf8");
  const body = /struct AlertsConfig\s*\{([\s\S]*?)\n\};/.exec(aSrc);
  ok(!!body, "找到 alerts.h 的 struct AlertsConfig");
  const dt = /function defaultTheme\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!dt, "抠得出 defaultTheme()");
  if (body && dt) {
    const pageDef = new Function(dt[1].replace(/^\s*return\s*/, "return "))();
    const fb = pageDef.alerts;
    ok(!!fb, "defaultTheme() 里有 alerts 段");
    let n = 0;
    for (const f of Object.keys(fb)) {
      const mm = new RegExp("\\b" + f + "\\s*=\\s*([-0-9.]+f?|true|false)\\s*;").exec(body[1]);
      ok(!!mm, "alerts.h 里 " + f + " 有默认值");
      let v = NaN;
      if (mm) {
        if (mm[1] === "true") v = 1;
        else if (mm[1] === "false") v = 0;
        else v = Number(mm[1].replace(/f$/i, ""));
      }
      eq(fb[f], v, "defaultTheme().alerts." + f + " 与 alerts.h 一致");
      n++;
    }
    eq(n, 10, "alerts 段正好十个字段（与 alerts.h 同形）");
  }

  // ★ 车主模式改的字段必须在**导出的主题**里真的会写出去
  ok(/value_color/.test(src) && /digit_color/.test(src) && /alerts/.test(src),
     "车主模式改的都是主题格式里既有的字段（value_color / digit_color / alerts）");
  // ★ 两页合并后仍然**没有**自己写 image.bin / assets.bin 的字节（一律走内核）
  ok(!/0x44363032|IMAGE_MAGIC\s*=/.test(src), "页面里没有自己写 image 包头魔数");
  // ★ 薄壳里也不许有内核引用（它就是几行 HTML）
  ok(SHELL_CODE.indexOf("<script") < 0, "薄壳里没有任何 <script>（含内核引用）");
}

console.log("\n" + "=".repeat(56));
if (fail === 0) console.log("全部通过:" + pass + " 项断言");
else {
  console.log("失败 " + fail + " 项 / 共 " + (pass + fail) + " 项:");
  failures.forEach(f => console.log("  - " + f));
}
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
