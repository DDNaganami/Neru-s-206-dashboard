/* ============================================================
 * test-face-stages.js —— 网页端阶段表与固件表的**对账**(Node 运行)
 *
 *   node tools/theme-editor/test-face-stages.js
 *
 * 为什么需要它:
 *   表情导入页的"阶段模拟"直接读 face-stages.js,然后告诉用户
 *   "低转速 = 常态脸 / 高转速 = 红区脸 / 水温高 = 过热脸"。
 *   这是用户刷图之前**唯一能看到的证据**。如果网页那份和固件那份
 *   不一样,预览就是在骗人,而真车上的表现要到刷完图才知道。
 *
 * 做法:直接**解析 lib/dashcore/face_stages.h 的源码文本**(那三张表),
 * 与 JS 镜像逐字段比对。不搞代码生成 —— 表很小,解析比生成好维护。
 *
 * 链路闭环(每一环都由某条测试钉住):
 *   face_stages.h ──(本文件)──> face-stages.js ──> 表情导入页
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

// ------------------------------------------------------------
// 解析 face_stages.h
// ------------------------------------------------------------

// kFaceStages 行:{"group", "level", 转速, 速度, 水温, Face::状态},
const stagesRe =
  /\{\s*"(\w+)"\s*,\s*"(\w+)"\s*,\s*([\d.]+)f\s*,\s*([\d.]+)f\s*,\s*([\d.]+)f\s*,\s*Face::(\w+)\s*\}/g;
const cppStages = [];
let m;
while ((m = stagesRe.exec(stagesH)) !== null) {
  cppStages.push({
    group: m[1], level: m[2],
    rpm: Number(m[3]), speed: Number(m[4]), coolant: Number(m[5]),
    face: m[6]
  });
}

// kFaceFallback 行:{FACE_SLOT(A), FACE_SLOT(B), FACE_SLOT(C), FACE_SLOT(D)},
const fallbackRe = /\{\s*FACE_SLOT\((\w+)\)\s*,\s*FACE_SLOT\((\w+)\)\s*,\s*FACE_SLOT\((\w+)\)\s*,\s*FACE_SLOT\((\w+)\)\s*\}/g;
const cppFallback = [];
while ((m = fallbackRe.exec(stagesH)) !== null) {
  cppFallback.push([m[1], m[2], m[3], m[4]]);
}

// kFaceRoleId 行:{3, 11, 12, 13, 4, 5, 14, 15},
const roleRe = /kFaceRoleId\[2\]\[8\]\s*=\s*\{([\s\S]*?)\};/;
const roleBlock = roleRe.exec(stagesH);
const cppRoles = [];
if (roleBlock) {
  const rowRe = /\{([\d,\s]+)\}/g;
  let rm;
  while ((rm = rowRe.exec(roleBlock[1])) !== null) {
    cppRoles.push(rm[1].split(",").map(s => Number(s.trim())).filter(n => !isNaN(n)));
  }
}

// 解析不出来就说明 face_stages.h 的排版被改了 —— 直接失败,别静默跳过
section("face_stages.h 可解析");
ok(cppStages.length === 9, "解析出 9 条阶段用例(得到 " + cppStages.length + ")");
ok(cppFallback.length === 8, "解析出 8 行降级链(得到 " + cppFallback.length + ")");
ok(cppRoles.length === 2 && cppRoles[0].length === 8 && cppRoles[1].length === 8,
   "解析出 2×8 的角色编号表");
if (cppStages.length !== 9 || cppFallback.length !== 8 || cppRoles.length !== 2) {
  console.log("\n  ⚠ face_stages.h 里的表格排版被改动了。");
  console.log("    那三张表的书写格式是被本测试解析的:每行一条,");
  console.log("    用 {\"...\", \"...\", 1.0f, 2.0f, 3.0f, Face::X}, 这种写法。");
  console.log("    改回原排版,或同步改本文件的解析正则。");
  process.exit(1);
}

// ------------------------------------------------------------
section("9 条阶段用例:JS 镜像 == face_stages.h");
eq(FS_JS.STAGES.length, cppStages.length, "条数");
for (let i = 0; i < cppStages.length; i++) {
  const c = cppStages[i], j = FS_JS.STAGES[i];
  const tag = c.group + "/" + c.level;
  eq(j.group, c.group, tag + " 分组");
  eq(j.level, c.level, tag + " 档位");
  eq(j.rpm, c.rpm, tag + " 转速");
  eq(j.speed, c.speed, tag + " 速度");
  eq(j.coolant, c.coolant, tag + " 水温");
  eq(j.face, c.face, tag + " 期望表情");
}

// 三组必须各三档,而且档位名是 low/mid/high(界面按这个取中文)
section("分组结构");
for (const g of ["rpm", "coolant", "speed"]) {
  const rows = FS_JS.stagesOf(g);
  eq(rows.length, 3, g + " 组有 3 档");
  eq(rows.map(r => r.level).join(","), "low,mid,high", g + " 档位顺序");
}

// ------------------------------------------------------------
section("降级链:JS 镜像 == face_stages.h");
eq(FS_JS.FACES.length, cppFallback.length, "链的条数 = 表情状态数");
for (let i = 0; i < cppFallback.length; i++) {
  const key = FS_JS.FACES[i].key;
  eq((FS_JS.FALLBACK[key] || []).join(","), cppFallback[i].join(","),
     "槽位 " + i + "(" + key + ") 的降级链");
}

// ------------------------------------------------------------
section("角色编号:JS 镜像 == face_stages.h == image-blob-build.js");
for (let side = 0; side < 2; side++) {
  for (let i = 0; i < FS_JS.FACES.length; i++) {
    const f = FS_JS.FACES[i];
    const js = side === 0 ? f.roleL : f.roleR;
    eq(js, cppRoles[side][i], "第 " + side + " 屏 " + f.key + " 的角色编号(face_stages.h)");
    eq(js, ImageBlob.ROLE["Face" + f.key + (side === 1 ? "R" : "")],
       "第 " + side + " 屏 " + f.key + " 的角色编号(image-blob-build.js)");
  }
}

// 保留编号不能被复用(9/10 曾是开机图)
section("保留编号 9/10 未被复用");
const usedRoles = [];
FS_JS.FACES.forEach(f => { usedRoles.push(f.roleL); usedRoles.push(f.roleR); });
ok(usedRoles.indexOf(9) === -1, "9 未被复用");
ok(usedRoles.indexOf(10) === -1, "10 未被复用");
eq(new Set(usedRoles).size, usedRoles.length, "16 个角色编号互不重复");

// ------------------------------------------------------------
section("resolve():缺图时的替代品符合固件规则");
{
  const only = (...ids) => (id) => ids.indexOf(id) >= 0;

  // 什么都没导入 → null(程序化占位表情接管)
  eq(FS_JS.resolve(0, "Idle", only()), null, "一张都没有 → null");

  // 只导入常态:所有状态都应落到常态
  const justIdle = only(ImageBlob.ROLE.FaceIdle);
  for (const key of ["Idle", "Blink", "Cruise", "Sport", "Redline", "Surprise", "Cold", "Hot"]) {
    eq(FS_JS.resolve(0, key, justIdle), ImageBlob.ROLE.FaceIdle, "只有常态图时 " + key + " → 常态");
  }

  // 有红区图:惊喜退到红区之前先看自己有没有
  const red = only(ImageBlob.ROLE.FaceIdle, ImageBlob.ROLE.FaceRedline);
  eq(FS_JS.resolve(0, "Sport", red), ImageBlob.ROLE.FaceRedline, "运动缺图 → 红区");
  eq(FS_JS.resolve(0, "Surprise", red), ImageBlob.ROLE.FaceRedline, "惊喜缺图 → 红区");

  // 左右屏必须各查各的:右屏的角色不能顶替左屏
  const leftOnly = only(ImageBlob.ROLE.FaceIdle);
  eq(FS_JS.resolve(1, "Idle", leftOnly), null, "只导入了左屏常态 → 右屏仍然无图");

  // 兜底:只导入"过热"一张时,别的状态也用它(有图就用,别去画占位脸)
  const hotOnly = only(ImageBlob.ROLE.FaceHot);
  eq(FS_JS.resolve(0, "Idle", hotOnly), ImageBlob.ROLE.FaceHot, "只有过热图时 常态 → 兜底用过热");
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
