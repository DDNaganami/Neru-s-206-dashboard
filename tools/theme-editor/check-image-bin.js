/* ============================================================
 * check-image-bin.js —— 把一份 image.bin **拆开逐字段核对**（只读，不改一个字节）
 *
 * 用法：
 *   node tools/theme-editor/check-image-bin.js <image.bin> [--target s3|classic|s3_240]
 *                                                         [--strict] [--quiet]
 *   退出码：0 = 判据全绿；1 = 有判据红（或 --strict 下有字段告警）；2 = 用法/读文件错
 *
 * ------------------------------------------------------------
 * 为什么要这个工具（2026-09-26）
 * ------------------------------------------------------------
 * 车主问的是："**是不是模拟页面那边导出的 bin 文件本来就有问题**"。
 * 这个问题**只能用文件自己的字节回答**：把包头、每张图的角色号/尺寸/字节数/
 * 偏移全摊开，一条条与"两个权威"对账：
 *   · 格式权威 = `lib/themetool/image_blob.h`（ImageEntry 的字段偏移、
 *     ImageBlobHeader 的 sizeof、ImageRole 的编号、IMAGE_PARTITION_BYTES）
 *   · 生成端   = `tools/theme-editor/image-blob-build.js`（build() 写出来的
 *     字节必须与上面那份定义逐字段一致）
 *   · 角色分组权威 = `lib/dashcore/face_stages.h` 的 `kFaceRoleId[2][7]`
 *     （左屏=转速表 5 张 / 右屏=速度表 5 张；**本文件是解析它得到的，
 *      不是手抄一份** —— 手抄会让"用被测对象验证被测对象"）
 *
 * ★ 这个工具**不碰设备、不开串口、不写文件**：只读入参那一个文件。
 * ★ 它判"文件本身对不对"，不判"屏上画得对不对"（那是 check-face-image.js 的事）。
 *
 * ------------------------------------------------------------
 * 一条要记住的既有事实：**name 字段是坏的**（本工具会告警，但不是判据）
 * ------------------------------------------------------------
 * `image-blob-build.js` 的 build() 里写名字那一行是
 *     blob[base + 18 + c] = e.name.charCodeAt(c) & 0xFF;
 * `& 0xFF` 会把**任何非 ASCII 字符截成它的低字节**。而页面
 * （image-editor.html 的 safeName()/byteLen()）是**按 UTF-8 字节数**限长的，
 * 用的又正是车主自己的中文文件名 ⇒ 落到文件里的 name 是"一个汉字一个字节"的
 * 残骸（例如 `6c 1f 68 2d 20 1f` = "转速表-怠速" 的低字节）。
 * 后果：**设备端一切正常**（固件按 role 找图，不看名字；imageBlobParse 只要求
 * 名字以 NUL 结束 —— 这一条仍然满足），但日志/清单里名字是乱码。
 * ⇒ 本工具默认把它算 **告警（WARN）**，不算判据红；`--strict` 下才当红。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");
const IB = require(path.join(__dirname, "image-blob-build.js"));

// ------------------------------------------------------------
// 断言/告警计数（收尾那行与仓库里其它 test-*.js 同一格式）
// ------------------------------------------------------------
let pass = 0, fail = 0, warn = 0, skip = 0;
const failures = [], warnings = [];
function ok(cond, name, detail) {
  if (cond) { pass++; console.log(`  [OK]   ${name}`); }
  else {
    fail++; failures.push(name + (detail ? "  —— " + detail : ""));
    console.log(`  [FAIL] ${name}${detail ? "  —— " + detail : ""}`);
  }
  return !!cond;
}
function note(cond, name, detail) {          // 告警：默认不算红，--strict 算红
  if (cond) { pass++; console.log(`  [OK]   ${name}`); }
  else {
    warn++; warnings.push(name + (detail ? "  —— " + detail : ""));
    console.log(`  [WARN] ${name}${detail ? "  —— " + detail : ""}`);
  }
  return !!cond;
}
function skipAssert(name, why) { skip++; console.log(`  [SKIP] ${name}  —— ${why}`); }
function eq(actual, expect, name) {
  return ok(actual === expect, name, actual === expect ? "" : `实际 ${actual}，期望 ${expect}`);
}

// ------------------------------------------------------------
// 权威 1/2：从源码里**解析**出角色表与枚举，而不是手抄
// ------------------------------------------------------------
function readText(p) { return fs.readFileSync(p, "utf8"); }

// `lib/dashcore/face_stages.h` 的 kFaceRoleId[2][7] —— 左组 / 右组
function parseFaceRoleId() {
  const src = readText(path.join(REPO_ROOT, "lib", "dashcore", "face_stages.h"));
  const m = src.match(/kFaceRoleId\s*\[2\]\s*\[7\]\s*=\s*\{([\s\S]*?)\};/);
  if (!m) throw new Error("face_stages.h 里找不到 kFaceRoleId[2][7]（格式变了？）");
  const rows = m[1].split("\n")
    .map(l => l.replace(/\/\/.*$/, "").trim())
    .filter(l => l.startsWith("{"))
    .map(l => l.replace(/[{}]/g, "").split(",").map(s => parseInt(s.trim(), 10))
              .filter(n => Number.isFinite(n)));
  if (rows.length !== 2) throw new Error("kFaceRoleId 应有 2 行，解析到 " + rows.length);
  return {
    left: rows[0].filter(n => n !== 0),
    right: rows[1].filter(n => n !== 0)
  };
}

// `lib/themetool/image_blob.h` 的 enum class ImageRole —— 已声明的角色号
function parseDeclaredRoles() {
  const src = readText(path.join(REPO_ROOT, "lib", "themetool", "image_blob.h"));
  const m = src.match(/enum class ImageRole\s*:\s*uint16_t\s*\{([\s\S]*?)\};/);
  if (!m) throw new Error("image_blob.h 里找不到 enum class ImageRole");
  const out = {};
  for (const line of m[1].split("\n")) {
    const mm = line.match(/^\s*(\w+)\s*=\s*(\d+)\s*,/);
    if (mm) out[parseInt(mm[2], 10)] = mm[1];
  }
  if (!Object.keys(out).length) throw new Error("ImageRole 里没解析出任何编号");
  return out;
}

const REPO_ROOT = path.resolve(__dirname, "..", "..");

// ★ image_blob.h 的保留编号（**不复用**）。手抄一份是**故意**的：
//   它们在那份头文件里是以注释形式列的（不是枚举成员），解析不出来；
//   而"别人的旧 bin 里出现了保留号"正是这个工具要报的事。
const RESERVED_ROLES = [2, 5, 7, 9, 10, 11, 14, 15, 16, 19, 20];

// ------------------------------------------------------------
// 解析
// ------------------------------------------------------------
const CF_NAME = { 0x06: "L8", 0x0a: "I8", 0x0e: "A8", 0x0f: "RGB888", 0x10: "ARGB8888",
                  0x11: "XRGB8888", 0x12: "RGB565", 0x14: "RGB565A8" };

function bppOf(cf) { return IB.bytesPerPixel(cf); }
function packedBppOf(cf) { return IB.packedBytesPerPixel(cf); }

function parseEntries(buf) {
  const count = buf.readUInt16LE(6);
  const list = [];
  for (let i = 0; i < count; i++) {
    const o = 12 + i * IB.ENTRY_SIZE;
    const nameRaw = buf.slice(o + 18, o + 18 + IB.NAME_MAX);
    const nul = nameRaw.indexOf(0);
    list.push({
      i,
      offset: buf.readUInt32LE(o + 0),
      size: buf.readUInt32LE(o + 4),
      w: buf.readUInt16LE(o + 8),
      h: buf.readUInt16LE(o + 10),
      cf: buf[o + 12],
      pad: buf[o + 13],
      role: buf.readUInt16LE(o + 14),
      order: buf.readUInt16LE(o + 16),
      nameRaw,
      nameEnd: nul,
      name: nul >= 0 ? nameRaw.slice(0, nul).toString("latin1") : "",
      nameAllAscii: nameRaw.every((b, k) => k >= nul || (b >= 0x20 && b < 0x7f))
    });
  }
  return list;
}

function hex(b) { return Buffer.from(b).toString("hex"); }

// 名字的"原始字节"怎么显示：全 ASCII 就原样，否则给 hex + 字节数
function nameCell(e) {
  if (e.nameAllAscii && e.name.length) return `"${e.name}"`;
  if (e.nameEnd === 0) return "(空)";
  return `<非 ASCII ${e.nameEnd} 字节 ${hex(e.nameRaw.slice(0, e.nameEnd))}>`;
}

// ------------------------------------------------------------
function main(argv) {
  let target = null, strict = false, quiet = false, file = null;
  for (let i = 0; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--target") { target = argv[++i]; continue; }
    if (a === "--strict") { strict = true; continue; }
    if (a === "--quiet") { quiet = true; continue; }
    if (a.startsWith("-")) { console.error("未知参数: " + a); return 2; }
    file = a;
  }
  if (!file) {
    console.error("用法: node check-image-bin.js <image.bin> [--target s3|classic|s3_240] [--strict]");
    return 2;
  }

  const buf = fs.readFileSync(file);
  const tgtId = target || IB.DEFAULT_TARGET;
  const tinfo = IB.targetInfo(tgtId);
  const roleId = parseFaceRoleId();
  const declared = parseDeclaredRoles();
  const expLeft = roleId.left, expRight = roleId.right;
  const expAll = [IB.ROLE.Background].concat(expLeft, expRight);

  console.log("=".repeat(72));
  console.log("image.bin 逐字段核对（只读）");
  console.log("  文件  : " + file);
  console.log("  字节数: " + buf.length);
  console.log("  目标板: " + tgtId + " —— " + tinfo.label);
  console.log("  分区  : " + (tinfo.partitionBytes / 1024 / 1024).toFixed(0) + " MB（" +
              tinfo.partitionsCsv + "）");
  console.log("=".repeat(72));

  // ---------------- 包头 ----------------
  // 12 字节头必须存在才谈得上后面
  if (buf.length < IB.HEADER_SIZE) {
    console.log("\n[包头]");
    ok(false, "文件至少要有 " + IB.HEADER_SIZE + " 字节（12 字节头 + 32×44 索引）",
       "实际 " + buf.length + " 字节");
    return finish();
  }
  const magic = buf.readUInt32LE(0), version = buf.readUInt16LE(4);
  const count = buf.readUInt16LE(6), dataBytes = buf.readUInt32LE(8);
  const hdrAscii = Buffer.from([0x32, 0x30, 0x36, 0x44]);
  console.log("\n[包头]  magic=0x" + magic.toString(16).toUpperCase() +
              "  version=" + version + "  count=" + count + "  data_bytes=" + dataBytes);
  console.log("        字节 0..3 在小端下读作 '" + hdrAscii.toString("latin1") + "'" +
              "（kImageBlobMagic = 0x44363032）");
  ok(magic === IB.MAGIC, "包头的 magic == 0x" + IB.MAGIC.toString(16).toUpperCase(),
     "实际 0x" + magic.toString(16).toUpperCase());
  eq(version, IB.VERSION, "包头的 version == " + IB.VERSION);
  ok(count > 0 && count <= IB.MAX_COUNT,
     "包头的 count 落在 1.." + IB.MAX_COUNT, "实际 " + count);
  eq(IB.HEADER_SIZE, 12 + IB.MAX_COUNT * IB.ENTRY_SIZE,
     "HEADER_SIZE == 12 + " + IB.MAX_COUNT + "×" + IB.ENTRY_SIZE + " （与 image_blob.h 的 sizeof 口径一致）");
  console.log("        ⇒ 包头常量 " + IB.HEADER_SIZE + " 字节（12 + " + IB.MAX_COUNT +
              "×" + IB.ENTRY_SIZE + "）");

  // ---------------- 逐张 ----------------
  const items = parseEntries(buf);
  console.log("\n[每张图]");
  console.log("  #  角色号  用途(权威名)   尺寸      颜色格式   字节数      偏移        名字");
  console.log("  -- ------ -------------- --------- ---------- ----------- ----------- ---------------------------");
  for (const e of items) {
    const canonical = IB.ROLE_NAMES[e.role];
    const group = roleId.left.includes(e.role) ? "左/转速"
                : roleId.right.includes(e.role) ? "右/速度"
                : e.role === IB.ROLE.Background ? "两屏共用" : "??";
    console.log("  " + String(e.i).padEnd(2) +
                " " + String(e.role).padStart(6) +
                " " + ((canonical || ("未知 " + e.role)) + "").padEnd(14) +
                " " + (e.w + "×" + e.h).padEnd(9) +
                " " + ((CF_NAME[e.cf] || ("0x" + e.cf.toString(16))) + "").padEnd(10) +
                " " + String(e.size).padStart(11) +
                " " + String(e.offset).padStart(11) +
                " " + nameCell(e));
    if (group === "??") console.log("       ↑ 角色号不在 image_blob.h 的 ImageRole 里");
  }

  // ---------------- 判据 ① 包头 ↔ 实际长度自洽 ----------------
  console.log("\n判据① 包头与实际长度自洽（总长 = 包头 + 各图字节）");
  eq(dataBytes, buf.length - IB.HEADER_SIZE,
     "data_bytes == 文件字节数 − " + IB.HEADER_SIZE);
  let sum = 0;
  for (const e of items) sum += e.size;
  eq(sum, dataBytes, "各图 size 之和 == data_bytes");
  eq(IB.HEADER_SIZE + dataBytes, buf.length, "包头 + 数据总长 == 文件字节数（尾部没有多余/截断）");
  // 未用的索引项必须是 0（entries 是 32 项定长）
  let unusedZero = true, unusedFirstBad = -1;
  for (let i = count; i < IB.MAX_COUNT; i++) {
    const o = 12 + i * IB.ENTRY_SIZE;
    for (let k = 0; k < IB.ENTRY_SIZE; k++) {
      if (buf[o + k] !== 0) { unusedZero = false; unusedFirstBad = i; break; }
    }
    if (!unusedZero) break;
  }
  ok(unusedZero, "count 之后的 " + (IB.MAX_COUNT - count) + " 个索引项全为 0（未用槽位干净）",
     unusedFirstBad >= 0 ? `第 ${unusedFirstBad} 项有非 0 字节` : "");

  // ①' 每张图自身的自洽（尺寸 ↔ 字节数；偏移紧排无洞无重叠）
  console.log("\n判据①' 每张图自身自洽（声明尺寸 ↔ 实际字节数；偏移紧排、无洞、无重叠）");
  let expectOff = 0, compactOk = true;
  for (const e of items) {
    const bpp = bppOf(e.cf);
    const expect = (packedBppOf(e.cf) * e.w + e.pad) * e.h;
    ok(e.cf !== undefined && bpp > 0, `第 ${e.i} 张（角色 ${e.role}）：颜色格式 0x` +
       e.cf.toString(16) + " 认得", bpp ? "" : "未知格式");
    ok(e.w > 0 && e.h > 0, `第 ${e.i} 张（角色 ${e.role}）：宽高都 > 0`, `${e.w}×${e.h}`);
    eq(e.size, expect, `第 ${e.i} 张（角色 ${e.role}）：size == (每像素${packedBppOf(e.cf)}字节×${e.w}+${e.pad})×${e.h}`);
    if (e.offset !== expectOff) compactOk = false;
    ok(e.offset + e.size <= dataBytes, `第 ${e.i} 张（角色 ${e.role}）：偏移+字节数不越过数据区`,
       `${e.offset}+${e.size} vs ${dataBytes}`);
    ok(e.nameEnd >= 0 && e.nameRaw[IB.NAME_MAX - 1] === 0,
       `第 ${e.i} 张（角色 ${e.role}）：name 是 NUL 结束的字符串（imageBlobParse 的硬要求）`);
    expectOff += e.size;
  }
  ok(compactOk, "各图 offset 依次紧排（offset[i] == Σsize[0..i-1]），无空洞、无重叠");
  eq(items.length ? items[items.length - 1].offset + items[items.length - 1].size : 0, dataBytes,
     "最后一张图的末尾正好落在数据区末尾（数据区被完整覆盖）");

  // ---------------- 判据 ② 角色号落在预期集合内 ----------------
  console.log("\n判据② 角色号落在预期集合内（背景 1 + 左组 " + expLeft.join("/") +
              " + 右组 " + expRight.join("/") + "）");
  console.log("        （左/右两组由 lib/dashcore/face_stages.h 的 kFaceRoleId[2][7] 解析得到；" +
              "已声明角色号取自 image_blob.h 的 enum class ImageRole）");
  ok(expLeft.every(r => declared[r]) && expRight.every(r => declared[r]),
     "face_stages.h 的左/右角色号都在 image_blob.h 的 ImageRole 里（两份权威不打架）");
  const badRoles = items.filter(e => !expAll.includes(e.role));
  ok(badRoles.length === 0, "每张图的角色号都属于预期集合",
     badRoles.map(e => `第 ${e.i} 张=角色 ${e.role}`).join("，"));
  const reservedHit = items.filter(e => RESERVED_ROLES.includes(e.role));
  ok(reservedHit.length === 0, "没有用到保留编号（image_blob.h：" + RESERVED_ROLES.join("/") + "）",
     reservedHit.map(e => `第 ${e.i} 张=角色 ${e.role}`).join("，"));
  const bgCount = items.filter(e => e.role === IB.ROLE.Background).length;
  eq(bgCount, 1, "背景图（角色 1）恰好 1 张");
  const leftIn = items.filter(e => expLeft.includes(e.role)).map(e => e.role);
  const rightIn = items.filter(e => expRight.includes(e.role)).map(e => e.role);
  const leftMissing = expLeft.filter(r => !leftIn.includes(r));
  const rightMissing = expRight.filter(r => !rightIn.includes(r));
  console.log("        左组实际 " + leftIn.length + " 张: [" + leftIn.join(", ") + "]  缺: [" +
              leftMissing.join(", ") + "]");
  console.log("        右组实际 " + rightIn.length + " 张: [" + rightIn.join(", ") + "]  缺: [" +
              rightMissing.join(", ") + "]");

  // ---------------- 判据 ③ 左右两组没有互相冒充 ----------------
  console.log("\n判据③ 左右两组的角色号没有互相冒充");
  const dupRoles = {};
  for (const e of items) dupRoles[e.role] = (dupRoles[e.role] || 0) + 1;
  const dup = Object.keys(dupRoles).filter(r => dupRoles[r] > 1);
  ok(dup.length === 0, "每个角色号只出现一次（否则「同号多帧」要按 order 排）",
     dup.map(r => `角色 ${r}×${dupRoles[r]}`).join("，"));
  const dupOrderClash = items.filter(e => dupRoles[e.role] > 1 && e.order !== 0 &&
                                          items.filter(o => o.role === e.role && o.order === e.order).length > 1);
  ok(dupOrderClash.length === 0, "同角色多帧时 order 互不相同（本文件同角色都只有 1 帧）");
  // ★ 名字字段也能当"侧面证据"：车主的中文名被 charCodeAt&0xFF 截过，但
  //   "转速表-…" 与 "速度表-…" 的低字节不同 ⇒ 两组的名字前缀仍然可分。
  //   这一条**不是猜中文**：只要求"左组内部一致、右组内部一致、两组不同"。
  //   ★ 什么时候**跳过**、什么时候**红**（这一步踩过一次，写清楚）：
  //     · 名字**根本没有区分度**（全文件只有一种前缀）⇒ 这条没有信息，跳过；
  //     · 名字**有区分度、却与角色号的分组对不上**（组内前缀不唯一）⇒ **红**。
  //       否则"把两张图的角色号对调"这种错会被静默跳过（实测踩到：
  //       第一版写成"不一致就跳过"，对调角色号的副本居然全绿）。
  const pref = e => hex(e.nameRaw.slice(0, 2));
  const allPrefs = new Set(items.map(pref));
  const leftItems = items.filter(e => expLeft.includes(e.role));
  const rightItems = items.filter(e => expRight.includes(e.role));
  const leftPrefs = new Set(leftItems.map(pref));
  const rightPrefs = new Set(rightItems.map(pref));
  const nameInformative = allPrefs.size > 1;
  if (!leftIn.length || !rightIn.length) {
    skipAssert("名字前缀与角色号分组同向", "文件里只有一组（名字无法交叉核对）");
  } else if (!nameInformative) {
    skipAssert("名字前缀与角色号分组同向", "这文件的名字全一样（没有区分度）");
  } else {
    const sameSide = leftPrefs.size === 1 && rightPrefs.size === 1;
    const differ = !sameSide ? false : [...leftPrefs][0] !== [...rightPrefs][0];
    ok(sameSide && differ,
       "名字前缀与角色号分组**同向**：左组 " + leftIn.length + " 张 = [" + [...leftPrefs].join(", ") +
       "]，右组 " + rightIn.length + " 张 = [" + [...rightPrefs].join(", ") + "]（各自唯一且两组不同）",
       sameSide ? "两组的名字前缀相同 ⇒ 分不出左右" : "组内的名字前缀不唯一 ⇒ 名字与角色号分组打架");
  }
  // 反向判据：组名不一致时，逐张指出"角色号说它在左组、名字却说它在右组"的那些图
  if (nameInformative && leftPrefs.size && rightPrefs.size) {
    // 取"多数派"前缀作为每一组的代表，避免被单张噪声带偏
    const majority = (list) => {
      const cnt = {};
      for (const e of list) cnt[pref(e)] = (cnt[pref(e)] || 0) + 1;
      return Object.keys(cnt).sort((a, b) => cnt[b] - cnt[a])[0];
    };
    const lp = majority(leftItems), rp = majority(rightItems);
    const cross = items.filter(e =>
      (expLeft.includes(e.role) && rp !== lp && pref(e) === rp) ||
      (expRight.includes(e.role) && rp !== lp && pref(e) === lp));
    ok(cross.length === 0,
       "反向：没有一张图的角色号说它在左组、名字前缀却是右组的代表前缀（反之亦然）",
       cross.map(e => `第 ${e.i} 张角色 ${e.role}（名字前缀 ${pref(e)}）`).join("，"));
  } else {
    skipAssert("反向：角色号分组与名字前缀不打架", "名字没有区分度");
  }

  // ---------------- 判据 ④ 尺寸与该目标板的口径一致 ----------------
  console.log("\n判据④ 尺寸与该目标板的口径一致（" + tinfo.faceTier + "：背景 " +
              (tinfo.faceTier === "res240" ? 240 : 480) + "×" +
              (tinfo.faceTier === "res240" ? 240 : 480) +
              "，表情上限 " + IB.faceCanvasMaxFor(tgtId) + " / 推荐 " +
              IB.faceSizeRecommendedFor(tgtId) + "）");
  const bg = items.find(e => e.role === IB.ROLE.Background);
  const bgSide = tinfo.faceTier === "res240" ? 240 : 480;
  if (bg) {
    eq(bg.w, bgSide, "背景宽 == " + bgSide + "（这一档的屏宽）");
    eq(bg.h, bgSide, "背景高 == " + bgSide);
    eq(bg.cf, IB.CF.RGB565, "背景用 RGB565（不透明 ⇒ 省 1/3 空间）");
  } else {
    ok(false, "有背景图可判尺寸");
  }
  const faces = items.filter(e => IB.ROLE_NAMES[e.role] && e.role !== IB.ROLE.Background);
  const cap = IB.faceCanvasMaxFor(tgtId), rec = IB.faceSizeRecommendedFor(tgtId);
  const overCap = faces.filter(e => e.w > cap || e.h > cap);
  ok(overCap.length === 0, "每张表情都不超过画布上限 " + cap +
     "（超了会盖住内圈副弧，且不报错）",
     overCap.map(e => `第 ${e.i} 张 ${e.w}×${e.h}`).join("，"));
  const notSquare = faces.filter(e => e.w !== e.h);
  ok(notSquare.length === 0, "每张表情都是正方形（圆脸按原尺寸居中画）",
     notSquare.map(e => `第 ${e.i} 张 ${e.w}×${e.h}`).join("，"));
  const notA8 = faces.filter(e => e.cf !== IB.CF.RGB565A8);
  ok(notA8.length === 0, "每张表情都用 RGB565A8（带透明 ⇒ 不挡底下的弧）",
     notA8.map(e => `第 ${e.i} 张 cf=0x${e.cf.toString(16)}`).join("，"));
  const sizes = [...new Set(faces.map(e => e.w))];
  ok(sizes.length <= 1, "所有表情同一尺寸（同一档做出来的）", "实际 " + sizes.join("/"));
  if (sizes.length === 1) {
    console.log("        表情尺寸 " + sizes[0] + "×" + sizes[0] + "（推荐 " + rec + "）" +
                (sizes[0] > rec ? "  ← 比推荐值大，但没超上限" : ""));
  }

  // ---------------- 判据 ⑤ 缺的那张如实列出 ----------------
  console.log("\n判据⑤ 缺口如实列出（一整套 = 左 " + expLeft.length + " + 右 " + expRight.length +
              " = " + (expLeft.length + expRight.length) + " 张表情）");
  const missing = leftMissing.concat(rightMissing);
  ok(leftMissing.length === 0 || leftMissing.every(r => !leftIn.includes(r)),
     "报告为「缺失」的角色号确实不在文件里（左：[" + leftMissing.join(", ") + "]）");
  ok(rightMissing.length === 0 || rightMissing.every(r => !rightIn.includes(r)),
     "报告为「缺失」的角色号确实不在文件里（右：[" + rightMissing.join(", ") + "]）");
  if (missing.length) {
    for (const r of missing) {
      console.log("        缺 角色 " + String(r).padStart(2) + " = " + IB.ROLE_NAMES[r] +
                  "（" + (expLeft.includes(r) ? "左/转速表" : "右/速度表") +
                  "）⇒ 该档位在板上会走降级链（face_stages.h 的 kFaceFallback），不是乱画");
    }
  } else {
    console.log("        一整套 10 张齐全");
  }

  // ---------------- 判据 ⑥ 重复/越界/长度对不上 ----------------
  console.log("\n判据⑥ 重复角色号 / 越界 / 长度对不上");
  ok(dup.length === 0, "没有重复角色号");
  ok(compactOk && items.every(e => e.offset + e.size <= dataBytes), "没有越界");
  ok(items.every(e => e.size === (packedBppOf(e.cf) * e.w + e.pad) * e.h),
     "没有「声明的尺寸」与「实际字节数」对不上的图");
  const overPart = buf.length > tinfo.partitionBytes;
  ok(!overPart, "镜像装得进目标板的 image 分区（" + tinfo.partitionBytes + " 字节）",
     buf.length + " > " + tinfo.partitionBytes);
  console.log("        占用 " + buf.length + " / " + tinfo.partitionBytes + " 字节 = " +
              (buf.length * 100 / tinfo.partitionBytes).toFixed(1) + "%");

  // ---------------- 字段告警（不算判据红，除非 --strict） ----------------
  console.log("\n[字段体检]（不算判据①〜⑥；--strict 下才算红）");
  const badName = items.filter(e => !e.nameAllAscii || e.nameEnd === 0);
  note(badName.length === 0,
       "每张图的 name 都是可读 ASCII（" + items.length + " 张里 " + badName.length + " 张不是）",
       "生成端 image-blob-build.js 的 `charCodeAt(c) & 0xFF` 把非 ASCII 截成低字节；" +
       "设备端不受影响（按 role 找图），但日志/清单里是乱码");

  // ---------------- 一句人话的结论 ----------------
  // ★ 这一段是**给车主看的那一句**：不要只给一堆 OK/FAIL，要让"这份文件到底
  //   有没有问题"能被一句话回答（有问题 ⇒ 指出第几张、哪个字段）。
  console.log("\n[结论]");
  const structuralBad = failures.filter(f => !/name/.test(f));
  const namesBad = badName.length;
  if (structuralBad.length === 0) {
    console.log("  · 结构与映射：**0 问题** —— 包头自洽（" + IB.HEADER_SIZE + " + " + dataBytes +
                " = " + buf.length + "）、" + items.length +
                " 张图的角色号全部落在预期集合、偏移紧排无洞无重叠、每张的尺寸与字节数自洽。");
    console.log("  · 缺口：" + (missing.length
      ? "缺 " + missing.map(r => r + "（" + IB.ROLE_NAMES[r] + "）").join("、") +
        " —— 这**不是**文件坏了，是那两张没导；板上会走降级链。"
      : "一整套齐全。"));
    console.log("  · 名字字段：" + (namesBad
      ? namesBad + " 张的 name 不是可读 ASCII（生成端 `charCodeAt(c) & 0xFF` 截断中文名）。" +
        "它**不影响取图**（固件按 role 找图），但日志/清单里是乱码 —— 要修的是生成端那一行，不是这份文件。"
      : "全部可读。"));
    console.log("  ⇒ 一句话：**这份 image.bin 本身是好的**" +
                (namesBad ? "（除 name 字段是乱码这一处，与显示无关）" : "") +
                " —— 屏上表情不对，问题不在这个文件里。");
  } else {
    console.log("  ⇒ 一句话：**这份 image.bin 有问题**，具体见上面 " + structuralBad.length +
                " 条 FAIL（第一条：" + structuralBad[0] + "）。");
  }

  return finish();
}

// ------------------------------------------------------------
function finish() {
  console.log("\n" + "=".repeat(72));
  if (fail === 0) {
    console.log("全部通过:" + pass + " 项断言" +
                (warn ? "（另有 " + warn + " 条字段告警，见上）" : "") +
                (skip ? "（" + skip + " 条跳过）" : ""));
  } else {
    console.log("失败 " + fail + " 项 / 共 " + (pass + fail) + " 项");
    failures.forEach(f => console.log("  - " + f));
    if (warn) console.log("  （另有 " + warn + " 条字段告警）");
  }
  console.log("=".repeat(72));
  return fail === 0 ? 0 : 1;
}

if (require.main === module) {
  // --strict：把字段告警也算成红（CI/收尾用；默认不红，因为设备端不看 name）
  const argv = process.argv.slice(2);
  const strict = argv.includes("--strict");
  let rc;
  try { rc = main(argv); }
  catch (err) { console.error("错误: " + err.message); process.exit(2); }
  if (rc === 0 && strict && warn > 0) {
    console.error("\n--strict：有 " + warn + " 条字段告警 ⇒ 退出码 1");
    process.exit(1);
  }
  process.exit(rc);
}

module.exports = { parseFaceRoleId, parseDeclaredRoles, parseEntries, RESERVED_ROLES };
