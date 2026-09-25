/* ============================================================
 * 「左屏/右屏」的**口径对账** —— 编辑器 ↔ 固件（2026-09-26 新增）
 *
 * 用途：`node tools/theme-editor/test-role-orientation.js`
 *
 * ------------------------------------------------------------
 * 为什么需要它
 * ------------------------------------------------------------
 * 车主 2026-09-26 那一单里明确点了一条："**顺手核对编辑器的左右口径与固件现在的映射
 * 是不是同一个方向** —— 如果编辑器说的"左屏"在固件里落到右屏，那是**口径反了**，
 * 要连编辑器文案/文档一起对齐，别只改一边。"
 *
 * 这条方向**两边各写了一份**（而且是必须的：编辑器要在浏览器里画出"左/右两块屏"，
 * 固件要按角色选出 `screens[0]` / `screens[1]`）：
 *   · 固件侧：`lib/dashcore/dash_role_layout.h`（`themeIndexForScreen` 等）
 *             + `lib/dashcore/face_stages.h`（`kFaceRoleId[组][槽位]`）
 *   · 编辑器侧：`tools/theme-editor/face-stages.js` 的 `SCREENS`（`key/idx/gauge`）
 *             + `tools/theme-editor/asset-spec.js` 的用途标签（"左屏·怠速" …）
 * 两份口径**一旦反了不会报错**：编辑器里调的是"左屏的表情"，刷上去出现在右屏上
 * —— 而"左上角那个标题"和"屏上的实际位置"没人会同时盯着看。
 *
 * ⇒ 本文件把"**同一个方向**"变成**可执行的判据**：逐条比对上面那四处的数字/名字。
 *   ★ 它是 `test-face-stages.js`（角色号对账）与 `test-theme-json.js`（弧的对账）
 *     之外的第三条：那两条各管一半，这一条专门管**"左右"这两个字**。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");

const FS_JS = require("./face-stages.js");
const AS = require("./asset-spec.js");

let pass = 0, fail = 0;
const failures = [];
function ok(cond, what) {
  if (cond) pass++;
  else { fail++; failures.push(what); console.log("  \u2717 " + what); }
}
function eq(a, b, what) {
  ok(a === b, what + "  (期望 " + JSON.stringify(b) + ",\u5f97\u5230 " + JSON.stringify(a) + ")");
}
function section(t) { console.log("\n== " + t); }

const repoRoot = path.resolve(__dirname, "..", "..");
const layoutH = fs.readFileSync(path.join(repoRoot, "lib", "dashcore", "dash_role_layout.h"), "utf8");
const stagesH = fs.readFileSync(path.join(repoRoot, "lib", "dashcore", "face_stages.h"), "utf8");
const themeH  = fs.readFileSync(path.join(repoRoot, "lib", "themetool", "ui_theme.h"), "utf8");
const imageH  = fs.readFileSync(path.join(repoRoot, "lib", "themetool", "image_blob.h"), "utf8");

// ------------------------------------------------------------
// 一、固件侧：把 `roleThemeIndex()` 的两个分支读出来
//   #if LINK_ROLE == 1
//     return 1u;   // 主板（右）：速度表 + 进气温度
//   #else
//     return 0u;   // 从板（左）：转速表 + 水温
// ------------------------------------------------------------
section("一、固件侧：角色 \u2192 主题下标（dash_role_layout.h）");
const roleFn = /inline\s+uint8_t\s+roleThemeIndex\(\)\s*\{([\s\S]*?)\n\}/.exec(layoutH);
ok(!!roleFn, "\u80fd\u89e3\u6790\u51fa roleThemeIndex()");
const fnBody = roleFn ? roleFn[1] : "";
const masterIdx = Number((/#if\s+LINK_ROLE\s*==\s*1[\s\S]*?return\s+(\d+)u/.exec(fnBody) || [])[1]);
const slaveIdx  = Number((/#else[\s\S]*?return\s+(\d+)u/.exec(fnBody) || [])[1]);
eq(masterIdx, 1, "\u4e3b\u677f\uff08LINK_ROLE==1\uff09\u8be5\u7528\u4e3b\u9898\u4e0b\u6807 1\uff08= screens[1]\uff09");
eq(slaveIdx, 0, "\u4ece\u677f\uff08LINK_ROLE==0\uff09\u8be5\u7528\u4e3b\u9898\u4e0b\u6807 0\uff08= screens[0]\uff09");

// `gaugeKindForScreen` 的方向：theme 1 = Speed、theme 0 = Rpm
const gaugeFn = /inline\s+GaugeKind\s+gaugeKindForScreen\([\s\S]*?\n\}/.exec(layoutH);
ok(!!gaugeFn, "\u80fd\u89e3\u6790\u51fa gaugeKindForScreen()");
const gaugeBody = gaugeFn ? gaugeFn[0] : "";
ok(/==\s*1u\s*\)\s*\?\s*kGaugeSpeed\s*:\s*kGaugeRpm/.test(gaugeBody),
   "\u4e3b\u9898\u4e0b\u6807 1 \u21d2 \u901f\u5ea6\u8f74\uff08Speed\uff09\u3001\u5426\u5219 \u21d2 \u8f6c\u901f\u8f74\uff08Rpm\uff09");

// ------------------------------------------------------------
// 二、固件侧：主题的两个槽，谁是哪块表
//   从 ui_theme.h 的 theme_reset_to_defaults() 读：screens[0] 外弧 kind、screens[1] 外弧 kind
//   弧 kind：0 = Speed、1 = Rpm、2 = Coolant、3 = Intake（ui_theme.h 的 ArcKind）
// ------------------------------------------------------------
section("二、固件侧：主题两个槽的\u4e3b\u8868（ui_theme.h 的默认值）");
// `theme_reset_to_defaults()` 里是 `L.arcs[0] = ArcStyle{ ArcKind::Rpm, 135, 405, ... };`
// ★ 从 `t.screens[N]` 那一行往后找**第一个** `arcs[0] = ArcStyle{ ArcKind::X` ——
//   不写死颜色/角度（那些会被改，我们只关心**哪块表**）。
//   弧 kind：0 = Speed、1 = Rpm、2 = Coolant、3 = Intake（ui_theme.h 的 ArcKind 顺序）。
function firstArcKindFor(idx) {
  const re = new RegExp("t\\.screens\\[" + idx + "\\]([\\s\\S]*?)\\.arcs\\[0\\]\\s*=\\s*ArcStyle\\{\\s*ArcKind::(\\w+)");
  const m = re.exec(themeH);
  if (!m) return NaN;
  return { Speed: 0, Rpm: 1, Coolant: 2, Intake: 3 }[m[2]];
}
const kindLeft = firstArcKindFor(0), kindRight = firstArcKindFor(1);
eq(kindLeft, 1, "screens[0] \u5916\u5f27 = Rpm\uff08\u5de6\u5c4f = \u8f6c\u901f\u8868\uff09");
eq(kindRight, 0, "screens[1] \u5916\u5f27 = Speed\uff08\u53f3\u5c4f = \u901f\u5ea6\u8868\uff09");

// ------------------------------------------------------------
// 三、★ 方向对账（本文件存在的理由）
//   固件：screens[0] = Rpm、screens[1] = Speed
//   编辑器：SCREENS[0].key === "left" 且 gauge === "rpm"；
//           SCREENS[1].key === "right" 且 gauge === "speed"
//   ⇒ "编辑器说的左屏" 就是 "固件里的 screens[0]" 就是 "转速表"。
// ------------------------------------------------------------
section("三、\u2605 \u65b9\u5411\u5bf9\u8d26\uff1a\u7f16\u8f91\u5668\u7684\u201c\u5de6/\u53f3\u201d == \u56fa\u4ef6\u7684 screens[0]/[1]");
const S0 = FS_JS.screen(0), S1 = FS_JS.screen(1);
eq(S0.idx, 0, "\u7f16\u8f91\u5668\u7b2c\u4e00\u9875\u7684 idx = 0\uff08\u5bf9\u5e94 screens[0]\uff09");
eq(S1.idx, 1, "\u7f16\u8f91\u5668\u7b2c\u4e8c\u9875\u7684 idx = 1\uff08\u5bf9\u5e94 screens[1]\uff09");
eq(S0.key, "left", "\u7f16\u8f91\u5668 idx 0 \u7684 key \u662f left");
eq(S1.key, "right", "\u7f16\u8f91\u5668 idx 1 \u7684 key \u662f right");
eq(S0.gauge, "rpm", "\u7f16\u8f91\u5668\u5de6\u5c4f\u7684\u4e3b\u8868\u662f rpm");
eq(S1.gauge, "speed", "\u7f16\u8f91\u5668\u53f3\u5c4f\u7684\u4e3b\u8868\u662f speed");
// 两边合起来的**那一条**：主题槽的表 == 编辑器同一 idx 的 gauge
ok((kindLeft === 1) === (S0.gauge === "rpm"),
   "\u7b2c 0 \u9875\uff1a\u56fa\u4ef6\u4e3b\u9898\u69fd\u4e0e\u7f16\u8f91\u5668\u8bf4\u7684\u662f\u540c\u4e00\u5757\u8868\uff08Rpm\uff09");
ok((kindRight === 0) === (S1.gauge === "speed"),
   "\u7b2c 1 \u9875\uff1a\u56fa\u4ef6\u4e3b\u9898\u69fd\u4e0e\u7f16\u8f91\u5668\u8bf4\u7684\u662f\u540c\u4e00\u5757\u8868\uff08Speed\uff09");
// 副表也要同向（左=水温、右=进气温度）—— 编辑器的 label 里写着
ok(/水温/.test(S0.label) && !/进气/.test(S0.label), "\u5de6\u5c4f\u6807\u9898\u5199\u7740\u6c34\u6e29\u3001\u4e0d\u5199\u8fdb\u6c14\u6e29\u5ea6");
ok(/进气温度/.test(S1.label) && !/水温/.test(S1.label), "\u53f3\u5c4f\u6807\u9898\u5199\u7740\u8fdb\u6c14\u6e29\u5ea6\u3001\u4e0d\u5199\u6c34\u6e29");

// ------------------------------------------------------------
// 四、表情角色号：编辑器 SCREENS[*].states[*].role == 固件 kFaceRoleId[组][槽位]
//   ★ 组 == 编辑器 idx（都是"哪块表"）—— 这一条正是本单 B 修的那个错位的对账。
//
//   ★★ 为什么**不能**按下标逐个比（第一版就是这么写的，当场红了 5 条）：
//     固件那张 `kFaceRoleId[组][槽位]` 的**槽位号是 (uint8_t)Face 的枚举顺序**
//     （Idle=0, Cruise=1, Sport=2, Redline=3, Overspeed=4, High=5, City=6），
//     而编辑器那份是**按档位从低到高**排的（左：怠速/巡航/运动/高转/红区）。
//     两边的**顺序本来就不同**（高转在固件里是槽位 5、在编辑器里是第 4 个）
//     ⇒ 只有"**状态名 → 角色号**"这个映射是可比的。
//     ★ 这正好也是本文件要守的那件事："左右"这两个字，不是"第几个"。
//
//   状态名从**固件自己的两张表**里读（不手抄）：
//     `Face::X` 的槽位号由 `expression.h` 的枚举顺序给出，
//     `kFaceLeftStates / kFaceRightStates` 给出"哪一屏有哪些状态"。
// ------------------------------------------------------------
section("四、\u8868\u60c5\u89d2\u8272\u53f7\uff1a\u7f16\u8f91\u5668\u7684 SCREENS[idx].states \u2194 \u56fa\u4ef6 kFaceRoleId[\u7ec4]");
const roleRe = /kFaceRoleId\[(\d+)\]\[(\d+)\]\s*=\s*\{([\s\S]*?)\};/;
const roleBlock = roleRe.exec(stagesH);
ok(!!roleBlock, "\u80fd\u89e3\u6790\u51fa kFaceRoleId");
const cppRoles = [];
if (roleBlock) {
  const rows = roleBlock[3].split("\n")
    .map(l => l.replace(/\/\/.*$/, "").trim())
    .filter(l => l.startsWith("{"));
  for (const row of rows) {
    cppRoles.push(row.replace(/[{}]/g, "").split(",").map(v => Number(v.trim())).filter(v => !Number.isNaN(v)));
  }
}
eq(cppRoles.length, 2, "\u56fa\u4ef6\u4fa7\u6709\u4e24\u7ec4\uff08\u5de6/\u53f3\uff09");

// Face 枚举顺序 → 槽位号（expression.h 是唯一出处）
// ★ 解析三条纪律（第一版就在这儿红过）：
//   ① 每一行**必须先去掉 `//` 注释**再 split —— 枚举里每个成员后面都跟了一句中文注释，
//      注释里还有"（）/ 数字"，带着它去 split(',') 会把注释里的逗号当成成员分隔符；
//   ② `Count` 是**哨兵**，不是成员（camera 的那条 slot 号不能被它顶掉）；
//   ③ 只认 `[A-Za-z_][A-Za-z0-9_]*` 形状的名字（去掉注释后剩下的空白/逗号不算）。
const faceEnum = (function () {
  const m = /enum class Face\s*:\s*uint8_t\s*\{([\s\S]*?)\}/.exec(
    fs.readFileSync(path.join(repoRoot, "lib", "dashcore", "expression.h"), "utf8"));
  const out = {};
  if (!m) return out;
  let idx = 0;
  for (let raw of m[1].split(",")) {
    raw = raw.replace(/\/\/[^\n]*/g, "").trim();
    if (!raw) continue;
    const name = raw.split("=")[0].trim();
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) continue;
    if (name === "Count") continue;          // 哨兵，不占槽位
    out[name] = idx++;
  }
  return out;
})();
ok(Object.keys(faceEnum).length >= 7,
   "\u80fd\u89e3\u6790\u51fa Face \u679a\u4e3e\u7684\u69fd\u4f4d\u53f7\uff08\u5b9e\u9645 " + Object.keys(faceEnum).length + " \u4e2a\uff09");

// 固件侧：状态名 → 该屏的角色号（只取"这一屏列出了的状态"）
function cppRoleByStateName(group) {
  const listRe = group === 0
    ? /kFaceLeftStates\[\]\s*=\s*\{([^}]*)\}/
    : /kFaceRightStates\[\]\s*=\s*\{([^}]*)\}/;
  const m = listRe.exec(stagesH);
  const out = {};
  if (!m) return out;
  for (const raw of m[1].split(",")) {
    const name = raw.trim().replace(/^Face::/, "");
    if (!name) continue;
    const slot = faceEnum[name];
    if (slot === undefined) continue;
    out[name] = cppRoles[group][slot];
  }
  return out;
}

for (let g = 0; g < 2; g++) {
  const ed = FS_JS.screen(g);
  const byName = cppRoleByStateName(g);
  eq(Object.keys(byName).length, ed.states.length,
     "\u7b2c " + g + " \u7ec4\uff1a\u56fa\u4ef6\u5217\u51fa\u7684\u72b6\u6001\u6570 == \u7f16\u8f91\u5668\u72b6\u6001\u6570");
  for (const st of ed.states) {
    eq(st.role, byName[st.key],
       "\u7b2c " + g + " \u7ec4\uff1a" + st.label + "\uff08" + st.key + "\uff09\u7684\u89d2\u8272\u53f7");
  }
}
// 取图那一侧也要同向：`faceIndexForScreen` 与 `themeIndexForScreen` 恒等（固件里是一行）
ok(/inline\s+uint8_t\s+faceIndexForScreen\(uint8_t\s+s\)\s*\{\s*return\s+themeIndexForScreen\(s\);\s*\}/.test(layoutH),
   "\u56fa\u4ef6\u4fa7\uff1afaceIndexForScreen(s) \u6052\u7b49\u4e8e themeIndexForScreen(s)\uff08\u56fe\u7247\u5206\u7ec4 == \u8868\u76d8\u4e0b\u6807\uff09");
ok(/inline\s+uint8_t\s+faceImageIndexForScreen\(uint8_t\s+s\)\s*\{\s*return\s+faceIndexForScreen\(s\);\s*\}/.test(layoutH),
   "\u56fa\u4ef6\u4fa7\uff1afaceImageIndexForScreen(s) \u6052\u7b49\u4e8e faceIndexForScreen(s)");

// ------------------------------------------------------------
// 五、图片用途标签（asset-spec.js 的 ROLES）：写着"左屏"的那几个角色号 vs 固件左组
//   ★ 这一条防的是"编辑器页面上的标签写反了"（用户照标签导图 ⇒ 导错屏）。
//   ★ `ROLES` 是 `image-editor.html` 与 `index.html` 两页共用的那张表
//     （`test-asset-spec.js` 已经在钉它的**内容**，这里只钉**左右方向**）。
// ------------------------------------------------------------
section("五、\u56fe\u7247\u7528\u9014\u6807\u7b7e\uff08asset-spec.js \u7684 ROLES\uff09\u7684\u201c\u5de6/\u53f3\u201d");
const roles = Array.isArray(AS.ROLES) ? AS.ROLES : [];
ok(roles.length > 0, "\u80fd\u8bfb\u5230 asset-spec.js \u7684 ROLES");
{
  const cppLeft = (cppRoles[0] || []).filter(v => v !== 0);
  const cppRight = (cppRoles[1] || []).filter(v => v !== 0);
  const labeledLeft = roles.filter(p => /^\u5de6\u5c4f/.test(p.label || "")).map(p => p.role);
  const labeledRight = roles.filter(p => /^\u53f3\u5c4f/.test(p.label || "")).map(p => p.role);
  eq(labeledLeft.length, cppLeft.length, "\u6807\u7740\u201c\u5de6\u5c4f\u201d\u7684\u7528\u9014\u6570 == \u56fa\u4ef6\u5de6\u7ec4\u89d2\u8272\u6570");
  eq(labeledRight.length, cppRight.length, "\u6807\u7740\u201c\u53f3\u5c4f\u201d\u7684\u7528\u9014\u6570 == \u56fa\u4ef6\u53f3\u7ec4\u89d2\u8272\u6570");
  for (const r of labeledLeft) ok(cppLeft.indexOf(r) >= 0, "\u6807\u7740\u201c\u5de6\u5c4f\u201d\u7684\u89d2\u8272 " + r + " \u786e\u5b9e\u5728\u56fa\u4ef6\u5de6\u7ec4");
  for (const r of labeledRight) ok(cppRight.indexOf(r) >= 0, "\u6807\u7740\u201c\u53f3\u5c4f\u201d\u7684\u89d2\u8272 " + r + " \u786e\u5b9e\u5728\u56fa\u4ef6\u53f3\u7ec4");
  // 反向：固件左组的角色不许被标成"右屏"（这就是"口径反了"的可执行形态）
  for (const r of cppLeft) ok(labeledRight.indexOf(r) < 0, "\u56fa\u4ef6\u5de6\u7ec4\u89d2\u8272 " + r + " \u4e0d\u8be5\u88ab\u6807\u6210\u201c\u53f3\u5c4f\u201d");
  for (const r of cppRight) ok(labeledLeft.indexOf(r) < 0, "\u56fa\u4ef6\u53f3\u7ec4\u89d2\u8272 " + r + " \u4e0d\u8be5\u88ab\u6807\u6210\u201c\u5de6\u5c4f\u201d");
  // 背景那一张必须**不带**左右（两屏共用一张）—— 带了就说明有人把它也当成了"按屏分"
  const bg = roles.filter(p => p.role === 1);
  eq(bg.length, 1, "\u80cc\u666f\u89d2\u8272 1 \u53ea\u6709\u4e00\u6761\u7528\u9014");
  if (bg.length === 1) ok(!/\u5de6\u5c4f|\u53f3\u5c4f/.test(bg[0].label),
                        "\u80cc\u666f\u90a3\u4e00\u6761\u7684\u6807\u7b7e\u4e0d\u5e26\u201c\u5de6/\u53f3\u5c4f\u201d\uff08\u4e24\u5c4f\u5171\u7528\uff09");
}

// ------------------------------------------------------------
console.log("\n" + "=".repeat(56));
if (fail === 0) {
  console.log("\u5168\u90e8\u901a\u8fc7:" + pass + " \u9879\u65ad\u8a00");
} else {
  console.log("\u5931\u8d25 " + fail + " \u9879 / \u901a\u8fc7 " + pass + " \u9879");
  for (const f of failures) console.log("  - " + f);
}
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
