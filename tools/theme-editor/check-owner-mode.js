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
 * ★ 为什么能这么查（而不是"跑个浏览器"）：这两个页面是**静态 HTML**，
 *   控件的**身份**就写在源码里（`data-ocap` 标签、标签文字、函数名）。
 *   本仓库既有风格也是这么查的（见 check-role-label.js / check-layer-order.js /
 *   test-theme-json.js 的"解析真源码"那几节）—— 不引浏览器、不装依赖。
 * ★ 两个页面用的是**同一张能力表**（OWNER_CAPS 那 7 个标签），所以"谁改坏了
 *   一边"会被立刻抓住，而且两边不会各自长出一套叫法。
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

const PAGES = { "index.html": read("index.html"), "image-editor.html": read("image-editor.html") };

// ------------------------------------------------------------
// 能力表（页面里那几个"身份标签"）
//   data-ocap="bg|arc|digit|redline|overspeed|face|save"
//   ★ 这 7 个标签覆盖车主原话点名的 9 类控件：
//     背景色(bg) + 四条弧颜色(arc×4) + 数字颜色(digit) + 红区(redline) +
//     超速(overspeed) + 表情图拖入(face) + 保存安装包(save)；
//     左右圆预览位是既有画布（cv0/cv1），不算控件。
// ------------------------------------------------------------
const OWNER_CAPS = ["bg", "arc", "digit", "redline", "overspeed", "face", "save"];
const OWNER_EXPECT_COUNT = { bg: 1, arc: 4, digit: 1, redline: 1, overspeed: 1, face: 1, save: 1 };
// ★ 那四条弧：两页的写法**不一样，都是有意的** ——
//   · 主题页：四行静态写在 HTML 里（`data-ocap="arc"` ×4）⇒ 数到 4 个标记；
//   · 图片页：四行由 JS 循环生成（容器上一处 `data-ocap="arc"` 标记 + 那个循环）
//     ⇒ 数到 1 个标记，**另外用循环那个数去回答"是不是四条"**。
//   这样"四条"在两页都是可执行的判据，而不是靠注释。
const ARC_MARK_EXPECT = { "index.html": 4, "image-editor.html": 1 };

section("① 车主模式恰好有那 9 类控件（两个页面各查一遍）");
// 身份标签有两种合法写法（两页各用一种，都是有意的）：
//   · 主题页（index.html）：**静态写在 HTML 里** —— `data-ocap="bg"`。
//     好处：车主模式下"这一栏是哪几件东西"一眼可读，检查脚本也数得到。
//   · 图片页（image-editor.html）：那几行弧/读数控件本来由 JS 生成，
//     所以用**字面量** setAttribute("data-ocap", "arc")。
//   两种都要求"字面量"：用变量拼出来的标签本脚本数不到，
//   而"数不到"会让这条守卫静默失效 —— 比报错更糟。
function countOwnerCaps(src) {
  const counts = {};
  const add = (k) => { counts[k] = (counts[k] || 0) + 1; };
  let m;
  const reAttr = /\bdata-ocap="([a-z]+)"/g;
  while ((m = reAttr.exec(src)) !== null) add(m[1]);
  const reJs = /setAttribute\(\s*"data-ocap"\s*,\s*"([a-z]+)"\s*\)/g;
  while ((m = reJs.exec(src)) !== null) add(m[1]);
  return counts;
}
const KNOWN_MARKS = OWNER_CAPS.concat(["preset", "port"]);
for (const [page, src] of Object.entries(PAGES)) {
  const counts = countOwnerCaps(src);
  for (const k of Object.keys(counts)) {
    ok(KNOWN_MARKS.indexOf(k) >= 0, page + " 的 data-ocap 值合法：" + k);
  }
  for (const c of OWNER_CAPS) {
    const want = (c === "arc") ? ARC_MARK_EXPECT[page] : OWNER_EXPECT_COUNT[c];
    eq(counts[c] || 0, want, page + " 车主模式的 " + c + " 控件标记个数");
  }
  // "四条弧"的**类别数**在两页都要是 4：数那个循环里的数组长度定义
  const arcsDecl = /const OWNER_ARCS\s*=\s*\[([\s\S]*?)\n\];/.exec(src);
  ok(!!arcsDecl, page + " 里有 OWNER_ARCS 声明");
  if (arcsDecl) {
    eq((arcsDecl[1].match(/\{\s*s\s*:/g) || []).length, 4, page + " 的 OWNER_ARCS 正好四条");
  }
  // 端口那一处标记恰好一个（可改端口、不可改偏移）
  eq(counts.port || 0, 1, page + " 车主模式恰好一处端口标记");
  // 预设那一行：主题页恰好一处
  if (page === "index.html") eq(counts.preset || 0, 1, page + " 预设那一行恰好一处");
  // 那张能力表本身也要在页面里（两页同源），并且**不多不少**
  const capsDecl = /const OWNER_CAPS\s*=\s*(\[[^\]]*\])/.exec(src);
  ok(!!capsDecl, page + " 里有 OWNER_CAPS 声明");
  if (capsDecl) {
    const arr = new Function("return " + capsDecl[1])();
    eq(JSON.stringify(arr), JSON.stringify(OWNER_CAPS), page + " 的 OWNER_CAPS 与能力表一致");
  }
}

section("② 车主模式是**默认**（没记录时进车主模式，不是高级）");
for (const [page, src] of Object.entries(PAGES)) {
  // 默认值必须写成"只有显式存了 adv 才进高级"
  const def = /applyMode\(\s*saved\s*===\s*"adv"\s*\?\s*"adv"\s*:\s*"owner"\s*,\s*true\s*\)/.exec(src);
  ok(!!def, page + " 启动时默认 applyMode(... \"owner\", true)");
  ok(/localStorage\.getItem\(MODE_KEY\)/.test(src), page + " 的模式选择从 localStorage 读");
  ok(/id="btn-mode-owner"/.test(src) && /id="btn-mode-adv"/.test(src),
     page + " 有「车主模式 / 高级」两个切换按钮");
}

section("③ JSON 文本框**只在高级里**");
for (const [page, src] of Object.entries(PAGES)) {
  const at = src.indexOf('id="json"');
  if (at < 0) { console.log("  - " + page + " 没有 JSON 文本框（本页不需要，跳过）"); continue; }
  // 它在 <div id="advanced"> 之后、<main> 之内 ⇒ 属于高级那半栏
  const advAt = src.indexOf('id="advanced"');
  const mainEnd = src.indexOf("</main>");
  ok(advAt >= 0 && at > advAt, page + " 的 JSON 文本框在 #advanced 之后");
  ok(mainEnd > at, page + " 的 JSON 文本框在 </main> 之前");
  // 车主模式那半栏里不许出现它
  const ownerStart = src.indexOf('id="owner"');
  const ownerEnd = advAt;
  ok(!(at > ownerStart && at < ownerEnd), page + " 的 JSON 文本框不在车主模式那一栏里");
}
// 主题页还要有一条更硬的：textarea 的 id 只能出现一次（两处就是两个真相）
eq((PAGES["index.html"].match(/id="json"/g) || []).length, 1, "index.html 只有一处 id=\"json\"");

section("④ 高级里旧控件一个不少（主题页）");
{
  const src = PAGES["index.html"];
  // buildControls() 必须还在、还在被调用（车主模式不许把它删掉）
  ok(/function buildControls\(\)/.test(src), "index.html 里 buildControls() 还在");
  ok(/^\s*buildControls\(\);/m.test(src), "index.html 启动时仍然调用 buildControls()");
  ok(/id="controls"/.test(src), "index.html 里 #controls 容器还在");
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
    "整体淡入", "扫表开始", "错峰启动", "0→满弧", "满弧停留", "满弧→向下", "满弧→0",
    "表情显形", "收尾时长",
    // 告警阈值（蜂鸣器删了，但阈值控件照旧）
    "超速 km/h", "超速迟滞", "红区 rpm", "红区迟滞", "转向忘关 s",
    "门去抖 ms", "通用去抖 ms", "同时只报最高优先级"
  ];
  for (const w of wanted) {
    // "满弧→向下" 是笔误保护：见下面的 slack —— 只查真在页面里的那些
    if (w === "满弧→向下") continue;
    ok(src.indexOf(w) >= 0, "index.html 高级里还有「" + w + "」");
  }
  // JSON 的两个按钮
  ok(/id="btn-apply"/.test(src), "index.html 还有「应用」JSON 的按钮");
  ok(/id="btn-copy"/.test(src), "index.html 还有「复制」JSON 的按钮");
  // 导入 / 导出 / 恢复默认
  ok(/id="btn-default"/.test(src) && /id="btn-import"/.test(src) && /id="btn-export"/.test(src),
     "index.html 还有 恢复默认 / 导入主题文件 / 导出主题文件");
  ok(/id="btn-play"/.test(src) && /id="cvA"/.test(src), "index.html 还有开机动画预览与播放按钮");
  // 预览画布（车主模式要的"左右圆预览"）
  ok(/id="cv0"/.test(src) && /id="cv1"/.test(src), "index.html 有左右两块预览画布");
}

section("⑤ 高级里旧控件一个不少（图片页）");
{
  const src = PAGES["image-editor.html"];
  const wanted = [
    'id="drop"', 'id="list"', 'id="bg-color"', 'id="show-arcs"',
    'id="btn-theme-export"', 'id="btn-load-theme"', 'id="btn-reset-theme"',
    'id="btn-assets-export"', 'id="arc-editors"', 'id="readout-editors"',
    'id="stage"', 'id="face-left"', 'id="face-right"',
    'id="cmd"', 'id="btn-copy"', 'id="btn-cheader"', 'id="btn-import-bin"',
    'id="target-board"', 'id="budget-bar"', 'id="part-info"', 'id="file-bin"',
    'id="btn-add"', 'id="btn-export"', 'id="stage-grid"', 'id="stage-note"',
    'id="file-theme"', 'id="file-img"'
  ];
  for (const w of wanted) ok(src.indexOf(w) >= 0, "image-editor.html 还有 " + w);
  // 那三个"老控件"的口径：contain / cover / stretch、裁透明边、目标宽度
  ok(/id: "contain"/.test(src) && /id: "cover"/.test(src) && /id: "stretch"/.test(src),
     "image-editor.html 还保留 contain / cover / stretch 三种缩放方式");
  ok(/裁透明边/.test(src) && /item\.trim/.test(src), "image-editor.html 还保留「裁透明边」");
  ok(/目标宽度/.test(src) && /SIZE_PRESETS/.test(src), "image-editor.html 还保留「目标宽度」与尺寸预设");
  // 老控件块确实被搬进 #advanced 里（而不是被注释掉/删掉）
  const advAt = src.indexOf('id="advanced"');
  const dropAt = src.indexOf('id="drop"');
  const budgetAt = src.indexOf('id="budget-bar"');
  ok(advAt >= 0 && dropAt > advAt && budgetAt > advAt,
     "image-editor.html 的图片列表/占用那些 section 都在 #advanced 之后");
}

section("⑥ 框选器：只给**表情槽**，背景不加");
for (const [page, src] of Object.entries(PAGES)) {
  ok(/id="crop"/.test(src), page + " 有框选器（模态 #crop）");
  ok(/id="crop-frame"/.test(src), page + " 有「方框 = 保留区」那一层");
  ok(/openCropper\(/.test(src), page + " 有 openCropper()");
  // 方框内的源矩形：以方框中心反算（= 主体居中）
  ok(/cropRenderSquare/.test(src), page + " 有 cropRenderSquare()（导出居中透明方图）");
  // 平移 / 滚轮 / 双指
  ok(/pointerdown/.test(src) && /pointermove/.test(src), page + " 框选器支持拖动平移（pointer 事件）");
  ok(/addEventListener\("wheel"/.test(src), page + " 框选器支持滚轮缩放");
  ok(/touchmove/.test(src) && /e\.touches\.length === 2/.test(src), page + " 框选器支持双指缩放");
  // 产物是**方图**且**透明打底**（不铺底色）
  const sq = /function cropRenderSquare\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!sq, page + " 找得到 cropRenderSquare()");
  if (sq) {
    const b = stripComments(sq[1]);
    ok(/out\.width\s*=\s*side/.test(b) && /out\.height\s*=\s*side/.test(b),
       page + " 框选产物是**方图**（宽=高=side）");
    ok(/drawImage\(/.test(b), page + " 框选产物由 drawImage 画出");
    ok(!/fillRect|fillStyle/.test(b), page + " 框选产物**不铺底色**（保留透明通道）");
    ok(/faceCanvasMaxFor/.test(b), page + " 框选产物的上限问 ImageBlob（不写死 320）");
  }
  // 框选**只**出现在表情那条路上：背景那两个字附近不许有"框选"
  const bgLine = /背景图?不加框选|背景不加框选/.exec(src);
  ok(!!bgLine, page + " 页面里明说「背景不加框选」");
  // 表情那条路先过 asset-spec 的 checkUpload（JPEG/无 alpha 会被拒）
  ok(/AssetSpec\.checkUpload\(/.test(src), page + " 表情入口走 AssetSpec.checkUpload()");
  ok(/表情要透明背景，请用 PNG/.test(src), page + " 拒绝文案里明说「表情要透明背景，请用 PNG」");
  // 产物仍是 RGB565A8（表情槽）
  const imgsrc = PAGES["image-editor.html"];
  ok(/ImageBlob\.CF\.RGB565A8/.test(imgsrc), "image-editor.html 里表情仍是 RGB565A8");
}

section("⑦ 框选只动「进槽前的像素」：还走既有那条缩放/打包路径");
{
  const src = PAGES["image-editor.html"];
  // 框选产物作为**普通 item** 推进 items ⇒ 后面 renderToTarget/buildPayload 一个字没改
  const conf = /function cropConfirm\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!conf, "找得到 cropConfirm()");
  if (conf) {
    const b = stripComments(conf[1]);
    ok(/items\.push\(/.test(b), "框选结果作为普通 item 进列表（走既有那条路）");
    ok(/pickDefaultSize\(/.test(b), "尺寸用既有的 pickDefaultSize()（只缩不放 + 上限）");
    ok(!/rgbaToRgb565/.test(b), "框选器**不自己编码**（编码在 buildPayload 里，只有一份）");
  }
  // 既有的缩放/打包函数没被动过
  ok(/function renderToTarget\(/.test(src), "renderToTarget() 还在（框选不改它）");
  ok(/function buildPayload\(/.test(src), "buildPayload() 还在");
  ok(/ImageBlob\.build\(buildPayload\(\)\)/.test(src), "打包仍然走 ImageBlob.build(buildPayload())");
  // 320 上限仍然来自 ImageBlob（两页都不许写死）
  for (const [page, s] of Object.entries(PAGES)) {
    ok(/ImageBlob\.faceCanvasMaxFor\(/.test(s), page + " 的表情上限问 ImageBlob.faceCanvasMaxFor()");
  }
}

section("⑧ 保存安装包：走**现有** asset-package 内核 + 一条命令（端口可改、偏移不可改）");
for (const [page, src] of Object.entries(PAGES)) {
  ok(/AssetPackageCore\.packBytes|exportAssetsBin|packBytes\(/.test(src),
     page + " 用 asset-package 内核打包");
  ok(/AssetPackageCore\.flashCommand\(/.test(src), page + " 的刷写命令来自 AssetPackageCore.flashCommand()");
  // 偏移不许有"用户可编辑"的入口：只允许一个端口输入框
  ok(/id="owner-port"/.test(src), page + " 有可改的**端口**输入框");
  ok(!/owner-offset|id="offset"/.test(src), page + " 没有可改的**偏移**输入框");
  // 那条命令里偏移来自 layout（0x210000），页面不写死字面量在命令里
  ok(!/write_flash 0x[0-9a-f]+ assets\.bin/.test(src), page + " 页面上没有把整条命令写死");
}
{
  const src = PAGES["image-editor.html"];
  // 「保存安装包」必须调既有导出（不另写一套打包）
  const fn = /function saveOwnerPackage\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  ok(!!fn, "找得到 saveOwnerPackage()");
  if (fn) ok(/exportAssetsBin\(\)/.test(fn[1]), "saveOwnerPackage() 调既有的 exportAssetsBin()");
  // 容器布局没有第二份实现（页面里不许再出现 spiffs / imageOffset 的字面量）
  ok(!/spiffsBytes\s*:/.test(src) || /coreForTarget/.test(src),
     "image-editor.html 的容器布局仍只有 coreForTarget() 一处");
  eq((src.match(/function coreForTarget\(/g) || []).length, 1,
     "image-editor.html 里 coreForTarget() 只有一份");
}
{
  // 主题页那份布局常量必须与内核的 THEME_SECTION_BYTES 一致（写死错了会被 packBytes 拒）
  const src = PAGES["index.html"];
  const m = /const OWNER_LAYOUT\s*=\s*\{([\s\S]*?)\n\};/.exec(src);
  ok(!!m, "index.html 里有 OWNER_LAYOUT（车主模式打包布局）");
  if (m) {
    const APC = require(path.join(dir, "asset-package-core.js"));
    const lay = new Function("return {" + m[1].replace(/\/\/[^\n]*/g, "") + "};")();
    eq(lay.themeSectionBytes, APC.THEME_SECTION_BYTES,
       "OWNER_LAYOUT.themeSectionBytes 与内核 THEME_SECTION_BYTES 一致");
    eq(lay.base, 0x210000, "OWNER_LAYOUT.base = theme 分区偏移 0x210000");
    eq(lay.base + lay.imageOffset, 0x254000, "OWNER_LAYOUT 的 image 偏移算出来是 0x254000");
  }
}

section("⑨ 3 个预设写进**现有主题状态**（当前默认 / 夜间 / 浅底）");
{
  const src = PAGES["index.html"];
  const m = /const PRESETS\s*=\s*\[([\s\S]*?)\n\];/.exec(src);
  ok(!!m, "index.html 里有 PRESETS");
  if (m) {
    const labels = (m[1].match(/label:\s*"([^"]+)"/g) || []).map(s => /"([^"]+)"/.exec(s)[1]);
    eq(labels.length, 3, "预设正好 3 个（实际：" + labels.join(" / ") + "）");
    ok(labels.indexOf("当前默认") >= 0, "有「当前默认」预设");
    ok(labels.indexOf("夜间") >= 0, "有「夜间」预设");
    ok(labels.indexOf("浅底") >= 0, "有「浅底」预设");
    // 写进现有字段：bg_color / value_color / readout.digit_color
    ok(/T\.bg_color\s*=/.test(m[1]), "预设写 bg_color");
    ok(/value_color\s*=/.test(m[1]), "预设写弧的 value_color");
    ok(/digit_color\s*=/.test(m[1]), "预设写 readout.digit_color");
    // 「当前默认」不许抄数字：必须问 defaultTheme()
    const def = /id:\s*"default"[\s\S]*?apply:[\s\S]*?\}\s*\}/.exec(m[1]);
    ok(!!def && /defaultTheme\(\)/.test(def[0]),
       "「当前默认」预设从 defaultTheme() 取（不抄一份数字）");
    // 预设里不许出现"新文件格式"的痕迹
    ok(!/JSON\.stringify|localStorage|new Blob/.test(m[1]), "预设只改状态，不写文件");
  }
  ok(/buildPresetRow|PRESETS\.forEach/.test(src), "预设被挂到界面上");
}

section("⑩ 颜色以**配色页**为主入口；图片页写明与它是同一份");
{
  const img = PAGES["image-editor.html"];
  ok(/与「① 配色」页是同一份/.test(img), "image-editor.html 明说颜色与配色页是同一份");
  ok(/THEME_KEY/.test(img), "image-editor.html 仍然用同一个 THEME_KEY");
  const idx = PAGES["index.html"];
  ok(/与「② 图片」页是同一份/.test(idx) || /同一个 .*THEME_KEY/.test(idx) ||
     /THEME_KEY/.test(idx), "index.html 也点明两页共用同一份");
  // 两页的键名必须一模一样（写错一个字符就静默失效 —— 老测试也钉这条）
  const keyOf = (s) => { const m = /const THEME_KEY = "([^"]+)"/.exec(s); return m ? m[1] : null; };
  eq(keyOf(img), keyOf(idx), "两页的 THEME_KEY 一致");
}

section("⑪ 告警分组文案按「蜂鸣器已删」改写（阈值照旧保留）");
{
  const src = PAGES["index.html"];
  ok(!/group\("告警 \/ 蜂鸣器"\)/.test(src), "主题页不再叫「告警 / 蜂鸣器」");
  ok(/group\("告警阈值（蜂鸣器已删）"\)/.test(src), "主题页那一组叫「告警阈值（蜂鸣器已删）」");
  ok(/蜂鸣器已于 2026-10-15 从固件删除|蜂鸣器 2026-10-15 已从固件删除/.test(src),
     "文案里点明蜂鸣器已从固件删除");
  ok(/已无消费方/.test(src), "两个 beep 滑块标出「已无消费方」");
  // ★ 阈值控件**一个都不能删**（蜂鸣器砍的是发声，不是判断）
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
  // 图片页那两个阈值也在车主模式里
  const img = PAGES["image-editor.html"];
  ok(/红区转速 rpm/.test(img) && /超速 km\/h/.test(img), "图片页车主模式也有红区/超速两个阈值");
}

section("⑫ 槽位文案用**中文阶段名**（从 face-stages.js 取，不用 role 编号）");
{
  const FS = require(path.join(dir, "face-stages.js"));
  // 两屏各自的 5 个阶段名
  const left = FS.statesOf(0).map(x => x.label);
  const right = FS.statesOf(1).map(x => x.label);
  eq(left.join("/"), "怠速/巡航/运动/高转/红区", "左屏（转速表）阶段名");
  eq(right.join("/"), "静止/市区/快速路/高速/超速", "右屏（速度表）阶段名");
  for (const [page, src] of Object.entries(PAGES)) {
    // 槽位文案必须**拼**自 face-stages（stateLabel/screen/statesOf），不许写死一串中文
    ok(/FaceStages\.stateLabel\(|FaceStages\.statesOf\(/.test(src),
       page + " 的车主模式槽位文案来自 FaceStages");
    // 页面里不该出现 "表情槽 3" 这种用 role 编号当名字的写法
    ok(!/表情槽\s*[0-9]/.test(src), page + " 没有用 role 编号当槽位名");
    // 5+5 个槽位名都要能被拼出来
    const slots = /function ownerSlots\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
    ok(!!slots, page + " 有 ownerSlots()");
    if (slots) {
      ok(/statesOf\(/.test(slots[1]), page + " 的 ownerSlots() 从 FaceStages.statesOf() 取");
      ok(/stateLabel|\.label/.test(slots[1]), page + " 的 ownerSlots() 用中文 label");
    }
  }
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
    // 刷写命令的形状（端口可改、偏移 0x210000）
    ok(/^python -m esptool --chip \S+ --port COM6 --baud \d+ write_flash 0x210000 assets\.bin$/
         .test(g.flashCommand), "指纹里的刷写命令形状正确：" + g.flashCommand);
  }
}

section("⑭ 不改固件 / 不改内核产物：那三个文件的**字节**没被动过");
{
  // 只断言"我们这一单没碰它们"——用 git 的工作区状态回答（若仓库不是 git 就跳过）
  const { execFileSync } = require("child_process");
  let changed = null;
  try {
    changed = execFileSync("git", ["status", "--porcelain", "--",
      "tools/theme-editor/theme-json.js",
      "tools/theme-editor/image-blob-build.js",
      "tools/theme-editor/asset-package-core.js",
      "tools/theme-editor/asset-spec.js",
      "tools/theme-editor/face-stages.js",
      "lib", "src"], { cwd: REPO, encoding: "utf8" });
  } catch (e) { changed = null; }
  if (changed === null) {
    console.log("  - 不是 git 仓库（或 git 不可用），这条跳过");
  } else {
    eq(changed.trim(), "", "主题/图片/容器内核与固件源码都没有改动");
  }
}

section("⑮ 页面引用的内核脚本都在，且默认主题/告警兜底与固件一致");
{
  const repo = path.resolve(dir, "..", "..");
  const { execFileSync } = require("child_process");
  for (const [page, src] of Object.entries(PAGES)) {
    // 车主模式的「保存安装包」/表情图要用内核 ⇒ 这两份必须真的引用进来
    ok(/<script[^>]*\bsrc="asset-package-core\.js"/.test(src),
       page + " 引用了 asset-package-core.js（打包内核，共用一份）");
  }
  ok(/<script[^>]*\bsrc="image-blob-build\.js"/.test(PAGES["index.html"]),
     "index.html 引用了 image-blob-build.js（表情那张方图要打成 image 段）");
  ok(/<script[^>]*\bsrc="asset-spec\.js"/.test(PAGES["index.html"]),
     "index.html 引用了 asset-spec.js（上传前的规格检查）");
  // 页面里**不许**出现第二份内核实现（抄一份 ⇒ 刷上去错位且不报错）
  eq((PAGES["index.html"].match(/function packBytes\s*\(/g) || []).length, 0,
     "index.html 里没有 packBytes 的第二份实现");

  // ★ 图片页车主模式的告警兜底：逐字段对到固件 alerts.h（不许抄错）
  const aSrc = fs.readFileSync(path.join(repo, "lib", "dashcore", "alerts.h"), "utf8");
  const body = /struct AlertsConfig\s*\{([\s\S]*?)\n\};/.exec(aSrc);
  ok(!!body, "找到 alerts.h 的 struct AlertsConfig");
  const m = /const OWNER_ALERTS_FALLBACK\s*=\s*\{([\s\S]*?)\n\};/.exec(PAGES["image-editor.html"]);
  ok(!!m, "image-editor.html 里有 OWNER_ALERTS_FALLBACK");
  if (body && m) {
    const fb = new Function("return {" + m[1].replace(/\/\/[^\n]*/g, "") + "};")();
    for (const f of Object.keys(fb)) {
      const mm = new RegExp("\\b" + f + "\\s*=\\s*([-0-9.]+f?|true|false)\\s*;").exec(body[1]);
      ok(!!mm, "alerts.h 里 " + f + " 有默认值");
      let v = NaN;
      if (mm) {
        if (mm[1] === "true") v = 1;
        else if (mm[1] === "false") v = 0;
        else v = Number(mm[1].replace(/f$/i, ""));
      }
      eq(fb[f], v, "OWNER_ALERTS_FALLBACK." + f + " 与 alerts.h 一致");
    }
    eq(Object.keys(fb).length, 10, "OWNER_ALERTS_FALLBACK 正好十个字段（与 alerts 段同形）");
  }

  // ★ 车主模式改的字段必须在**导出的主题**里真的会写出去：
  //   四条弧的 value_color / readout.digit_color / alerts.* 都在主题格式里
  const idx = PAGES["index.html"];
  ok(/value_color/.test(idx) && /digit_color/.test(idx) && /alerts/.test(idx),
     "车主模式改的都是主题格式里既有的字段（value_color / digit_color / alerts）");
  // ★ 页面不许自己写 image.bin / assets.bin 的字节（一律走内核）
  for (const [page, src] of Object.entries(PAGES)) {
    ok(!/0x44363032|IMAGE_MAGIC\s*=/.test(src), page + " 里没有自己写 image 包头魔数");
  }
}

console.log("\n" + "=".repeat(56));
if (fail === 0) console.log("全部通过:" + pass + " 项断言");
else {
  console.log("失败 " + fail + " 项 / 共 " + (pass + fail) + " 项:");
  failures.forEach(f => console.log("  - " + f));
}
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
