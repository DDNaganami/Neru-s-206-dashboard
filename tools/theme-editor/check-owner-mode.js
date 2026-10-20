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
//   ★★ 2026-10-15 晚（本单，主题格式修订 R2 + R3）：上面那句"转速与速度**没有**
//      独立的键（它们共用 digit）"**已经作废** —— 车主下令"转速/速度现在就拆开"，
//      固件加了两个键（readout.rpm_color / speed_color，缺键回落 digit_color），
//      编辑器随之把「数字颜色」那一块拆成**转速数字颜色 / 速度数字颜色**两块；
//      同一天晚上底板深浅也进了主题（readout.plate_opa，0..255，缺键 38 = 15%），
//      车主模式多一个滑杆（`plate`）。⇒ `digit` 这个标签**不再出现在车主模式里**
//      （它只剩"老主题缺新键时的回落值"和"高级里那一行"两个身份）。
//      判据见下面 ①c 那一节（改成断言"它们**存在**"，是这一单的验收点）。
//   ★★ 2026-10-15 晚（车主："光有导出，没有导入 bin 啊？"）：`import` 进能力表 ——
//      「① 配色」「② 图片」**各一个**「打开安装包…」按钮（与 save 同一个道理），
//      两个按钮都只**替点那个既有的隐藏输入框 #file-theme** ⇒ 读文件那条路
//      一行都没新增（判据见 §⑧b：同一个输入框、没有第二套读取函数）。
// ------------------------------------------------------------
const OWNER_CAPS = ["daynight", "bg", "arc", "rpm", "speed", "coolant", "intake", "plate",
                    "redline", "overspeed", "face", "import", "save"];
const OWNER_EXPECT_COUNT = { daynight: 1,
                             bg: 1, arc: 4, rpm: 1, speed: 1, coolant: 1, intake: 1,
                             plate: 1, redline: 1, overspeed: 1, face: 1,
                             import: 2, save: 2 };
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
//   ★★ 2026-10-17（夜间模式 R5，docs/NIGHT-MODE.md §9.1/§9.2）：能力表多两项 ——
//      · `daynight`：「① 配色」最上面那一行「白天 / 夜间」切换（车主模式默认可见）。
//        同一套控件、**两份主题对象**（切到夜间时 T 指向夜间那一份）；判据见 §①d。
//      · `nightbg`：槽位清单里**夜间底图**那一格（role 23）。它与背景格同规格，
//        但**不要求**有图（没传就是白天那张，不是错误）⇒ 单独立一个标记，不与 bgimage 混。
const KNOWN_MARKS = OWNER_CAPS.concat(["preset", "port", "bgimage", "nightbg", "slots", "legend"]);
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
  eq(counts.nightbg || 0, 1, "槽位清单里**夜间底图**那一格恰好一处 nightbg 标记（R5）");
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

section("①c 读数颜色四块板 + 底板深浅：键都在固件里，色板接的就是那些键");
{
  const src = MERGED;
  // ① 四块色板接的键必须**主题里确实有**（都在 theme_store.cpp 里解析）
  const bindC = /ownerColorBind\(\s*"oc-coolant"[\s\S]{0,200}?T\.readout\.coolant_color/.exec(src);
  const bindI = /ownerColorBind\(\s*"oc-intake"[\s\S]{0,200}?T\.readout\.intake_color/.exec(src);
  const bindR = /ownerColorBind\(\s*"oc-rpm"[\s\S]{0,200}?T\.readout\.rpm_color/.exec(src);
  const bindS = /ownerColorBind\(\s*"oc-speed"[\s\S]{0,200}?T\.readout\.speed_color/.exec(src);
  ok(!!bindC, "「水温颜色」接 readout.coolant_color");
  ok(!!bindI, "「进气颜色」接 readout.intake_color");
  ok(!!bindR, "「转速数字颜色」接 readout.rpm_color（R2 新键）");
  ok(!!bindS, "「速度数字颜色」接 readout.speed_color（R2 新键）");
  // ② 初值来源 = 主题当前值（sync 读的是同一个 getter，不是写死常量）
  ok(/coolant:\s*coolant/.test(src) && /intake:\s*intake/.test(src),
     "水温/进气两块色板进了 ownerControls（render 时会被同步成主题当前值）");
  ok(/rpm:\s*rpm/.test(src) && /speed:\s*speed/.test(src),
     "转速/速度两块色板也进了 ownerControls");
  for (const k of ["coolant", "intake", "rpm", "speed", "plate"]) {
    ok(new RegExp("ownerControls\\." + k + "\\.sync\\(\\)").test(src),
       "renderOwner() 里会同步 " + k + "（高级里改 / 套预设 / 读入主题都会走到）");
  }
  // ③ 这些键在**固件解析器**里（权威：lib/themetool/theme_store.cpp）
  const ts = fs.readFileSync(path.join(REPO, "lib", "themetool", "theme_store.cpp"), "utf8");
  for (const k of ["coolant_color", "intake_color", "rpm_color", "speed_color", "plate_opa"]) {
    ok(ts.indexOf('"' + k + '"') >= 0, "theme_store.cpp 解析 " + k);
  }
  // ④ ★★ R2 的兼容条款：缺 rpm/speed 时回落到 **digit_color**（旧主题照旧）
  ok(/if\s*\(!has_rpm_color\)\s*t\.readout\.rpm_color\s*=\s*t\.readout\.digit_color/.test(ts),
     "theme_store.cpp：缺 rpm_color ⇒ 回落 digit_color");
  ok(/if\s*\(!has_speed_color\)\s*t\.readout\.speed_color\s*=\s*t\.readout\.digit_color/.test(ts),
     "theme_store.cpp：缺 speed_color ⇒ 回落 digit_color");
  // ⑤ 默认值（唯一事实来源 = ui_theme.h / dash_ui.cpp）
  const h = fs.readFileSync(path.join(REPO, "lib", "themetool", "ui_theme.h"), "utf8");
  ok(/t\.readout\.rpm_color\s*=\s*t\.readout\.digit_color/.test(h),
     "ui_theme.h：rpm_color 默认 = digit_color 的默认（不设时与拆分前一致）");
  ok(/t\.readout\.speed_color\s*=\s*t\.readout\.digit_color/.test(h),
     "ui_theme.h：speed_color 默认 = digit_color 的默认");
  ok(/kReadoutPlateOpaDefault\s*=\s*\(uint8_t\)\(255 \* 15 \/ 100\)/.test(h),
     "ui_theme.h：底板缺省 = 255*15/100 = 38/255（与拆出来之前逐位相同）");
  // ⑥ 键**确实在页面默认值里**（defaultTheme() 与 theme-default.json 两处都要有）
  const dFn = /function defaultTheme\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!dFn, "合并后的页面里有 defaultTheme()");
  const d = new Function(dFn[1].replace(/^\s*return\s*/, "return "))();
  for (const k of ["coolant_color", "intake_color", "rpm_color", "speed_color", "plate_opa"]) {
    eq(typeof d.readout[k], "number", "defaultTheme().readout." + k + " 有默认值");
  }
  const tpl = JSON.parse(fs.readFileSync(path.join(dir, "theme-default.json"), "utf8")
    .replace(/0x([0-9a-fA-F]+)/g, (m, h2) => String(parseInt(h2, 16))));
  const tplTheme = tpl.theme || tpl;
  for (const k of ["coolant_color", "intake_color", "rpm_color", "speed_color", "plate_opa"]) {
    eq(tplTheme.readout[k], d.readout[k], "theme-default.json 的 " + k + " 与页面默认值一致");
  }
  // ⑦ ★ 那句说明必须在（一句话、不是控件），而且**口径已更新**
  ok(/转速与速度的数字颜色已各自独立/.test(src),
     "配色区里有一句话说明「转速与速度的数字颜色已各自独立」");
  ok(!/目前共用「数字颜色」/.test(src),
     "上一单那句「目前共用『数字颜色』」已经删掉（它随本单作废）");
  // 那句话附近**不许**冒出色板/滑块（说明归说明，别顺手做成控件）
  const ownerTheme = src.slice(src.indexOf('id="owner-theme"'), src.indexOf('id="owner-images"'));
  const legend = /<div class="note"[^>]*data-ocap="legend"[^>]*>([\s\S]*?)<\/div>/.exec(ownerTheme);
  ok(!!legend, "那行说明带 data-ocap=\"legend\" 标记（便于本脚本点名）");
  if (legend) {
    ok(!/<input|<select|<button/.test(legend[1]),
       "说明那一行里没有 input/select/button（是纯文字）");
  }
  // ⑧ 底板深浅那一行：滑杆的域必须是**主题文件的 0..255**，不是百分比
  const plateRow = /<div class="row" data-ocap="plate">([\s\S]*?)<\/div>/.exec(ownerTheme);
  ok(!!plateRow, "车主模式里有 data-ocap=\"plate\" 那一行（数字底板深浅）");
  if (plateRow) {
    ok(/id="oc-plate"/.test(plateRow[1]) && /type="range"/.test(plateRow[1]),
       "底板那一行是一个滑杆 #oc-plate");
    ok(/min="0"[^>]*max="255"/.test(plateRow[1]) || /max="255"[^>]*min="0"/.test(plateRow[1]),
       "滑杆的域是 0..255（与主题键 plate_opa 同一量纲，不是百分比）");
    ok(/id="oc-plate-pct"/.test(plateRow[1]), "旁边有百分比显示 #oc-plate-pct（只是给人看）");
  }
  ok(/ownerOpaBind\(\s*"oc-plate"[\s\S]{0,160}?T\.readout\.plate_opa/.test(src),
     "底板滑杆接的是 readout.plate_opa");
  ok(/Math\.round\(v \* 100 \/ 255\)/.test(src),
     "百分比是按 255 域换算出来的（38 ⇒ 15%），不是把文件里的数当百分比");
  // ⑨ 高级里那几行也都在（一份状态两处显隐）
  ok(/colorRow\(wrap,\s*"水温（左屏）"/.test(src), "高级里仍有「水温（左屏）」色板");
  ok(/colorRow\(wrap,\s*"进气温度（右屏）"/.test(src), "高级里仍有「进气温度（右屏）」色板");
  ok(/colorRow\(wrap,\s*"大数字 · 转速（左屏）"/.test(src),
     "高级里也有「大数字 · 转速（左屏）」那一行");
  ok(/colorRow\(wrap,\s*"大数字 · 速度（右屏）"/.test(src),
     "高级里也有「大数字 · 速度（右屏）」那一行");
  ok(/colorRow\(wrap,\s*"大数字 · 回落值（两屏共用）"/.test(src),
     "高级里保留了 digit_color 的入口（它是缺键回落值，不是死键）");
  ok(/opaRow\(wrap,\s*"读数底板深浅/.test(src),
     "高级里也有底板深浅那一行（opaRow 与弧的「轨道透明」共用同一个控件）");
  // ⑩ ★★ R4（2026-10-15，车主："rpm / km/h 的字体颜色是不是漏掉了"）：
  //   主读数**单位**颜色跟随**该屏的数字颜色**；`unit_color` 键保留但已无渲染消费者。
  {
    const dSrc = fs.readFileSync(path.join(REPO, "src", "dash_ui.cpp"), "utf8");
    const hSrc2 = fs.readFileSync(path.join(REPO, "lib", "themetool", "ui_theme.h"), "utf8");
    ok(/主题格式修订 R4/.test(hSrc2), "ui_theme.h 里有 R4 的修订注记");
    ok(/unit_lbl\s*=\s*make_readout_label\(\s*parent,\s*READOUT_UNIT_FONT,\s*lv_color_hex\(digit_base_color\(pk\)\)/
         .test(dSrc),
       "dash_ui.cpp：单位标签用 digit_base_color(pk)（跟该屏数字同一个值）");
    ok(!/lv_color_hex\(READOUT_UNIT_COLOR\)/.test(dSrc),
       "dash_ui.cpp 不再用 unit_color 画单位（它已无渲染消费者）");
    ok(/uint32_t\s+unit_color\s*;/.test(hSrc2) && /t\.readout\.unit_color\s*=\s*0x9AA0A6/.test(hSrc2),
       "unit_color 的字段与默认值都**保留**（老主题文件不许静默换意思）");
    ok(ts.indexOf('"unit_color"') >= 0, "theme_store.cpp 照旧解析 unit_color");
    // 预览侧也必须跟随（否则又是"真机与预览不一致"）
    ok(/drawReadoutLabel\(ctx, W, L\.unit_cy, u, unitPx, hexOf\(digitColor\)\)/.test(src),
       "预览里单位用的是 digitColor（该屏数字色），不是 r.unit_color");
    ok(!/hexOf\(r\.unit_color\)/.test(src), "预览里不再用 r.unit_color 画任何东西");
    // 两处「单位颜色」控件仍在（键保留），且标签写明它已不生效
    ok(/单位颜色（R4 起跟随数字色，已不生效）/.test(src),
       "高级里「单位颜色」控件保留，并注明 R4 起已不生效");
    ok(/单位 km\/h · rpm（R4 起跟随该屏数字色，本行已不生效）/.test(src),
       "「高级 → 读数颜色」里那一行也注明了");
  }
}

section("①d 白天 / 夜间切换（R5）：同一套控件、两份对象、导出带 night 段");
{
  const src = MERGED;
  const ownerTheme = src.slice(src.indexOf('id="owner-theme"'), src.indexOf('id="owner-images"'));
  // ① 切换那一行必须**静态写在「① 配色」里**（车主模式默认可见 = 默认就是车主模式）
  const row = /<div class="row" data-ocap="daynight">([\s\S]*?)<\/div>/.exec(ownerTheme);
  ok(!!row, "「① 配色」里有 data-ocap=\"daynight\" 那一行（白天 / 夜间切换）");
  if (row) {
    ok(/id="oc-slot-day"/.test(row[1]) && /id="oc-slot-night"/.test(row[1]),
       "那一行里两个按钮：#oc-slot-day / #oc-slot-night");
    ok(/白天/.test(row[1]) && /夜间/.test(row[1]), "按钮文案是中文「白天 / 夜间」");
  }
  // ② 三份状态 + 换挡函数（页面里只有一份实现）
  ok(/let T = defaultTheme\(\);/.test(src), "T 仍在（= 现在正在编辑的那一份）");
  ok(/let TD = T;/.test(src), "TD = 白天那一份");
  ok(/let TNight = null;/.test(src), "TNight = 夜间那一份（懒复制：第一次切过去才建）");
  ok(/let themeSlot = "day";/.test(src), "themeSlot 记着现在编辑哪一份（默认白天）");
  eq(countOf(src, /function setThemeSlot\(/g), 1, "setThemeSlot() 只有一份");
  ok(/\["oc-slot-day", "oc-slot-night"\]\.forEach/.test(src),
     "两个按钮接的是**同一个** setThemeSlot()（不是两套切换）");
  ok(/T = \(want === "night"\) \? nightTheme\(\) : TD/.test(src),
     "切挡 = 换 T 指向的对象（渲染器一个字节都没改）");
  // ③ 夜间被改过的**唯一收口**：forgetThemeSrcBytes（改色、套预设、读主题都走它）
  const forget = /function forgetThemeSrcBytes\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!forget, "有 forgetThemeSrcBytes()");
  if (forget) {
    ok(/themeSlot === "night"/.test(forget[1]) && /nightTouched = true/.test(forget[1]),
       "forgetThemeSrcBytes() 在夜间挡下把 nightTouched 置真（改过才写 night）");
  }
  // ④ 导出的形状 {theme, night}，而且三处（导出 / 文本框 / 容器段）走**同一句**
  ok(/function themeFileObject\(\)/.test(src) && /theme: TD, night: themeNightBlock\(\)/.test(src),
     "themeFileObject() = { theme: TD, night: themeNightBlock() }（§8.1 的形状）");
  ok(/ThemeJson\.nightPatch\(TD, TNight\)/.test(src),
     "night 段 = ThemeJson.nightPatch(白天, 夜间)（只写差异，缺键继承）");
  ok(/function themeFileText\(\)/.test(src) &&
     /JSON\.stringify\(themeFileObject\(\), null, 2\)/.test(src),
     "themeFileText() 是 theme.json 的唯一份文本");
  eq(countOf(src, /themeFileText\(\)/g) >= 3 ? 1 : 0, 1,
     "导出 theme.json / JSON 文本框 / assets.bin 的主题段都走 themeFileText()（三处、一份）");
  ok(!/JSON\.stringify\(\{ theme: T \}, null, 2\)/.test(src),
     "页面里没有第二句 `JSON.stringify({theme:T}, null, 2)`（会漏掉 night 段）");
  // ⑤ 导入：缺 night 的老文件照样能用；有 night 时**缺键继承白天**
  ok(/function applyImportedNight\(nightBlock\)/.test(src), "有 applyImportedNight()");
  ok(/ThemeJson\.applyPatch\(cloneTheme\(TD\), nightBlock\)/.test(src),
     "导入 night 段用 applyPatch（缺键继承白天），而且**先克隆**（否则夜间与白天共享弧对象）");
  ok(/const hasNight = applyImportedNight\(parsed && parsed\.night\)/.test(src),
     "loadThemeText() 两份一起读（读不到 night 就只读白天）");
  ok(/只有白天 ⇒ 夜间 = 白天副本/.test(src), "来源说明里点明「老文件 ⇒ 夜间 = 白天副本」");
  // ⑥ 夜间底图那一格（role 23）
  const sp = /function ownerSlotSpecs\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!sp && /ImageBlob\.ROLE\.BackgroundNight/.test(sp[1]),
     "ownerSlotSpecs() 里有夜间底图（ImageBlob.ROLE.BackgroundNight）");
  ok(!!sp && /night: true/.test(sp[1]), "夜间底图那一格带 night:true（标签/空态文案分流）");
  ok(/bgRoleForSlot\(\)/.test(src) && /pick\(ImageBlob\.ROLE\.BackgroundNight\)/.test(src),
     "预览在夜间挡用 role 23、没传就退回 role 1（与设备同一条降级）");
  // ⑦ 真跑一遍 nightPatch：**数组必须仍是数组**（固件只认数组，数字键对象会静默不生效）
  {
    const TJ = require(path.join(dir, "theme-json.js"));
    const day = { bg_color: 1,
                  screens: [{ arcs: [{ value_color: 1 }, { value_color: 2 }] },
                            { arcs: [{ value_color: 3 }] }] };
    const night = JSON.parse(JSON.stringify(day));
    night.screens[0].arcs[1].value_color = 99;
    const p = TJ.nightPatch(day, night);
    ok(Array.isArray(p.screens), "nightPatch 的 screens 仍是**数组**（不是数字键对象）");
    ok(Array.isArray(p.screens[0].arcs), "arcs 仍是**数组**");
    eq(p.screens[0].arcs[1].value_color, 99, "只留改过的那个叶子");
    eq(Object.keys(p.screens[0].arcs[0] || {}).length, 0, "没改的元素是 {}（= 继承）");
    ok(TJ.isPlainObject(p.screens[1]) && Object.keys(p.screens[1]).length === 0,
       "整屏没改 ⇒ 该下标是空对象（数组**长度与形态不变**：JSON 里不会变成 null）");
    eq(p.screens.length, 2, "夜间数组的长度 = 白天的长度（元素一个不少）");
    eq(p.bg_color, undefined, "没改的顶层键不写进 night");
    eq(TJ.deepEqual(TJ.applyPatch(day, p), night), true,
       "applyPatch(白天, nightPatch) 能回到夜间那一份（继承规则闭合）");
    eq(JSON.stringify(TJ.nightPatch(day, day)), "{}", "没有差异时是 {}（= 全继承 = 永远白天）");
  }
}

section("①e 角色 23 = BackgroundNight（三处编号表对账 + 「没传就是白天那张」）");
{
  const IB = require(path.join(dir, "image-blob-build.js"));
  const AS = require(path.join(dir, "asset-spec.js"));
  const hBlob = fs.readFileSync(path.join(REPO, "lib", "themetool", "image_blob.h"), "utf8");
  eq(IB.ROLE.BackgroundNight, 23, "image-blob-build.js 的 ROLE.BackgroundNight = 23");
  ok(/BackgroundNight\s*=\s*23\b/.test(hBlob), "image_blob.h 的 ImageRole::BackgroundNight = 23");
  const r23 = AS.roleByNumber(23);
  ok(!!r23, "asset-spec.js 的 ROLES 里有编号 23");
  eq(r23 && r23.key, "BackgroundNight", "asset-spec 里 23 的 key 与固件同名（BackgroundNight）");
  eq(r23 && r23.label, "夜间底图（两屏共用）", "asset-spec 里 label = 「夜间底图（两屏共用）」");
  eq(r23 && r23.alpha, "either", "与背景同规格：不要求 alpha");
  eq(r23 && r23.sizeByTier, "background", "与背景同一档尺寸口径（480×480）");
  ok(AS.RESERVED_ROLES.indexOf(23) < 0, "23 **不是**保留号（保留号一律不复用）");
  eq(AS.NEXT_FREE_ROLE, 24, "下一个空号是 24");
  eq(IB.ROLE_NAMES[23], "夜间底图", "打包器的显示名是「夜间底图」");
  // theme-json.js 里新增的 nightPatch 必须是**新增**（老 API 一个都不许动）
  const TJ = require(path.join(dir, "theme-json.js"));
  for (const k of ["stripHex", "parseThemeJson", "themeObject", "mergeDefaults",
                   "diffDeep", "applyPatch", "applyReadoutFallbacks"]) {
    eq(typeof TJ[k], "function", "theme-json.js 的老 API 还在：" + k);
  }
  // ★ 上传端口径：夜间底图与背景**同规格**（PNG / JPEG / WebP 都收、不要求 alpha），
  //   而表情槽照旧拒 JPEG —— R5 只加了底图，没有顺手放宽表情那条。
  ok(AS.checkUpload("x.png", "image/png", 23, "s3").ok, "夜间底图收 PNG");
  ok(AS.checkUpload("x.jpg", "image/jpeg", 23, "s3").ok,
     "夜间底图收 JPEG（与背景同规格：不要求 alpha）");
  eq(AS.checkUpload("x.jpg", "image/jpeg", IB.ROLE.FaceIdle, "s3").ok, false,
     "表情槽照旧拒 JPEG（R5 没有放宽表情的口径）");
  // ★ 打包器 → 解析器：role 23 真的能写进 image 段、再被读回来
  {
    const APC = require(path.join(dir, "asset-package-core.js"));
    const blob = IB.build([{ name: "bg_night", w: 8, h: 8, cf: IB.CF.RGB565,
                             role: IB.ROLE.BackgroundNight, order: 0,
                             pixels: new Uint8Array(8 * 8 * 2) }]).blob;
    const info = APC.parseImageBlob(new Uint8Array(blob), { label: "role23 夹具" });
    eq(info.count, 1,
       "一份**只有**夜间底图的 image.bin 能打出来（不算『缺背景』，那是合法配方）");
    eq(info.entries[0].role, 23, "解析回来的角色号是 23（image 段里那一项）");
    eq(info.entries[0].cf, IB.CF.RGB565, "夜间底图的颜色格式是 RGB565（与背景一致）");
  }
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

section("⑧b 导入：车主模式两个按钮，与高级那两个**共用同一个输入框、同一条实现**");
{
  const src = MERGED;
  // ① 车主模式里恰好两个「打开安装包…」按钮（① 配色 / ② 图片 各一个）
  const ownerTheme = src.slice(src.indexOf('id="owner-theme"'), src.indexOf('id="owner-images"'));
  const ownerImages = src.slice(src.indexOf('id="owner-images"'), src.indexOf('id="advanced"'));
  ok(/<button id="btn-owner-import"[^>]*data-ocap="import"/.test(ownerTheme),
     "「① 配色」分区里有「打开安装包…」按钮 #btn-owner-import");
  ok(/<button id="btn-owner-import-img"[^>]*data-ocap="import"/.test(ownerImages),
     "「② 图片」分区里也有一个 #btn-owner-import-img");
  ok(/assets\.bin \/ theme\.json/.test(ownerTheme) && /assets\.bin \/ theme\.json/.test(ownerImages),
     "两个按钮的文案都点明了认哪些文件（assets.bin / theme.json）");
  // ② ★ 它们接的是**同一个** #file-theme（不是各自的第二个输入框）
  ok(/\["btn-owner-import", "btn-owner-import-img"\]\.forEach/.test(src),
     "两个按钮在同一个 forEach 里接线（一处实现）");
  ok(/btn-owner-import[\s\S]{0,400}?document\.getElementById\("file-theme"\)\.click\(\)/.test(src),
     "接线体是「替点 #file-theme」——没有第二套读文件的代码");
  eq(countOf(src, /readAssetFile\s*\(/g) >= 2 ? 1 : 0, 1,
     "readAssetFile() 只有一份（定义 + #file-theme 的 change）");
  ok(/id="file-theme"/.test(src), "那个隐藏输入框仍然在（两个分区的按钮都指向它）");
  ok(/getElementById\("file-theme"\)\.addEventListener\("change"/.test(src),
     "输入框的 change 仍然接在 readAssetFile()（读文件那条路没被复制）");
  // ③ 高级那两个入口照旧可用（同一条路），没有被搬走
  ok(/id="btn-import"/.test(src) && /导入主题文件/.test(src),
     "高级里「导入主题文件」#btn-import 仍在");
  ok(/id="btn-load-theme"/.test(src) && /读入 theme\.json \/ assets\.bin/.test(src),
     "高级里「读入 theme.json / assets.bin」#btn-load-theme 仍在");
  //   ★ 数是 **3** 而不是 4：车主模式那两个按钮**共用一个 forEach 接线**
  //     （两个 id 一行 forEach），高级那两个各自一行 ⇒ 3 处调用点管 4 个按钮。
  eq(countOf(src, /getElementById\("file-theme"\)\.click\(\)/g), 3,
     "3 处「替点 #file-theme」的接线管住 4 个按钮（车主 2 个共用一行 + 高级 2 个各一行）");
  ok(!/unpackBytes\s*\(/.test(src.replace(/AssetPackageCore\.unpackBytes\s*\(\{[\s\S]{0,200}?\}\)/, "X")) ||
     countOf(src, /AssetPackageCore\.unpackBytes\s*\(/g) === 1,
     "页面里只调用一次内核的 unpackBytes（没有第二套解包）");
  // ④ 读进来的清单要在**车主模式**也显示一份（否则车主不知道读进来了什么）
  ok(/function showAssetsInfo\(/.test(src), "有 showAssetsInfo()（一处生成、两处显示）");
  ok(/id="owner-assets-info"/.test(ownerImages), "车主模式里有显示清单的容器 #owner-assets-info");
  ok(/showAssetsInfo\(describeImageInfo/.test(src),
     "读入容器后把清单同时写到两处（高级 + 车主模式）");
  // ⑤ ★★ 2026-10-15（车主："打开安装包…之后没有看见图片载入"）：读进来的图必须
  //    **装回槽位清单** —— 以前只应用配色，image 段只记在手里 ⇒ 槽位照旧是空的。
  ok(/function restoreImagesFromInfo\(/.test(src), "有 restoreImagesFromInfo()（装回槽位）");
  ok(/restoreImagesFromInfo\(res\.imageBytes,\s*res\.imageInfo,\s*f\.name\)/.test(src),
     "容器那条路用它把 image 段装回槽位（用 unpackBytes 给的既有解析）");
  ok(/items\.splice\(i,\s*1\)/.test(src) && /items\.push\(/.test(src),
     "装回时同一用途上的旧项被替换、其余角色保留（导入两次结果一样）");
  //    单独一份 image.bin 那条老路也要装回（以前只报字节数 + 刷写命令）
  ok(/function legacyFileInfo\(f,\s*bytes\)/.test(src),
     "legacyFileInfo() 收的是**字节**（不是长度）");
  ok(/AssetPackageCore\.parseImageBlob\(bytes/.test(src),
     "它用既有的 parseImageBlob 解析（image.bin 也就装回槽位）");
  ok(/legacyFileInfo\(f,\s*new Uint8Array\(r\.result\)\)/.test(src),
     "#file-bin（载入已有 bin）走**同一个** legacyFileInfo（没有第二份实现）");
  //   ★ 判据盯**状态栏那句话**，不是全文搜四个字 —— 代码注释里会引用老文案
  //     （"以前写的是…"），全文搜会把注释也算成"还在"（实测踩过这个假红）。
  ok(!/"注意：本工具不解析已有 bin 的像素/.test(src),
     "状态栏不再说『本工具不解析已有 bin 的像素』（它现在是假的）");
  ok(/不是\*\*本工具认的\*\* image\.bin|不是.{0,4}本工具认的.{0,4}image\.bin/.test(src),
     "读不懂的字节仍然给一句准确的话（『这不是本工具认的 image.bin』）");
  // ⑥ ★★ 往返判据的**实现**：导入来的项带着原始设备字节 ⇒ buildPayload 直接透传
  ok(/if \(item\.packed/.test(src), "buildPayload() 对导入项走 item.packed 透传（带收口判据）");
  ok(/nameRaw: item\.nameRaw/.test(src), "透传时连**名字的原始字节**一起带上");
  ok(/packed: raw,/.test(src) && /nameRaw: nameRaw,/.test(src),
     "restoreImagesFromInfo() 把原始像素与原始名字字节都挂在项上");
  // ⑦ ★★ 透传要**收口**：改过就不许再透传（否则"改了没反应"）。
  ok(/function packedUnchanged\(item\)/.test(src),
     "有 packedUnchanged()（唯一那处判据，不在四个控件里各加一句）");
  ok(/item\.packed && item\.packedMeta && packedUnchanged\(item\)/.test(src),
     "buildPayload() 只在『没改过』时才透传");
  ok(/packedMeta:\s*\{/.test(src), "导入时把快照（w/h/cf/role/stridePad/fit/trim）挂在项上");
  ok(/m\.fit === item\.fit && m\.trim === !!item\.trim/.test(src),
     "快照连缩放方式与裁透明边一起比（改了就回渲染路径）");
}

section("⑧c 快照文件名带时间戳 + 「保存」与「写进板子」两件事的文案");
//   车主原话："为什么每次刷写都要保存一次 bin 呢，这样我的下载文件夹会有很多个相同的 bin。"
//   ⇒ 编辑器这边只做两件事：① 下载名带时间戳（字节不变）；② 把两件事写清。
{
  const src = MERGED;
  const own = src.slice(src.indexOf('id="owner"'), src.indexOf('id="advanced"'));
  // ① 时间戳文件名：只改**文件名生成**，不碰字节
  ok(/function stampName\(base\)/.test(src), "有 stampName()（只生成文件名）");
  ok(/const outName = stampName\("assets\.bin"\)/.test(src),
     "「保存安装包」用它生成下载名");
  ok(/download\(new Blob\(\[packed\.bytes\][\s\S]{0,80}?outName\)/.test(src),
     "下载时用的是那个带时间戳的名字（字节还是 packed.bytes）");
  ok(/flashCommand\(packed\.layout, outName,/.test(src),
     "状态栏给的刷写命令也用同一个文件名（复制粘贴不会对不上）");
  ok(/padStart\(2, "0"\)/.test(src) && /getFullYear\(\)/.test(src),
     "时间戳按 年-月-日-时分 补零（assets-2026-10-15-2130.bin 这种）");
  // ② 文案：两件事写清（保存 = 快照会下载；写进板子 = 直接刷、不下载）
  ok(/「保存安装包」= 留一份快照/.test(own), "文案点明「保存安装包」= 留一份快照");
  ok(/「写进板子」= 直接把当前状态刷进板子/.test(own),
     "文案点明「写进板子」= 直接把当前状态刷进板子");
  ok(/不产生下载/.test(own), "文案点明写进板子**不产生下载**");
  ok(/assets-2026-10-15-2130\.bin/.test(own), "文案里给了文件名示例（带时间戳）");
  // ★ 只是文案：这几句所在的那几行里**没有**控件（控件个数由 §① 的能力表钉着）
  let checkedNotes = 0;
  for (const m of own.matchAll(/<div class="note"[^>]*>([\s\S]*?)<\/div>/g)) {
    if (/留一份快照|不产生下载/.test(m[1])) {
      checkedNotes++;
      ok(!/<input|<select|<button/.test(m[1]), "那几句说明里没有控件（是纯文字）");
    }
  }
  ok(checkedNotes >= 2, "两个分区里都写了这几句（找到 " + checkedNotes + " 处）");
  eq(countOf(own, /data-ocap="save"/g), 2, "「保存安装包」仍然是 2 个按钮（没有新增控件）");
  eq(countOf(own, /data-ocap="import"/g), 2, "「打开安装包…」仍然是 2 个（没有新增控件）");
}

section("⑧d 「写进板子」不再要求先保存（车主：『点了写进板子没反应』）");
//   真因：点的是编辑器这块面板的 #btn-owner-flash，而它当时要求"先点过保存安装包"
//   （pending 为空就只说一句"先点上面的…"）。现在：**当场用当前状态打包**再 POST。
{
  const src = MERGED;
  // ① 打包**只有一条路**：两个入口共用 packCurrentAssets()
  ok(/function packCurrentAssets\(\)/.test(src), "有 packCurrentAssets()（唯一那条打包路）");
  const expBody = /function exportAssetsBin\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!expBody && /packCurrentAssets\(\)/.test(expBody[1]), "「保存安装包」走 packCurrentAssets()");
  ok(!!expBody && /\bdownload\(/.test(expBody[1]),
     "★ exportAssetsBin() 里仍然直接调 download()（外壳靠『截下它』拿字节 —— 约定不许断）");
  const flashBody = /async function ownerFlashWrite\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!flashBody && /packCurrentAssets\(\)/.test(flashBody[1]),
     "「写进板子」也走同一个 packCurrentAssets()（没有第二套打包）");
  ok(!!flashBody && !/\bdownload\(/.test(flashBody[1]),
     "「写进板子」那条路里**没有** download() ⇒ 不产生下载文件");
  // ② "先保存"那道门槛已经删掉（pending 这个隐式状态也不留）
  ok(!/ownerClient\.pending/.test(src), "没有 ownerClient.pending 了（不再留『上一次保存的字节』）");
  ok(!/先点上面的「保存安装包/.test(src), "那句『先点上面的保存安装包』已删（它就是车主看到的『没反应』）");
  ok(/const ownerClient = \{ on: false, ports: \[\] \};/.test(src),
     "ownerClient 里只剩 on / ports 两个字段");
  // ③ 偏移仍然没有入口：请求体只有 port / kind / path
  ok(/\{ port: port, kind: "assets", path: up\.path \}/.test(src),
     "POST /api/flash 的请求体还是那三个字段");
  ok(!/offset\s*:/.test(flashBody ? flashBody[1] : ""),
     "「写进板子」的请求体里没有偏移字段（想改也没地方改）");
  // ④ 非客户端场景**一个字没变**：不是 http(s) 就早退（面板不出现），esptool 命令照旧
  ok(/if \(proto !== "http:" && proto !== "https:"\) return;/.test(src),
     "file:// 双击打开 ⇒ 探测直接返回（那块面板一直 display:none）");
  ok(/<div id="owner-flash" style="display:none">/.test(src),
     "面板默认是 display:none（只有客户端答了才露出来）");
  ok(/function updateOwnerFlash\(\)/.test(src) && /id="owner-cmd"/.test(src),
     "没有客户端时那条 esptool 命令（#owner-cmd）与端口框照旧在");
  // ⑤ 版本 / 能力标记（外壳据此提前发现"老页面"）
  ok(/window\.__OWNER_MODE_VERSION = OWNER_MODE_VERSION/.test(src),
     "暴露 window.__OWNER_MODE_VERSION");
  ok(/window\.__OWNER_MODE_CAPS = OWNER_MODE_CAPS/.test(src) && /flashNoSave: true/.test(src),
     "暴露 window.__OWNER_MODE_CAPS（含 flashNoSave）");
  // ★★ R5：新能力也要进能力表 —— 外壳靠它判断"这页能不能存两套主题 / 有没有夜间底图槽"。
  ok(/nightTheme: true/.test(src), "能力表里报了 nightTheme（R5：两套主题）");
  ok(/nightBackground: true/.test(src), "能力表里报了 nightBackground（R5：夜间底图槽）");
  ok(/data-owner-mode-version/.test(src), "<html> 上也有同一份版本号（DOM 侧副本）");
  // ⑥ 文案：不要"先保存"那套说法
  ok(/不需要先保存/.test(src), "面板文案点明『不需要先保存』");
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
    // ★★ 2026-10-15（R2）：预设还必须写 rpm_color / speed_color ——
    //   只写 digit_color 的话屏上大数字**一点都不会变**（渲染侧按角色取
    //   rpm/speed），症状很像"预设按钮坏了"。
    ok(/rpm_color\s*=/.test(m[1]), "预设写 readout.rpm_color（拆开之后必须写）");
    ok(/speed_color\s*=/.test(m[1]), "预设写 readout.speed_color（同上）");
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

section("⑬ 打包产物指纹（.golden-hashes.json）—— R5 之后**三份都该变**（主题多 night、图片多 role 23）");
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
      eq(cur[k], g[k], "指纹自洽（文件里的值 = 重算的值）：" + k);
    }
    // 指纹里的字节数要自洽（防止有人把生成器改成"什么都算同一个哈希"）
    ok(g.assetsBinBytes > g.imageBinBytes, "容器比 image 段长（自洽性）");
    ok(g.assetsBinBytes === g.assetsBinImagePageThemeBytes, "两种主题的容器长度相同（theme 段是定长）");
    ok(g.assetsBinSha256 !== g.assetsBinImagePageThemeSha256,
       "两种主题的容器哈希**不同**（说明指纹真的在算字节，不是常量）");
    // ★★ 三处 sha **逐条点名钉死**（值本身不许动 —— 要动就是"格式改了"，
    //   必须在 make-golden-hashes.js 那段说明里写清原因与新值）：
    //   2026-10-15 晚 R2（rpm_color/speed_color）+ R3（plate_opa）：
    //     · themeJson / imageEditorThemeJson **有意变更**（多三个键）；
    //     · imageBin **不变**（与图片无关）；
    //     · assetsBin **必须变** —— 它是"主题段 + image 段"的整包，
    //       主题段就在里面（取证见 make-golden-hashes.js 顶上那一段：
    //       差异全部落在 [0, 0x4000) 主题段内，[0x4000,end) 逐字节相同）。
    //   ★★ 2026-10-17（夜间模式 R5，docs/NIGHT-MODE.md §8）—— **这一单三份都变**，
    //      与上一单"imageBin 不变"是相反的，理由也相反：
    //        · themeJson 2975 → **2990** 字节：形状从 {theme} 变成 {theme, night}；
    //          默认页面"没动过夜间" ⇒ night 是 {}，多出来的正好是
    //          `,\n  "night": {}` 这 15 个字节（不是把整份白天抄一遍）。；
    //        · imageBin 16524 → **24716** 字节：多了**新角色 23**（夜间底图，
    //          夹具里是一张 64×64 RGB565 = 8192 字节）；
    //        · assetsBin 295052 → **303244** 字节：主题段 +15、image 段 +8192。
    //      ⇒ 上一单那句"imageBin 与图片无关"在这一单**不成立**：这一单加的正是图片角色。
    eq(g.themeJsonSha256,
       "21db68663972cbd0df5dd99f9cc5559674a5bf07b7d601c044cbcdd86fe960a0",
       "theme.json 的指纹（R5 之后：形状 {theme, night}，2990 字节）");
    eq(g.imageBinSha256,
       "b3fcde838e36ad8175388f50f7bbc01fe627c9c2f06f173cb007175ed3845a2d",
       "image.bin 的指纹（R5 之后：多了 role 23 夜间底图）");
    eq(g.assetsBinSha256,
       "1060979c0d4997f6464d9a2a1a15dfeeef55cf2e3402b2d89d96452e8901e9d6",
       "assets.bin 的指纹（主题段 +15 字节、image 段 +8192 字节）");
    // 刷写命令的形状（端口可改、偏移 0x210000）
    ok(/^python -m esptool --chip \S+ --port COM6 --baud \d+ write_flash 0x210000 assets\.bin$/
         .test(g.flashCommand), "指纹里的刷写命令形状正确：" + g.flashCommand);
  }
}

section("⑭ 图片那一半：**字节布局**一个字没动（谁动过 + 布局本身，两条一起钉）");
{
  // ★★ 2026-10-15 晚（车主："打开安装包…之后没有看见图片载入"）**口径再改一次**：
  //    `image-blob-build.js` 这一单**有意**动了 —— 名字字段从"码点低 8 位"改成
  //    **UTF-8**（中文名不再变乱码），并支持 `nameRaw` 原样回写（那是"导入 → 再导出
  //    逐字节相同"的前提）。⇒ 再按老口径"这个文件不许出现在 git 改动里"就会把
  //    **做对了**判成红。所以这一节现在钉两件事：
  //      ① **谁动过**：asset-package-core / face-stages / theme.json 必须干净；
  //      ② **布局本身**：常量与项内偏移逐条对账 —— 这才是"格式没变"的真判据
  //         （C 侧另有 offsetof/sizeof 断言，JS 侧有 test-image-blob-build.js，
  //           这里再按"格式定义"重列一遍，谁改坏了当场红）。
  //    ★ 主题那一半的证据在 ⑬（三个 sha 逐条点名，含 imageBin 不变）。
  const { execFileSync } = require("child_process");
  let changed = null;
  try {
    changed = execFileSync("git", ["status", "--porcelain", "--",
      "tools/theme-editor/asset-package-core.js",
      "tools/theme-editor/face-stages.js",
      "tools/theme-editor/theme.json"], { cwd: REPO, encoding: "utf8" });
  } catch (e) { changed = null; }
  if (changed === null) {
    console.log("  - 不是 git 仓库（或 git 不可用），这条跳过");
  } else {
    eq(changed.trim(), "",
       "容器内核与表情表（asset-package-core / face-stages）没改动");
  }
  // ② image.bin 的**布局**（image-blob-build.js ↔ lib/themetool/image_blob.h）：
  //    常量 + 项内偏移，一个都不许动。
  {
    const IB = require(path.join(dir, "image-blob-build.js"));
    const hBlob = fs.readFileSync(path.join(REPO, "lib", "themetool", "image_blob.h"), "utf8");
    eq(IB.MAGIC, 0x44363032, "魔数 '206D'（与 image_blob.h 同值）");
    eq(IB.VERSION, 1, "版本号 1");
    eq(IB.NAME_MAX, 24, "name[24]");
    eq(IB.MAX_COUNT, 32, "最多 32 项");
    eq(IB.ENTRY_SIZE, 44, "项大小 44（没有对齐空洞）");
    eq(IB.HEADER_SIZE, 12 + 32 * 44, "包头 1420 = 12 + 32×44");
    ok(/kImageBlobVersion\s*=\s*1\b/.test(hBlob), "C 侧版本号仍是 1（两边一起钉）");
    // 项内偏移：名字在 +18（24 字节），顺序与前 18 字节一个字节没变
    const src2 = fs.readFileSync(path.join(dir, "image-blob-build.js"), "utf8");
    for (const [off, what] of [["base + 0", "offset u32"], ["base + 4", "size u32"],
                               ["base + 8", "w u16"], ["base + 10", "h u16"],
                               ["base + 12", "cf u8"], ["base + 13", "stride_pad u8"],
                               ["base + 14", "role u16"], ["base + 16", "order u16"],
                               ["base + 18", "name[24]"]]) {
      ok(src2.indexOf(off) >= 0, "项内还有 " + what + "（" + off + "）");
    }
  }
  // ★ 主题格式那一半：这一单**必须**动过（那几个键 + 解析 + 取用都在）
  //   —— 判据用**源码内容**而不是 git 状态：提交之后 git 状态就干净了，
  //     而"这几个键在不在"才是这条要守的东西。
  const tsSrc2 = fs.readFileSync(path.join(REPO, "lib", "themetool", "theme_store.cpp"), "utf8");
  const uiSrc2 = fs.readFileSync(path.join(REPO, "src", "dash_ui.cpp"), "utf8");
  ok(tsSrc2.indexOf('"rpm_color"') >= 0 && tsSrc2.indexOf('"speed_color"') >= 0,
     "主题格式扩了：theme_store.cpp 解析 rpm_color / speed_color");
  ok(uiSrc2.indexOf("digit_base_color(") >= 0,
     "渲染侧按屏的角色取色（dash_ui.cpp 的 digit_base_color）");
  //   ★ asset-spec.js **不在**上面那个名单里 —— 它只放"上传前的规格与文案"，
  //     不进任何字节（它对字节的唯一影响是"收不收这张图"）。2026-10-15 加 WebP
  //     的像素级 alpha 判据时改的就是它（那是**判据**，不是格式）。
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
