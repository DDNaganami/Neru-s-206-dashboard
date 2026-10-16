/* ============================================================
 * check-preview-plate.js —— 「读数底片」的**固件 ↔ 预览镜像**漂移检查
 *
 *   node tools/theme-editor/check-preview-plate.js
 *   退出码：0 = 两边一致（绿）；1 = **镜像尚未落地**（等那一单）；
 *           2 = 两边**真的不一致**（固件改了、预览没跟上 —— 要修的是代码）
 *
 * ------------------------------------------------------------
 * 为什么要有它（2026-10-15）
 * ------------------------------------------------------------
 * 车主："现在板子上的固件的数字是带个底板的，预览里面是没有的。"
 * ⇒ 真机（固件 LVGL）在读数下面垫一块半透明近黑圆角块，而**编辑器预览是浏览器
 *   侧自己画的第二份实现** —— 于是"固件改了底片、预览没跟上"这类漂移**不会报错**，
 *   只会让人看着预览说"跟车上不一样"（本项目最贵的那类 bug）。
 *
 * 所以这一条**读源码、逐个常量对账**（⚠ 全是**只读**）：
 *   · 固件（唯一事实来源）：
 *       `lib/themetool/ui_theme.h`   —— 底片不透明度的**默认值**（R3 之后它住在主题里）
 *       `src/dash_ui.cpp`            —— 底片色 / pad / radius / 层序 / "静态"这条性质
 *   · 预览镜像：`tools/theme-editor/index.html` 里那份 JS 实现
 *
 * ★★ 不硬编码行号：一律用**字段名/函数名/宏名**正则去源码里取值 ——
 *    别人挪了行号、加了注释、重排了函数，这一条都会照旧工作。
 *
 * ★★ R3 之后的形状（2026-10-15 晚，车主"底板深浅要能在编辑器里调"）
 *    —— 这正是本脚本存在的意义：**固件在一天之内从"编译期常量"改成了"主题键"**，
 *       如果哪一边没跟上，这里立刻红。现在两边是：
 *         · 不透明度：主题键 `readout.plate_opa`（0..255，缺键 → **38**）
 *           默认值**只定义一处**：`ui_theme.h` 的 `kReadoutPlateOpaDefault`
 *           渲染侧读宏 `READOUT_PLATE_OPA`（`= g_theme.readout.plate_opa`）
 *         · 色 / pad / radius：**仍然是编译期常量**（"可读性"属实现细节，
 *           不进主题；见 `dash_ui.cpp` 底片那三条铁律）
 *       预览镜像要跟的是**这两个来源**，不是某一份文件。
 *
 * ------------------------------------------------------------
 * ★★ 两条**必须写下来的坑**（它们决定"哪些能对齐、哪些不能"）
 * ------------------------------------------------------------
 * ① **预览要镜像 `ts()` 那层缩放**：固件里所有几何都是
 *    `ts(v480) = lroundf(v480 * theme_scale())`（`dash_ui.cpp` 的 `ts()`），
 *    也就是"**480 基准 × 主题缩放**"；**别把 480 基准的数当设备像素**用。
 *    240 档那块板上 `theme_scale()` ≈ 0.5 ⇒ pad 变 3px、radius 变 5px。
 *    预览里必须走同一套换算，否则两块板的观感不一样。
 * ② **字体墨迹宽高无法逐值对齐**：固件用 LVGL 内置 Montserrat，按
 *    `lv_font_get_glyph_dsc()` 量**字形墨迹盒**（`measure_text()`）；
 *    浏览器用的是系统字体 + 页面自己那张 `READOUT_FONT_PX`（近似替代，见
 *    `index.html` 里 drawReadoutLabel 的注释）⇒ "底片到底多大"两边**注定有差**。
 *    所以本脚本只断言**恒定部分**：色 / 不透明度 / pad / radius / 层序 /
 *    "中心 = 墨迹盒中心、x 恒为屏中心" / "静态、不逐帧"。
 *    墨迹与宽度按"近似 + 注释"处理，**不在这里假装能对齐**。
 *
 * ------------------------------------------------------------
 * 镜像还没落地时会怎样
 * ------------------------------------------------------------
 * 给预览加底片镜像那一单落地之前，`index.html` 里找不到镜像常量 ⇒ 本脚本打印
 * 「镜像尚未落地（等那一单）」并**非零退出（1）**——
 * ★ **故意不静默通过**：静默通过等于"这条守卫一开始就是假的"。
 * 等镜像落地，它自然转绿。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");

const dir = __dirname;
const REPO = path.resolve(dir, "..", "..");

// ★ 可选：`--page=<路径>` 换一份"预览页"来对账。
//   它存在的理由很实际：镜像还没落地时，"不一致"那条分支**没法在真页面上试**
//   （碰不到真文件），就得能拿一份**假夹具**把这条分支也走一遍 ——
//   否则那个分支永远是"写了没验过"的代码。默认仍然是真页面。
const pageArg = (function () {
  const a = process.argv.find(x => x.indexOf("--page=") === 0);
  return a ? a.slice("--page=".length) : null;
})();

const FW_CPP = path.join(REPO, "src", "dash_ui.cpp");
const FW_H = path.join(REPO, "lib", "themetool", "ui_theme.h");
const PAGE = pageArg ? path.resolve(pageArg) : path.join(dir, "index.html");

let pass = 0, fail = 0;
const failures = [];
function ok(cond, what) {
  if (cond) pass++;
  else { fail++; failures.push(what); console.log("  ✗ " + what); }
  return !!cond;
}
function eq(actual, expected, what) {
  ok(actual === expected,
     what + "  (固件 " + JSON.stringify(expected) + "，预览 " + JSON.stringify(actual) + ")");
}
function section(t) { console.log("\n== " + t); }

// 行号（只用于**打印给人看**，绝不当判据）
function lineOf(src, re) {
  const m = re.exec(src);
  if (!m) return -1;
  return src.slice(0, m.index).split("\n").length;
}

// ------------------------------------------------------------
// 一、固件侧：常量（按**宏名/字段名**取，不认行号）
// ------------------------------------------------------------
const hRaw = fs.readFileSync(FW_H, "utf8");
const cRaw = fs.readFileSync(FW_CPP, "utf8");
const fw = {};

section("固件侧（唯一事实来源）：底片常量");
{
  // ---- 不透明度的默认值：只应定义在 ui_theme.h 一处 ----
  const md = /kReadoutPlateOpaDefault\s*=\s*[^;]*?255\s*\*\s*(\d+)\s*\/\s*100\s*\)?\s*;/.exec(hRaw);
  if (ok(!!md, "ui_theme.h 里有 kReadoutPlateOpaDefault = 255 * N / 100")) {
    fw.opaPct = Number(md[1]);
    fw.opa = Math.trunc(255 * fw.opaPct / 100);      // C 的整数除法 = 截断
    console.log("  · kReadoutPlateOpaDefault 在 ui_theme.h:" +
                lineOf(hRaw, /kReadoutPlateOpaDefault\s*=/) +
                " → " + fw.opaPct + "% = " + fw.opa + "/255");
  }
  ok(/static_assert\s*\(\s*kReadoutPlateOpaDefault\s*==\s*38/.test(hRaw),
     "ui_theme.h 有 static_assert 钉住「默认必须是 38/255」");
  // ---- 主题键本身 ----
  ok(/uint8_t\s+plate_opa\s*;/.test(hRaw), "ReadoutTheme 里有字段 plate_opa（主题键 readout.plate_opa）");
  ok(/t\.readout\.plate_opa\s*=\s*kReadoutPlateOpaDefault\s*;/.test(hRaw),
     "theme_set_defaults() 把 plate_opa 的缺省设成 kReadoutPlateOpaDefault");
  ok(/#define\s+READOUT_PLATE_OPA\s+\(?\s*g_theme\.readout\.plate_opa/.test(hRaw),
     "渲染侧读宏 READOUT_PLATE_OPA（= g_theme.readout.plate_opa）");
  // ---- 色 / pad / radius：仍是**编译期常量**（不进主题）----
  const mc = /kReadoutPlateColor\s*=\s*(0x[0-9A-Fa-f]+)\s*;/.exec(cRaw);
  const mp = /kReadoutPlatePad\s*=\s*(\d+)\s*;/.exec(cRaw);
  const mr = /kReadoutPlateRadius\s*=\s*(\d+)\s*;/.exec(cRaw);
  if (ok(!!mc, "dash_ui.cpp 里有 kReadoutPlateColor")) fw.color = Number(mc[1]);
  if (ok(!!mp, "dash_ui.cpp 里有 kReadoutPlatePad")) fw.pad = Number(mp[1]);
  if (ok(!!mr, "dash_ui.cpp 里有 kReadoutPlateRadius")) fw.radius = Number(mr[1]);
  console.log("  · 色 " + (fw.color === undefined ? "?" : "0x" + fw.color.toString(16)) +
              " / pad " + fw.pad + " / radius " + fw.radius + "（都在 src/dash_ui.cpp）");
  // ---- 渲染处必须**读主题**，不许再自带一个字面量 ----
  const hasMacroUse = /lv_obj_set_style_bg_opa\s*\(\s*p\s*,\s*\(?\s*lv_opa_t\s*\)?\s*READOUT_PLATE_OPA\s*,/.test(cRaw);
  ok(hasMacroUse, "make_readout_plate() 用 READOUT_PLATE_OPA 设不透明度（读主题，不自带字面量）");
  const dupOpa = (cRaw.match(/kReadoutPlateOpa\b/g) || []).length;
  ok(dupOpa === 0, "dash_ui.cpp 里**不再有**第二份 kReadoutPlateOpa（只有主题那一处默认值）");
  // ---- ts()：480 基准 × theme_scale() ----
  const mt = /int32_t\s+ts\s*\([^)]*\)\s*\{([^}]*)\}/.exec(cRaw);
  ok(!!mt && /theme_scale\s*\(\s*\)/.test(mt[1]),
     "ts() 走的是 theme_scale()（480 基准 × 主题缩放 —— 镜像别拿 480 当像素）");
  // ---- 尺寸与对齐 ----
  ok(/lv_obj_set_size\s*\(\s*p\s*,[^;]*\+\s*2\s*\*\s*pad[^;]*\+\s*2\s*\*\s*pad\s*\)/.test(cRaw),
     "尺寸 = 墨迹盒 + 2×pad");
  ok(/lv_obj_align\s*\(\s*p\s*,\s*LV_ALIGN_CENTER\s*,\s*0\s*,\s*\([^)]*\)\s*\/\s*2\s*\)/.test(cRaw),
     "底片水平居中（x 偏移 0）且中心 = 墨迹盒中心");
}

section("固件层序：表情 → **底片** → 弧 → 读数（车主定稿的五层）");
{
  // ★ 只认**调用点**：函数定义长这样 `static void build_arcs(lv_obj_t* parent, ...) {`
  //   ⇒ 用"名字 + 参数表 + 紧跟 `{`"排除定义，剩下的最后一次出现就是调用点。
  const callIdx = (name) => {
    const re = new RegExp("\\b" + name + "\\s*\\([^;]*?\\)\\s*;", "g");
    let m, last = -1;
    while ((m = re.exec(cRaw)) !== null) last = m.index;   // 调用以 `;` 结尾
    return last;
  };
  const iFace = callIdx("build_face");
  const iPlate = callIdx("build_readout_plates");
  const iArcs = callIdx("build_arcs");
  const iRead = callIdx("build_readout");
  ok(iFace >= 0 && iPlate >= 0 && iArcs >= 0 && iRead >= 0,
     "四个建屏函数都有调用点（face@" + lineOf(cRaw, /\bbuild_face\s*\([^;]*?\)\s*;/) +
     " plate@" + lineOf(cRaw, /\bbuild_readout_plates\s*\([^;]*?\)\s*;/) + "）");
  ok(iFace < iPlate, "表情在底片**之前**建（底片压在表情之上）");
  ok(iPlate < iArcs, "底片在弧**之前**建（弧画在底片之上 ⇒ 底片啃不到弧）");
  ok(iArcs < iRead, "读数最后建（最高层）");
}

section("固件「底片是静态的」：样式只在建屏那一次写，逐帧那条路不碰它");
{
  const code = cRaw.replace(/\/\*[\s\S]*?\*\//g, "").replace(/(^|[^:])\/\/[^\n]*/g, "$1");
  const body = (name) => {
    const m = new RegExp("(?:static\\s+)?[A-Za-z_][\\w:*&\\s]*\\b" + name +
                         "\\s*\\([^)]*\\)\\s*\\{([\\s\\S]*?)\\n\\}").exec(code);
    return m ? m[1] : null;
  };
  const mk = body("make_readout_plate");
  if (ok(!!mk, "找得到 make_readout_plate() 的函数体")) {
    for (const [call, what] of [
      ["lv_obj_set_style_bg_color", "色"],
      ["lv_obj_set_style_bg_opa", "不透明度"],
      ["lv_obj_set_style_radius", "圆角"]
    ]) {
      const n = (mk.match(new RegExp(call, "g")) || []).length;
      eq(n, 1, "底片" + what + "只在建屏时设一次（" + call + " 出现 " + n + " 次）");
    }
  }
  for (const fn of ["readout_apply", "arc_apply_red"]) {
    const b = body(fn);
    ok(!!b, "固件里找得到 " + fn + "()（逐帧那条路）");
    if (b) ok(!/plate/i.test(b), fn + "() 里**不碰**底片（不跟状态/数值/红区）");
  }
}

// ------------------------------------------------------------
// 二、预览侧镜像：从 index.html 的**脚本正文**里按变量名取（不认行号）
// ------------------------------------------------------------
const pageRaw = fs.readFileSync(PAGE, "utf8");
// ★ 只在 <script> 正文里找：CSS 里同样有 .stage-grid 这类名字，会误命中
const pageCode = (function () {
  const out = [];
  const re = /<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi;
  let m;
  while ((m = re.exec(pageRaw)) !== null) out.push(m[1]);
  return out.join("\n");
})();

section("预览侧镜像：tools/theme-editor/index.html");
const mirror = {};
{
  const grab = (names) => {
    for (const n of names) {
      // ★ 0x 那一支必须排在十进制前面：写成 `([0-9]+|0x[0-9A-Fa-f]+)` 时，
      //   正则会在 `0x000000` 上先匹配到 `[0-9]+` = **"0"**（实测踩过：
      //   于是"色不一致"变成"色 = 0 vs 0"，把真不一致判成一致）。
      const re = new RegExp("\\b" + n + "\\s*=\\s*(0x[0-9A-Fa-f]+|[0-9]+)", "i");
      const m = re.exec(pageCode);
      if (m) return { name: n, val: Number(m[1]) };
    }
    return null;
  };
  mirror.color = grab(["PLATE_COLOR", "READOUT_PLATE_COLOR", "PLATE_BG_COLOR",
                       "PLATE_FILL", "PLATE_INK"]);
  mirror.opa = grab(["PLATE_OPA", "READOUT_PLATE_OPA", "PLATE_ALPHA", "PLATE_OPACITY"]);
  mirror.pad = grab(["PLATE_PAD", "READOUT_PLATE_PAD", "PLATE_PADDING"]);
  mirror.radius = grab(["PLATE_RADIUS", "READOUT_PLATE_RADIUS", "PLATE_R"]);
}
// 「镜像落地了没有」的判据：**只认实现痕迹**，不认"出现过 plate 这个字" ——
//   ★ 实测踩过：预览里已经有 `plate_opa`（主题键、车主模式那个深浅滑块用的），
//     宽松地搜 /\bplate\b/ 会命中它 ⇒ 把"还没落地"误判成"落地了但值不对"（红错等级）。
const hasPlateDraw = /function\s+\w*[Pp]late\w*\s*\(/.test(pageCode);
const hasPlateConst = /\b[A-Z_]*PLATE[A-Z_]*\s*=\s*[0-9]/.test(pageCode);
const mirrorLanded = !!((mirror.color || mirror.opa || mirror.pad || mirror.radius) &&
                        (hasPlateDraw || hasPlateConst));

if (!mirrorLanded) {
  // ★ 故意非零退出：静默通过 = 这条守卫一开始就是假的
  console.log("  · 预览侧：没有找到底片常量 + 绘制函数（只有主题键 plate_opa，那不算镜像）。");
  console.log("\n  镜像尚未落地（等「给预览加底片镜像」那一单）。");
  console.log("  固件侧那些常量都已核对（见上）；等那一单把镜像加上，本检查自然转绿。");
  console.log("\n" + "=".repeat(56));
  console.log("镜像尚未落地 —— 退出码 1（不是失败，是「还没做」）");
  console.log("  固件侧已核对：" + pass + " 项");
  console.log("=".repeat(56));
  process.exit(1);
}

section("逐值对账：固件 ↔ 预览镜像");
{
  const nameOf = (x) => (x ? x.name : "（缺）");
  // 色：固件是 0x000000
  eq(mirror.color ? mirror.color.val : null, fw.color,
     "底片色（镜像变量 " + nameOf(mirror.color) + "）");
  // 不透明度：**必须与主题同一个域 0..255** —— `ui_theme.h` 里写得很死：
  //   "别在这里再发明 0..100 或 0..1"（`track_opa` 那套就是 0..255，
  //    车主模式**显示**成百分比、存进文件的仍是 0..255）。
  //   ★ 所以这里**不**做"≤100 就当百分比"的宽容换算 —— 第一版做了那个换算，
  //     结果把夹具里的 38 当成 38% 换出 96/255，**把"值不对"报成了"域不对"**
  //     （两个错叠在一起、现场极难看懂）。宁可如实报差值，并提示域的口径。
  if (mirror.opa) {
    eq(mirror.opa.val, fw.opa, "底片不透明度（" + mirror.opa.name + "，0..255 域）");
    if (mirror.opa.val !== fw.opa && mirror.opa.val <= 100) {
      console.log("    ↳ 提示：镜像里是 " + mirror.opa.val +
                  "，看着像**百分比**；主题那个键的域是 0..255" +
                  "（" + fw.opaPct + "% ⇒ " + fw.opa + "），预览也该存 0..255。");
    }
  } else {
    ok(false, "预览镜像里没有底片不透明度常量（PLATE_OPA 一类）");
  }
  eq(mirror.pad ? mirror.pad.val : null, fw.pad, "内边距（480 基准，" + nameOf(mirror.pad) + "）");
  eq(mirror.radius ? mirror.radius.val : null, fw.radius, "圆角（480 基准，" + nameOf(mirror.radius) + "）");
  ok(hasPlateDraw, "预览里有一个 draw*Plate*()（真的画出来，不只是常量）");
  // 缩放：预览里必须能看见 480 基准 → 画布的那层换算（ts / theme_scale / W/480）
  ok(/theme_scale|W\s*\/\s*480|\bts\s*\(/.test(pageCode),
     "预览里有 480 基准的缩放换算（别拿 480 的数当像素）");
  // 层序：预览里也要有"表情 → 底片 → 弧 → 读数"的顺序
  //   ★ 必须只认**调用**：函数定义会先出现（`function drawReadoutPlate(...)`），
  //     只看"名字第一次出现"会把定义当成调用点 ⇒ 假红（实测踩过）。
  //     这里用"前面不是 `function`、后面不是 `)` 紧跟 `{`"来认调用。
  const callPos = (nameRe) => {
    const re = new RegExp("(^|[^.\\w])(" + nameRe + ")\\s*\\(", "g");
    let m;
    while ((m = re.exec(pageCode)) !== null) {
      const before = pageCode.slice(Math.max(0, m.index - 24), m.index + m[0].length);
      if (/\bfunction\s*$/.test(before) || /\bfunction\s+\w*\s*$/.test(before)) continue;  // 定义
      const after = pageCode.slice(m.index + m[0].length - 1);
      if (/^\s*\([^)]*\)\s*\{/.test(after)) continue;                                     // 定义（带参）
      return m.index;
    }
    return -1;
  };
  const pFace = callPos("drawFace|blitCentered");
  const pPlate = callPos("draw\\w*[Pp]late\\w*");
  const pArc = callPos("drawArc");
  const pRead = callPos("drawReadout");
  ok(pFace >= 0 && pPlate >= 0 && pArc >= 0 && pRead >= 0,
     "预览里四处调用都能定位（face=" + pFace + " plate=" + pPlate +
     " arc=" + pArc + " readout=" + pRead + "）");
  ok(pFace < 0 || pPlate > pFace, "预览里底片画在表情**之后**（压在表情之上）");
  ok(pArc < 0 || pPlate < pArc, "预览里弧画在底片**之后**（与固件同一层序：弧不会被底片盖）");
  ok(pRead < 0 || pArc < pRead || pArc < 0, "预览里读数在弧之后（最高层）");
}

// ------------------------------------------------------------
// 三、R4（2026-10-15）：**主读数单位**的颜色 = 该屏数字颜色（两边同一套）
//
//   车主原话："rpm / km/h 的字体颜色是不是漏掉了，分别和转速/速度两个字体颜色绑定好了。"
//   ⇒ 单位不再读共用的 `readout.unit_color`，而是跟着本屏的数字色（左屏 rpm_color /
//     右屏 speed_color）。这一节钉的正是"两边都跟、而且都没有再回头去读 unit_color"
//     —— 属于本脚本的份内事（真机与预览的漂移检查）。
//   ★ `unit_color` 键**保留**（老主题文件不许"静默换意思"）：所以这里断言的是
//     "没有任何渲染代码用它"，而不是"它不存在"。
// ------------------------------------------------------------
section("R4：主读数单位颜色 = 该屏数字颜色（固件 ↔ 预览镜像）");
{
  ok(/unit_lbl\s*=\s*make_readout_label\(\s*parent,\s*READOUT_UNIT_FONT,\s*lv_color_hex\(digit_base_color\(pk\)\)/
       .test(cRaw),
     "固件：单位标签用 digit_base_color(pk) 建（与数字同一个值）");
  ok(!/lv_color_hex\(READOUT_UNIT_COLOR\)/.test(cRaw),
     "固件：没有任何地方再拿 unit_color 画单位");
  ok(/uint32_t\s+unit_color\s*;/.test(hRaw),
     "固件：unit_color 字段**保留**（键还在，只是没有渲染消费者）");
  ok(/drawReadoutLabel\(ctx, W, L\.unit_cy, u, unitPx, hexOf\(digitColor\)\)/.test(pageCode),
     "预览：单位用的是 digitColor（该屏数字色）");
  ok(!/hexOf\(r\.unit_color\)/.test(pageCode),
     "预览：没有任何地方再拿 unit_color 画东西");
  ok(/主题格式修订 R4/.test(hRaw), "固件头文件里有 R4 的修订注记");
}

console.log("\n" + "=".repeat(56));
if (fail === 0) {
  console.log("全部通过:" + pass + " 项断言（固件与预览镜像一致）");
} else {
  console.log("失败 " + fail + " 项 / 共 " + (pass + fail) + " 项：");
  failures.forEach(f => console.log("  - " + f));
}
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 2);
