/* ============================================================
 * test-face-stages.js —— 网页端阶段表与固件表的**对账**(Node 运行)
 *
 *   node tools/theme-editor/test-face-stages.js
 *
 * 为什么需要它:
 *   表情导入页的"阶段模拟"直接读 face-stages.js,然后告诉用户
 *   "转速·中 = 左屏巡航脸、右屏常态脸"。这是用户刷图之前**唯一**能看到的证据。
 *   如果网页那份和固件那份不一样,预览就是在骗人,而真车上的表现要到刷完图才知道。
 *
 * 做法:直接**解析 lib/dashcore/face_stages.h 的源码文本**(那几张表),
 * 与 JS 镜像逐字段比对。不搞代码生成 —— 表很小,解析比生成好维护。
 *
 * ★ 这一轮的重点是"**每屏一套独立表情**":
 *   两边都必须同意"左屏哪些状态 / 右屏哪些状态",以及"某阶段只有哪一屏会变"。
 *   这两件事写错了都不会报错(只会有一张脸永远不出现),所以必须机器校验。
 *
 * 链路闭环(每一环都由某条测试钉住):
 *   face_stages.h ──(本文件)──> face-stages.js ──> 表情导入页 / 主题编辑器
 *        │                                              │
 *        └──(test_image_blob.cpp)──> image_blob.h <──────┘
 *                                     ↑
 *                        image-blob-build.js(本文件同时比对 ROLE)
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");

const FS_JS = require("./face-stages.js");
const ImageBlob = require("./image-blob-build.js");

let pass = 0, fail = 0;
const failures = [];

function ok(cond, what) {
  if (cond) { pass++; }
  else { fail++; failures.push(what); console.log("  ✗ " + what); }
}
function eq(a, b, what) {
  ok(a === b, what + "  (期望 " + JSON.stringify(b) + ",得到 " + JSON.stringify(a) + ")");
}
function section(t) { console.log("\n== " + t); }

const dir = __dirname;
const repoRoot = path.resolve(dir, "..", "..");
const stagesH = fs.readFileSync(
  path.join(repoRoot, "lib", "dashcore", "face_stages.h"), "utf8");
const stateH = fs.readFileSync(
  path.join(repoRoot, "lib", "dashcore", "vehicle_state.h"), "utf8");

// 量程上限:画弧进度要用,两边不一致的表现是"预览里弧的进度和真车不同"(看不出来)
const maxRe = /kRpmMax\s*=\s*([\d.]+)f/;
const maxSpeedRe = /kSpeedMax\s*=\s*([\d.]+)f/;
const cppRpmMax = Number((maxRe.exec(stateH) || [])[1]);
const cppSpeedMax = Number((maxSpeedRe.exec(stateH) || [])[1]);

// ------------------------------------------------------------
// 解析 face_stages.h
// ------------------------------------------------------------

// kFaceStages 行:{"group", "level", 转速, 速度, 水温, 进气温度, Face::左, Face::右},
// ★ 列数变了要改这条正则(进气温度那一列是 2026-09 加的)。
const stagesRe =
  /\{\s*"(\w+)"\s*,\s*"(\w+)"\s*,\s*([\d.]+)f\s*,\s*([\d.]+)f\s*,\s*([\d.]+)f\s*,\s*([\d.]+)f\s*,\s*Face::(\w+)\s*,\s*Face::(\w+)\s*\}/g;
const cppStages = [];
let m;
while ((m = stagesRe.exec(stagesH)) !== null) {
  cppStages.push({
    group: m[1], level: m[2],
    rpm: Number(m[3]), speed: Number(m[4]), coolant: Number(m[5]), intake: Number(m[6]),
    left: m[7], right: m[8]
  });
}

// kFaceFallback 行:{FACE_SLOT(A), FACE_SLOT(B), ...},
// ★ 这里**不写死每行几个**:链长必须等于"每屏状态数",而那个数会随档位变化
//   (2026-09-18 从 4 档扩到 5 档时,写死 4 个的正则直接一行都匹配不到,
//    报出来的是"解析出 0 行降级链",看不出真正原因)。
//   改成:先按行抓出整行,再数里面的 FACE_SLOT —— 条数由下面的用例断言。
const fallbackRowRe = /\{([^{}]*FACE_SLOT\([^{}]*)\}/g;
const fallbackItemRe = /FACE_SLOT\((\w+)\)/g;
const cppFallback = [];
while ((m = fallbackRowRe.exec(stagesH)) !== null) {
  const names = [];
  let im;
  fallbackItemRe.lastIndex = 0;
  while ((im = fallbackItemRe.exec(m[1])) !== null) names.push(im[1]);
  cppFallback.push(names);
}

// kFaceRoleId[2][7] = { {...}, {...} };   ← 0 表示这屏用不到这个状态
const roleRe = /kFaceRoleId\[(\d+)\]\[(\d+)\]\s*=\s*\{([\s\S]*?)\};/;
const roleBlock = roleRe.exec(stagesH);
const cppRoleDims = roleBlock ? [Number(roleBlock[1]), Number(roleBlock[2])] : [0, 0];
const cppRoles = [];
if (roleBlock) {
  const rowRe = /\{([\d,\s]+)\}/g;
  let rm;
  while ((rm = rowRe.exec(roleBlock[3])) !== null) {
    cppRoles.push(rm[1].split(",").map(s => Number(s.trim())).filter(n => !isNaN(n)));
  }
}

// kFaceLeftStates / kFaceRightStates = {Face::A, Face::B, ...};
function parseStateList(name) {
  const re = new RegExp(name + "\\[\\]\\s*=\\s*\\{([^}]*)\\}");
  const hit = re.exec(stagesH);
  if (!hit) return null;
  return hit[1].split(",").map(s => s.trim())
    .map(s => (/^Face::(\w+)$/.exec(s) || [])[1])
    .filter(Boolean);
}
const cppLeftStates = parseStateList("kFaceLeftStates");
const cppRightStates = parseStateList("kFaceRightStates");

// 解析不出来就说明 face_stages.h 的排版被改了 —— 直接失败,别静默跳过
section("face_stages.h 可解析");
ok(cppStages.length === 16, "解析出 16 条阶段用例(得到 " + cppStages.length + ")");
ok(cppFallback.length === 7, "解析出 7 行降级链(得到 " + cppFallback.length + ")");
// 每行的链长必须等于"每屏状态数"(5) —— 链短了就会出现"降级不到任何一张图"
for (let i = 0; i < cppFallback.length; i++) {
  eq(cppFallback[i].length, 5, "降级链第 " + i + " 行有 5 个候选");
}
ok(cppRoleDims[0] === 2 && cppRoleDims[1] === 7,
   "角色编号表的维度是 [2][7](得到 [" + cppRoleDims.join("][") + "])");
ok(cppRoles.length === 2 && cppRoles[0].length === 7 && cppRoles[1].length === 7,
   "解析出 2×7 的角色编号表");
ok(!!cppLeftStates && cppLeftStates.length === 5, "解析出左屏状态表(5 个)");
ok(!!cppRightStates && cppRightStates.length === 5, "解析出右屏状态表(5 个)");
// ★ 这条守卫的数字必须跟着契约走(阶段条数 / 降级链行数 / 每屏状态数 / 槽位数),
//   否则会出现最坏的情况:**解析失败时守卫先说"排版被改了"**,
//   而真正原因(数字没同步)被这句警告盖住 —— 2026-09-18 加了两个用例就是这个症状。
const kExpectStages = 16;      // 5 转速 + 5 车速 + 3 水温 + 3 进气
const kExpectSlots  = 7;       // 槽位数 = Face::Count
const kExpectStates = 5;       // 每屏状态数
if (cppStages.length !== kExpectStages || cppFallback.length !== kExpectSlots ||
    cppRoles.length !== 2 || (cppRoles[0] || []).length !== kExpectSlots ||
    !cppLeftStates || cppLeftStates.length !== kExpectStates ||
    !cppRightStates || cppRightStates.length !== kExpectStates) {
  console.log("\n  ⚠ face_stages.h 里的表格**没解析成预期形状**(可能是排版被改,");
  console.log("    也可能是本测试的预期数字没跟着契约更新)。实测:");
  console.log("      阶段用例 " + cppStages.length + " 条(期望 " + kExpectStages + ")");
  console.log("      降级链   " + cppFallback.length + " 行(期望 " + kExpectSlots + ")");
  console.log("      角色表   " + cppRoles.length + " 行,每行 " +
              cppRoles.map(r => r.length).join("/") + "(期望 2 行 × " + kExpectSlots + ")");
  console.log("      状态表   左 " + (cppLeftStates ? cppLeftStates.length : "null") +
              " / 右 " + (cppRightStates ? cppRightStates.length : "null") +
              "(期望 " + kExpectStates + ")");
  console.log("    每行一条的写法见文件头注释;数字对不上就同步本文件的 kExpect*。");
  process.exit(1);
}

// ------------------------------------------------------------
section("每屏的状态集合:JS 镜像 == face_stages.h");
eq(FS_JS.SCREENS.length, 2, "两屏");
// ★ 槽位顺序 ≠ 档位顺序,别再按下标比(2026-09-18 扩到 5 档时踩的):
//   kFaceRoleId 是**按槽位**排的(枚举顺序,两个屏共用一套槽位名),
//   而 JS 的 states 是**按该屏档位从低到高**排的。
//   左屏加了"高转"之后这两套顺序就分叉了(高转的槽位是 5,但它是该屏第 4 档),
//   按下标比会报"角色编号不一致",而其实两边都对 —— 所以**按状态名比**。
const FACE_KEYS = ["Idle", "Cruise", "Sport", "Redline", "Overspeed", "High", "City"];
for (let i = 0; i < 2; i++) {
  const js = FS_JS.SCREENS[i].states.map(s => s.key);
  const cpp = (i === 0) ? cppLeftStates : cppRightStates;
  const who = (i === 0 ? "左屏" : "右屏");
  eq(js.slice().sort().join(","), cpp.slice().sort().join(","), who + "状态集合");
  eq(js.length, 5, who + " 5 个状态");

  // 每个状态的图片角色号:按**名字**在槽位表里找,再和 JS 比
  for (const st of FS_JS.SCREENS[i].states) {
    const slot = FACE_KEYS.indexOf(st.key);
    ok(slot >= 0, who + " 的 " + st.key + " 是个已知槽位名");
    if (slot < 0) continue;
    eq(st.role, cppRoles[i][slot], who + " 的 " + st.key + " 角色号");
  }
  // 该屏用不到的槽位必须是 0(否则会出现"永远显示不出来的图")
  for (let slot = 0; slot < FACE_KEYS.length; slot++) {
    const key = FACE_KEYS[slot];
    if (js.indexOf(key) >= 0) continue;
    eq(cppRoles[i][slot], 0, who + " 不该有的 " + key + " 角色号必须是 0");
  }
}

// 两屏状态集合必须**不一样**(这正是"每屏一套"的意义);相同的话说明有人抄错了
section("两屏状态集合确实不同(左有红区/高转、右有超速/市区)");
{
  const L = FS_JS.SCREENS[0].states.map(s => s.key);
  const R = FS_JS.SCREENS[1].states.map(s => s.key);
  ok(L.indexOf("Redline") >= 0, "左屏有红区");
  ok(L.indexOf("High") >= 0, "左屏有高转");
  ok(L.indexOf("Overspeed") < 0, "左屏没有超速");
  ok(L.indexOf("City") < 0, "左屏没有市区");
  ok(R.indexOf("Overspeed") >= 0, "右屏有超速");
  ok(R.indexOf("City") >= 0, "右屏有市区");
  ok(R.indexOf("Redline") < 0, "右屏没有红区");
  ok(R.indexOf("High") < 0, "右屏没有高转");
  // 所有用到的角色编号必须互不重复(左右也不能撞)
  const all = FS_JS.SCREENS.reduce((a, s) => a.concat(s.states.map(x => x.role)), []);
  eq(new Set(all).size, all.length, "10 个角色编号互不重复");
  eq(all.length, 10, "两屏各 5 张 = 10 张");
}

// ------------------------------------------------------------
section("16 条阶段用例:JS 镜像 == face_stages.h");
eq(FS_JS.STAGES.length, cppStages.length, "条数");
for (let i = 0; i < cppStages.length; i++) {
  const c = cppStages[i], j = FS_JS.STAGES[i];
  const tag = c.group + "/" + c.level;
  eq(j.group, c.group, tag + " 分组");
  eq(j.level, c.level, tag + " 档位");
  eq(j.rpm, c.rpm, tag + " 转速");
  eq(j.speed, c.speed, tag + " 速度");
  eq(j.coolant, c.coolant, tag + " 水温");
  eq(j.left, c.left, tag + " 左屏期望表情");
  eq(j.right, c.right, tag + " 右屏期望表情");
}

section("分组结构");
for (const g of ["rpm", "speed", "coolant", "intake"]) {
  const rows = FS_JS.stagesOf(g);
  ok(rows.length >= 3, g + " 组至少 3 档");
  eq(rows[0].level, "low", g + " 第一档是 low");
}
eq(FS_JS.stagesOf("rpm").length, 5, "转速组 5 档(怠速/巡航/运动/高转/红区)");
eq(FS_JS.stagesOf("speed").length, 5, "车速组 5 档(静止/市区/快速路/高速/超速)");
eq(FS_JS.stagesOf("coolant").length, 3, "水温组 3 档");
// ★ 进气温度那一组是用户当场抓出来的漏项:"阶段里面缺失了进气温度低中高的选项"。
//   弧和读数加了、阶段表没加,结果是**模拟器里点不出来**那条弧 —— 表里没有它。
eq(FS_JS.stagesOf("intake").length, 3, "进气温度组 3 档");
{
  const vals = FS_JS.stagesOf("intake").map(s => s.intake);
  eq(vals.join(","), "20,40,65", "进气三档的取值(环境温度/常温行驶/堵车热浸)");
  // 三档必须真的落在默认量程 0..80 内,而且彼此拉开 ——
  // 否则格子上那条弧看起来"点了没反应"
  ok(vals[0] >= 0 && vals[2] <= 80, "进气三档都落在默认量程 0..80 内");
  ok(vals[1] - vals[0] >= 10 && vals[2] - vals[1] >= 10, "进气三档之间至少差 10℃");
}
// ★ 每屏 4 个状态 → 表里就该有 4 条:每个状态都能被"点"出来。
//   (当年右屏第 4 档是"急加速惊喜",它不是按车速分档的,所以表里没有它 ——
//    用户试用时问"这个档选不出来是做什么用的",改成超速后就补齐了。)
eq(FS_JS.stagesOf("speed").length, FS_JS.SCREENS[1].states.length,
   "车速组的档数 == 右屏状态数(每个状态都点得出来)");
eq(FS_JS.stagesOf("rpm").length, FS_JS.SCREENS[0].states.length,
   "转速组的档数 == 左屏状态数");
{
  const over = FS_JS.stagesOf("speed").filter(s => s.level === "over")[0];
  ok(!!over, "车速组有超速那一档");
  ok(over.speed > 130, "超速档的车速 " + over.speed + " 要真的超过阈值 130");
  eq(over.right, "Overspeed", "超速档的右屏期望是超速脸");
}
eq(FS_JS.group("coolant").faces, false, "水温组声明为不影响表情");
eq(FS_JS.group("intake").faces, false, "进气温度组声明为不影响表情");

// ------------------------------------------------------------
// 量程上限(画弧进度用):网页镜像 == vehicle_state.h
// 不一致的表现是"预览里弧的进度和真车不一样" —— 同样不会报错。
section("量程上限:JS 镜像 == vehicle_state.h");
eq(FS_JS.MAX.rpm, cppRpmMax, "转速上限 kRpmMax");
eq(FS_JS.MAX.speed, cppSpeedMax, "车速上限 kSpeedMax");
ok(cppRpmMax > 0 && cppSpeedMax > 0, "解析出了量程上限(排版没被改)");

// 阶段表里的转速值不能超出表盘刻度 —— 超了预览里弧会填满而真车也填满,
// 但数字读数会显示一个表盘上不存在的转速,容易误判"表是不是坏了"
section("阶段值不超出量程");
for (const st of FS_JS.STAGES) {
  ok(st.rpm <= FS_JS.MAX.rpm, st.group + "/" + st.level + " 的转速 " + st.rpm + " 没超上限");
  ok(st.speed <= FS_JS.MAX.speed, st.group + "/" + st.level + " 的车速 " + st.speed + " 没超上限");
}
// 红区那一档必须**是这台车真能到的地方**(否则"红区"这一档在真车上永远看不到)。
// ★ 判据是"落在红区档内 + 不超表盘上限",**不是**"接近表盘上限":
//   表盘 7000 而断油 6300(用户实测),7000 那一段发动机根本到不了 ——
//   红区行取 6200(断油下方)才有现实对应。上面 C 侧那条用例同样这么钉。
{
  const red = FS_JS.stagesOf("rpm").filter(s => s.level === "redline")[0];
  ok(red.rpm > 0 && red.rpm <= FS_JS.MAX.rpm,
     "红区档的转速 " + red.rpm + " 要在表盘上限 " + FS_JS.MAX.rpm + " 之内");
  ok(red.rpm >= 5800 && red.rpm < 6300,
     "红区档的转速 " + red.rpm + " 要落在红区档内(>=5800)、且在断油 6300 之下");
  eq(red.left, "Redline", "红区档的左屏期望是红区脸");
}

// ------------------------------------------------------------
// ★★ 这一轮的核心规则,两端各钉一遍(C 侧见 test_face_stages.cpp)
section("每组只变自己那一维");
for (const st of FS_JS.STAGES) {
  if (st.group === "speed") {
    eq(st.rpm, 900, "车速·" + st.level + " 的转速必须是怠速 900");
  } else if (st.group === "rpm") {
    eq(st.speed, 0, "转速·" + st.level + " 的车速必须是 0");
  }
  if (st.group !== "coolant") {
    eq(st.coolant, 85, st.group + "·" + st.level + " 的水温必须是正常值 85");
  }
  // 进气温度同理:除了进气组自己,别的组都必须把进气钉在常温 35 ——
  // 否则点"水温·高"时右屏那条进气弧也会跟着动,看不出是哪条在变
  if (st.group !== "intake") {
    eq(st.intake, 35, st.group + "·" + st.level + " 的进气温度必须是常温 35");
  } else {
    eq(st.rpm, 900, "进气·" + st.level + " 的转速必须是怠速 900");
    eq(st.speed, 0, "进气·" + st.level + " 的车速必须是 0");
    eq(st.coolant, 85, "进气·" + st.level + " 的水温必须是正常值 85");
  }
}

section("每屏只被自己那一路驱动");
for (const st of FS_JS.STAGES) {
  const tag = st.group + "/" + st.level;
  if (st.group === "rpm") {
    eq(st.right, "Idle", tag + " 不该动右屏(速度表)");
  } else if (st.group === "speed") {
    eq(st.left, "Idle", tag + " 不该动左屏(转速表)");
  } else {
    // 温度组(水温 / 进气温度):两屏都不参与表情,只动各自的副弧与数字
    eq(st.left, "Idle", tag + " 不该动左屏(温度不参与表情)");
    eq(st.right, "Idle", tag + " 不该动右屏(温度不参与表情)");
  }
  // 期望的表情必须是那一屏真的有的状态
  ok(FS_JS.roleFor("left", st.left) !== null, tag + " 左屏期望 " + st.left + " 是左屏的状态");
  ok(FS_JS.roleFor("right", st.right) !== null, tag + " 右屏期望 " + st.right + " 是右屏的状态");
}

section("被驱动的那一屏各档必须给出不同表情");
for (const spec of [{ g: "rpm", side: "left" }, { g: "speed", side: "right" }]) {
  const faces = FS_JS.stagesOf(spec.g).map(s => FS_JS.faceOf(s, spec.side));
  eq(new Set(faces).size, faces.length, spec.g + " 组各档表情不重复(" + faces.join(",") + ")");
}

// ------------------------------------------------------------
section("降级链:JS 镜像 == face_stages.h");
{
  // 两屏状态的并集（现在是 7 个:Idle/Cruise/Sport/Redline/Overspeed/High/City），
  // 每个槽位一行链 —— 行数必须等于槽位数。
  const union = new Set();
  FS_JS.SCREENS.forEach(s => s.states.forEach(x => union.add(x.key)));
  eq(union.size, 7, "两屏状态并集是 7 个槽位");
  eq(cppFallback.length, union.size, "链的行数 = 状态并集大小");
  for (let i = 0; i < cppFallback.length; ++i) {
    for (const name of cppFallback[i]) {
      ok(union.has(name), "第 " + i + " 行链里的 " + name + " 是个真实状态名");
    }
  }
  // ★ 按**状态名**逐行对账(不能按下标:见上面槽位顺序 ≠ 档位顺序那段说明)
  for (const key of union) {
    const cppRow = cppFallback[FACE_KEYS.indexOf(key)];
    ok(!!cppRow, key + " 在 face_stages.h 里有降级链");
    if (!cppRow) continue;
    eq((FS_JS.FALLBACK[key] || []).join(","), cppRow.join(","),
       "状态 " + key + " 的降级链");
  }
}

section("角色编号:JS 镜像 == face_stages.h == image-blob-build.js");
for (let side = 0; side < 2; side++) {
  for (const st of FS_JS.SCREENS[side].states) {
    const js = st.role;
    const slot = FACE_KEYS.indexOf(st.key);
    eq(js, cppRoles[side][slot], "第 " + side + " 屏 " + st.key + " 角色号(face_stages.h)");
    // 右屏的角色名带 R 后缀(与 image_blob.h 的命名一致)
    eq(js, ImageBlob.ROLE["Face" + st.key + (side === 1 ? "R" : "")],
       "第 " + side + " 屏 " + st.key + " 角色号(image-blob-build.js)");
  }
}

// ------------------------------------------------------------
section("保留编号没有被复用");
{
  const used = FS_JS.SCREENS.reduce((a, s) => a.concat(s.states.map(x => x.role)), []);
  // 2=开机帧、5=左屏惊喜、7=右屏红区、9/10=开机图、11/16=眨眼图、14/15/19/20=冷车/过热
  for (const reserved of [2, 5, 7, 9, 10, 11, 14, 15, 16, 19, 20]) {
    ok(used.indexOf(reserved) === -1, reserved + " 没被当成在用角色");
    ok(ImageBlob.ROLE_NAMES[reserved] === undefined, reserved + " 在打包器里没有名字");
  }
}

// ------------------------------------------------------------
section("resolve():缺图时的替代品符合固件规则");
{
  const only = (...ids) => (id) => ids.indexOf(id) >= 0;

  eq(FS_JS.resolve("left", "Idle", only()), null, "一张都没有 → null");

  // 只导入左屏常态:左屏所有状态都应落到它
  const justLeftIdle = only(ImageBlob.ROLE.FaceIdle);
  for (const st of FS_JS.SCREENS[0].states) {
    eq(FS_JS.resolve("left", st.key, justLeftIdle), ImageBlob.ROLE.FaceIdle,
       "只有左屏常态图时 " + st.key + " → 常态");
  }
  eq(FS_JS.resolve("right", "Idle", justLeftIdle), null, "只导入了左屏 → 右屏仍然无图");

  // 有红区图:运动缺图退红区
  const red = only(ImageBlob.ROLE.FaceIdle, ImageBlob.ROLE.FaceRedline);
  eq(FS_JS.resolve("left", "Sport", red), ImageBlob.ROLE.FaceRedline, "运动缺图 → 红区");

  // 右屏的超速缺图:退到运动
  const rSport = only(ImageBlob.ROLE.FaceIdleR, ImageBlob.ROLE.FaceSportR);
  eq(FS_JS.resolve("right", "Overspeed", rSport), ImageBlob.ROLE.FaceSportR, "超速缺图 → 运动");

  // 兜底:只导入"左屏红区"一张时,别的状态也用它(有图就用)
  const redOnly = only(ImageBlob.ROLE.FaceRedline);
  eq(FS_JS.resolve("left", "Idle", redOnly), ImageBlob.ROLE.FaceRedline, "只有红区图时 常态 → 兜底用红区");
}

// ------------------------------------------------------------
console.log("\n" + "=".repeat(56));
if (fail === 0) {
  console.log("全部通过:" + pass + " 项断言");
} else {
  console.log("失败 " + fail + " 项 / 共 " + (pass + fail) + " 项:");
  failures.forEach(f => console.log("  - " + f));
}
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
