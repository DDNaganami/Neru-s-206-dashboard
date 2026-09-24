/* ============================================================
 * test-asset-spec.js —— 素材规格与转换的用例（Node 运行）
 *
 *   node tools/theme-editor/test-asset-spec.js
 *
 * 为什么值得单独一个文件（车主 2026-09-24 的明确要求）:
 *   "上传端规格没写清、也没有转换"是这一轮要补的洞。而**规格一旦写下来就会
 *   与实现分叉** —— 文档说上限 320、打包器哪天改成 300，谁也不会发现，
 *   直到有人拿 310 的图导进去盖住弧。所以这里做两件事:
 *     ① 把 asset-spec.js 的每一个数与**真正干活的那两个模块**对账
 *        （image-blob-build.js 的画布/分区、固件侧的 ImageRole 编号、
 *         两份分区表的实际字节数）；
 *     ② 转换与拒绝路径逐条测（保持长宽比、裁透明边、alpha 要求、圆屏约束、
 *        不认识的格式、放大 vs 缩小）。
 *
 * ★ 断言里出现的数字都是**推导出来的**（问 ImageBlob / 读分区表），
 *   不写死 —— 只有"这套规格本身"的那几个数（336 / 168 之类）是显式钉住的。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");

const IB = require("./image-blob-build.js");
const AS = require("./asset-spec.js");

let pass = 0, fail = 0;
const failures = [];

function ok(cond, what) {
  if (cond) pass++;
  else { fail++; failures.push(what); console.log("  ✗ " + what); }
}
function eq(a, b, what) {
  ok(a === b, what + "  (期望 " + JSON.stringify(b) + ",得到 " + JSON.stringify(a) + ")");
}
function throws(fn, what) {
  try { fn(); fail++; failures.push(what + "（本该抛错却通过了）"); console.log("  ✗ " + what + "（本该抛错）"); }
  catch (e) { pass++; }
}
function section(t) { console.log("\n== " + t); }

// ------------------------------------------------------------
// 造测试用的 RGBA（纯数据，不依赖 canvas）
// ------------------------------------------------------------
// 中心实心方块 + 四周透明，方块颜色可指定
function boxRgba(w, h, box, rgba4, pad) {
  const d = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      const inside = x >= pad && x < w - pad && y >= pad && y < h - pad;
      if (inside) {
        d[i] = rgba4[0]; d[i + 1] = rgba4[1]; d[i + 2] = rgba4[2]; d[i + 3] = rgba4[3];
      }
    }
  }
  return d;
}

// ============================================================
section("一、规格数字与实现侧对账（分叉就会红）");
{
  // 角色编号：asset-spec 的复述必须与打包器（= 固件那一侧的对账源）逐条一致
  for (const r of AS.ROLES) {
    const ibKey = r.key;
    ok(IB.ROLE[ibKey] !== undefined, "image-blob-build 里有角色 " + ibKey);
    eq(r.role, IB.ROLE[ibKey], "角色号 " + r.id + " 与 image-blob-build 一致");
    eq(AS.roleByNumber(r.role) !== null, true, "编号 " + r.role + " 能反查回来");
    // 名字建议在长度上限之内（名字是 24 字节 C 串 ⇒ 最多 23 字符）
    ok(AS.suggestedName(r.id).length <= AS.OUTPUT.nameMaxChars,
       "建议文件名 " + AS.suggestedName(r.id) + " 不超过 " + AS.OUTPUT.nameMaxChars + " 字符");
  }
  // 保留编号一律不在表里、也不等于下一个可用号
  for (const n of AS.RESERVED_ROLES) {
    eq(AS.roleByNumber(n), null, "保留编号 " + n + " 不在在用表里");
    ok(n !== AS.NEXT_FREE_ROLE, "下一个可用号 " + AS.NEXT_FREE_ROLE + " 不是保留号");
  }
  // ★ 表里必须列满 11 个在用角色（曾经漏掉 21/22 那两档，而漏了只会静默降级）
  eq(AS.ROLES.length, 11, "在用角色 11 个（1 背景 + 两屏各 5）");

  // 分区大小：asset-spec 报的必须等于**读分区表**得到的那个数
  for (const t of ["classic", "s3", "s3_240"]) {
    eq(AS.budget(t).partitionBytes, IB.partitionBytesFor(t),
       t + " 的分区大小与 image-blob-build 一致");
  }

  // 画布上限 / 推荐 / 内圈副弧半径：与 FACE_SIZE_TIERS 一致（那是读 theme-default 算的）
  for (const t of ["classic", "s3", "s3_240"]) {
    const tier = AS.tierOf(t);
    eq(tier.faceCanvasMax, IB.faceCanvasMaxFor(t), t + " 画布上限一致");
    eq(tier.faceRecommended, IB.faceSizeRecommendedFor(t), t + " 推荐尺寸一致");
    eq(tier.faceInnerMostRadius, IB.arcInnerMostRadiusFor(t), t + " 内圈副弧内沿一致");
  }
  // 480 档的原始设计值（改这两个数等于改整套几何，要有意识）
  eq(AS.tierOf("s3").faceCanvasMax, 320, "480 档画布上限 320");
  eq(AS.tierOf("s3").faceRecommended, 300, "480 档推荐 300");
  eq(AS.tierOf("s3_240").faceCanvasMax, 160, "240 档画布上限 160");
  eq(AS.tierOf("s3_240").faceRecommended, 152, "240 档推荐 152");

  // 产物格式：每像素字节数必须与打包器一致（RGB565A8 = 3，不是 2）
  eq(AS.OUTPUT.bytesPerPixelByRole.background, IB.bytesPerPixel(IB.CF.RGB565), "背景 2 字节/像素");
  eq(AS.OUTPUT.bytesPerPixelByRole.face, IB.packedBytesPerPixel(IB.CF.RGB565A8), "表情 3 字节/像素(含 A8 平面)");
  eq(AS.OUTPUT.cfByRole.background, IB.CF.RGB565, "背景用 RGB565");
  eq(AS.OUTPUT.cfByRole.face, IB.CF.RGB565A8, "表情用 RGB565A8");
  eq(AS.OUTPUT.headerBytes, IB.HEADER_SIZE, "镜像头 1420 字节");
  eq(AS.OUTPUT.entryBytes, IB.ENTRY_SIZE, "索引项 44 字节");
  eq(AS.OUTPUT.maxCount, IB.MAX_COUNT, "最多 32 张");
}

// ============================================================
section("二、圆形可视区约束（480 圆屏 ⇒ 内容落在内切圆内）");
{
  // √2 的账：内切正方形边长 = D / √2
  eq(AS.circleSafeSide(480), 336, "480 圆屏的内切正方形边长（向下对齐到 4）");
  eq(AS.circleSafeSide(240), 168, "240 圆屏的内切正方形边长");
  // 336 的四角必须真的在圆内（半径 240），337 就不该在（这是"取整方向"的证明）
  const d336 = Math.sqrt(336 * 336 + 336 * 336) / 2;
  const d340 = Math.sqrt(340 * 340 + 340 * 340) / 2;
  ok(d336 < 240, "336×336 的四角在半径 240 内（" + d336.toFixed(1) + "）");
  ok(d340 > 240, "340×340 的四角已经在圆外（" + d340.toFixed(1) + "）—— 所以上限取 336 是对的");
  // 表情画布上限本来就在圆内：这是"正常做图不会撞上圆约束"的证明
  ok(AS.tierOf("s3").faceCanvasMax <= AS.circleSafeSide(480),
     "480 档表情上限(320) ≤ 圆内正方形(336)");
  ok(AS.tierOf("s3_240").faceCanvasMax <= AS.circleSafeSide(240),
     "240 档表情上限(160) ≤ 圆内正方形(168)");
  // 背景必须铺满整屏（等于屏分辨率）
  eq(AS.slotBox("background", "s3").max, 480, "480 档背景要 480×480");
  eq(AS.slotBox("background", "s3_240").max, 240, "240 档背景要 240×240");
}

// ============================================================
section("二之二、档位口径：480 = 2.8C（最终板）/ 240 = 历史 DualEye（已退货）");
{
  // ★ 2026-09-24：这两条 label/tier 文案是**给用户看的档位名**，
  //   它们必须说清"哪个是目标板、哪个只是历史档" —— 措辞被改回去就会红。
  ok(/2\.8C/.test(AS.ROUND_PANEL.res480.tier), "480 档点名 2.8C："
     + AS.ROUND_PANEL.res480.tier);
  ok(/最终板/.test(AS.ROUND_PANEL.res480.tier), "480 档写明「最终板」");
  ok(/历史/.test(AS.ROUND_PANEL.res240.tier), "240 档写明「历史」："
     + AS.ROUND_PANEL.res240.tier);
  ok(/退货/.test(AS.ROUND_PANEL.res240.tier), "240 档写明「已退货」");
  ok(/DualEye/.test(AS.ROUND_PANEL.res240.tier), "240 档点名 DualEye（历史那块板）");

  // 目标板 label 也必须带上这些口径（它是下拉框里用户唯一看得见的那行字）
  ok(/2\.8C/.test(IB.TARGETS.s3.label + IB.TARGETS.s3.hint), "s3 目标的 label/hint 里写了 2.8C");
  ok(/退货|历史/.test(IB.TARGETS.s3_240.label + IB.TARGETS.s3_240.hint),
     "s3_240 目标的 label/hint 里写了退货/历史");
  // 480 档标签同样（下拉框里选"分辨率档"时显示的就是 FACE_SIZE_TIERS[].label）
  ok(/2\.8C/.test(IB.FACE_SIZE_TIERS[0].label), "480 分辨率档标签里有 2.8C");
  ok(/历史/.test(IB.FACE_SIZE_TIERS[1].label), "240 分辨率档标签里有「历史」");

  // ---- ★ 2026-09-24 口径变更：**默认目标板 = 2.8C（最终板）= s3** ----
  //   车主原话："默认就改成 2.8C 最终版吧，而且是双 2.8C"。
  //   这条必须钉住：默认板决定页面一打开看到的规格、画布上限与分区大小
  //   （拿经典板的 1MB 口径去做最终板的图，是这一档最容易犯的错）。
  eq(IB.DEFAULT_TARGET, "s3", "默认目标板是 s3（2.8C 最终板），不是 classic");
  eq(IB.targetInfo().id, "s3", "不给 target 时 targetInfo() 给的就是 2.8C 那一档");
  eq(IB.partitionBytesFor(), IB.TARGETS.s3.partitionBytes,
     "不给 target 时分区分母按 2.8C 算（8MB），不是经典板的 1MB");
  eq(AS.tierOf().displayRes, 480, "默认档是 480 屏");

  // ---- ★ 双 2.8C 的口径：两块同型号同分辨率的屏 ----
  //   label 是下拉框里那行字；hint 是选中后紧跟的一行 —— 两处都要说"双"，
  //   并且要把"刷同一份 image.bin / 占用按单块板算"讲清楚
  //   （含糊其辞会让人以为预算要按两块相加）。
  ok(/双\s*2\.8C/.test(IB.TARGETS.s3.label), "s3 的 label 写明**双** 2.8C："
     + IB.TARGETS.s3.label);
  ok(/双\s*2\.8C/.test(IB.TARGETS.s3.hint), "s3 的 hint 也写明双 2.8C");
  ok(/两块/.test(IB.TARGETS.s3.hint), "hint 里点明是**两块**屏");
  ok(/同型号同分辨率/.test(IB.TARGETS.s3.hint), "hint 里点明两块屏同型号同分辨率");
  ok(/左屏/.test(IB.TARGETS.s3.hint) && /转速表/.test(IB.TARGETS.s3.hint),
     "hint 里写清左屏 = 转速表");
  ok(/右屏/.test(IB.TARGETS.s3.hint) && /速度表/.test(IB.TARGETS.s3.hint),
     "hint 里写清右屏 = 速度表");
  ok(/同一份\s*image\.bin/.test(IB.TARGETS.s3.hint),
     "hint 里写清两块板刷**同一份** image.bin");
  ok(/单块板/.test(IB.TARGETS.s3.hint),
     "hint 里写清占用/预算按**单块板**算（不是两块相加）");
  // 左右屏的区别靠**用途**表达 —— 两屏的表情角色本来就是分开的两组，且数量相同
  {
    const left = AS.ROLES.filter(r => r.id === "face_idle" || r.id === "face_cruise" ||
                                      r.id === "face_sport" || r.id === "face_high" ||
                                      r.id === "face_redline");
    const right = AS.ROLES.filter(r => r.id === "face_idle_r" || r.id === "face_city_r" ||
                                       r.id === "face_cruise_r" || r.id === "face_sport_r" ||
                                       r.id === "face_overspeed_r");
    eq(left.length, 5, "左屏(转速表)5 个表情用途");
    eq(right.length, 5, "右屏(速度表)5 个表情用途");
    eq(left.filter(r => right.indexOf(r) >= 0).length, 0,
       "左右屏的用途**不重叠**（两块屏靠用途区分，不是靠「第几块板」）");
  }
  // 预算按单块板：2.8C 那一档的整套必须装得进**一个** 8MB 分区
  ok(AS.budget("s3").fullSetBytes < AS.budget("s3").partitionBytes,
     "整套素材装得进单块 2.8C 板的 image 分区（" + AS.budget("s3").fullSetBytes +
     " < " + AS.budget("s3").partitionBytes + "）");

  // ---- 物理口径（出处：PURCHASE.md 第六节「Ø 有效区」那一列）----
  eq(AS.ROUND_PANEL.res480.activeAreaMm10, 7013, "2.8C 有效区 Ø70.13mm（单位 1e-2 mm）");
  eq(AS.ROUND_PANEL.res240.activeAreaMm10, null,
     "DualEye 那一档**没有**量过有效区 ⇒ 留空，不许编一个数");
  eq(AS.ROUND_PANEL.res480.displayRes, 480, "2.8C 是 480×480");
  eq(AS.ROUND_PANEL.res240.displayRes, 240, "DualEye 是 240×240");

  // 1 像素 = 0.1461 mm（单位 1e-4 mm ⇒ 1461），与 C++ 侧 panelMmPerPx10000 同一个数
  eq(AS.mmPerPixelX10000(AS.ROUND_PANEL.res480), 1461, "1 像素 = 0.1461 mm");
  eq(AS.mmPerPixelX10000(AS.ROUND_PANEL.res240), null,
     "没有有效区数据时给不出换算 ⇒ null（不许瞎算）");

  // tierOf() 要把这块屏的物理口径挂上去（页面用它渲染"屏是圆的"那句）
  eq(AS.tierOf("s3").panel.activeAreaMm10, 7013, "tierOf(s3).panel 带上 Ø70.13mm");
  eq(AS.tierOf("s3_240").panel.activeAreaMm10, null, "tierOf(s3_240).panel 没有那个数");
  eq(AS.tierOf("s3").circleSafe, 336, "tierOf(s3).circleSafe = 336（内切正方形）");
  eq(AS.tierOf("s3_240").circleSafe, 168, "tierOf(s3_240).circleSafe = 168");
}

// ============================================================
section("三、可接受格式与拒绝路径");
{
  eq(AS.formatOf("a.png").id, "png", "PNG 认");
  eq(AS.formatOf("a.PNG").id, "png", "后缀大小写不敏感");
  eq(AS.formatOf("a.jpeg").id, "jpg", ".jpeg 也算 JPEG");
  eq(AS.formatOf("a.webp").id, "webp", "WebP 认");
  eq(AS.formatOf("a.svg"), null, "SVG 不认（矢量要另一套渲染路径）");
  eq(AS.formatOf("a.gif"), null, "GIF 不认（调色板透明缩放会碎）");
  eq(AS.formatOf("a.bmp"), null, "BMP 不认");
  eq(AS.formatOf("noext", "image/png").id, "png", "没有后缀时退回 MIME");

  // 表情 + JPEG = **拒绝**（并且理由要能看懂）
  const r1 = AS.checkUpload("face.jpg", "image/jpeg", "face_idle", "s3");
  eq(r1.ok, false, "JPEG 当表情用要被拒");
  ok(/alpha/.test(r1.errors[0]), "拒绝理由里点名了 alpha：" + r1.errors[0]);
  ok(/PNG/.test(r1.errors[0]), "拒绝理由里给了出路（改用 PNG）");

  // 背景 + JPEG = 可以（背景不需要 alpha）
  eq(AS.checkUpload("bg.jpg", "image/jpeg", "background", "s3").ok, true,
     "JPEG 当背景用可以");

  // PNG 当表情 = 通过（可能有 WebP 之外的其他提醒，但没有 error）
  const r2 = AS.checkUpload("face.png", "image/png", "face_idle", "s3");
  eq(r2.ok, true, "PNG 当表情用通过");
  eq(r2.errors.length, 0, "PNG 没有 error");

  // WebP 当表情 = 通过但有提醒（有损边缘会脏）
  const r3 = AS.checkUpload("face.webp", "image/webp", "face_idle", "s3");
  eq(r3.ok, true, "WebP 当表情用通过（只提醒）");
  ok(r3.warnings.length > 0, "WebP 有一条提醒（有损边缘脏边）");

  // 不认识的格式 = 拒绝，且理由里列出可接受的三种
  const r4 = AS.checkUpload("face.svg", "image/svg+xml", "face_idle", "s3");
  eq(r4.ok, false, "SVG 被拒");
  ok(/PNG/.test(r4.errors[0]) && /JPEG/.test(r4.errors[0]) && /WebP/.test(r4.errors[0]),
     "拒绝理由里列出了可接受格式");
}

// ============================================================
// ★ 三之二、用途的**两种写法必须等价**（2026-09-24 的真实故障，钉死它）
//
//   故障现场：`image-editor.html` 的 addFiles() 把 `ImageBlob.ROLE.Background`
//   （**数字** 1）递给 checkUpload()，而那一侧当时只认**名字** ⇒ 恒判
//   `ok:false, errors:["不认识的用途：1"]` ⇒ **任何图片都进不了列表**，
//   页面上还一个字都不打（理由被 refresh() 刷掉）。车主的原话就是"上传图片没有任何反应"。
//
//   根因不是"页面写错了一行"，而是**两边的编号表没有人钉过**：
//   `ImageBlob.ROLE`（打包器/固件那一侧）与 `asset-spec.js` 的 ROLES
//   （上传端那一侧）各有一份编号，靠人眼对齐。下面这两组断言就是那道闸门：
//     ① 每个 ImageBlob.ROLE 的**数字**都能在 asset-spec 里解析出用途；
//     ② 同一个用途，传**数字**与传**名字**得到的检查结果**逐字节相同**。
//   任何一边改了编号而忘了另一边，这里立刻红。
// ============================================================
section("三之二、用途：数字与名字必须等价（上传端与打包器的编号表对账）");
{
  // ① 每个编号都要能反查出用途 —— 这一条覆盖**所有**角色，不只是背景。
  //    漏掉一个编号，页面里那个用途的图就会被静默拒收（正是这次的症状）。
  for (const key of Object.keys(IB.ROLE)) {
    const num = IB.ROLE[key];
    const r = AS.roleByNumber(num);
    ok(r !== null, "ImageBlob.ROLE." + key + "（编号 " + num + "）能在 asset-spec 里反查出用途");
    if (r) {
      eq(r.role, num, "反查出来的 " + r.id + " 的编号还是 " + num + "（来回一致）");
      eq(r.key, key, "反查出来的 " + r.id + " 的 key 与 ImageBlob.ROLE 的键名一致（" + key + "）");
    }
  }
  // 反向也要成立：asset-spec 表里每个角色，按它的**名字**能查到、按它的**编号**也能查到同一个
  for (const r of AS.ROLES) {
    eq(AS.roleById(r.id), r, "roleById(" + r.id + ") 拿回同一条");
    eq(AS.roleByNumber(r.role), r, "roleByNumber(" + r.role + ") 拿回同一条");
  }

  // ② 同一个用途、同一个文件：传数字与传名字，结果必须**完全一致**。
  //    覆盖"会通过"和"会被拒"两条路 —— 只测通过的那条会漏掉
  //    "两边都拒但理由不同"这种更难查的分叉。
  const cases = [
    // [文件, MIME, 数字用途, 用途名, 目标板]
    ["x.png", "image/png", IB.ROLE.Background, "background", "s3"],
    ["x.png", "image/png", IB.ROLE.FaceIdle, "face_idle", "s3"],
    ["x.png", "image/png", IB.ROLE.FaceOverspeedR, "face_overspeed_r", "s3"],
    ["x.png", "image/png", IB.ROLE.FaceCityR, "face_city_r", "s3_240"],
    // JPEG 当表情 = 两边都该**拒**，而且拒绝理由要一模一样（alpha 那条）
    ["face.jpg", "image/jpeg", IB.ROLE.FaceIdle, "face_idle", "s3"],
    // 认不出的格式 = 两边都该拒（格式那条与用途无关，也要一致）
    ["face.svg", "image/svg+xml", IB.ROLE.FaceIdle, "face_idle", "s3"],
    // WebP 当表情 = 两边都该给同一条警告
    ["face.webp", "image/webp", IB.ROLE.FaceIdle, "face_idle", "s3"]
  ];
  for (const [file, mime, num, name, target] of cases) {
    const byNum = AS.checkUpload(file, mime, num, target);
    const byName = AS.checkUpload(file, mime, name, target);
    eq(JSON.stringify(byNum), JSON.stringify(byName),
       "「" + file + "」当 " + name + "（" + target + "）：传编号 " + num +
       " 与传名字结果一致");
  }

  // ③ 认不出来的**编号**仍然要拒（别把"归一化"写成"什么都放行"）——
  //    保留编号 2 与一个从没出现过的号都必须拒绝，且理由里带上调用方传的那个值。
  for (const bad of [AS.RESERVED_ROLES[0], AS.NEXT_FREE_ROLE, 999]) {
    const r = AS.checkUpload("x.png", "image/png", bad, "s3");
    eq(r.ok, false, "编号 " + bad + " 没有对应用途 ⇒ 要拒");
    ok(r.errors[0].indexOf(String(bad)) >= 0,
       "拒绝理由里带上了调用方传的值 " + bad + "：" + r.errors[0]);
  }
  // 完全没给用途（undefined）也不能被当成合法
  eq(AS.checkUpload("x.png", "image/png", undefined, "s3").ok, false, "不给用途要拒");

  // ④ 认不出的**目标板**要"响亮地失败"，不许静默兜底成 480 档 ——
  //    板名写错就悄悄按另一块板算规格，比直接抛错危险得多
  //    （ImageBlob.targetInfo() 里就是 throw，这里把它钉住）。
  throws(() => AS.checkUpload("x.png", "image/png", IB.ROLE.Background, "no_such_board"),
         "未知目标板要抛错（传编号）");
  throws(() => AS.checkUpload("x.png", "image/png", "background", "no_such_board"),
         "未知目标板要抛错（传名字）");
}

// ============================================================
section("四、转换：保持长宽比、只缩不放、尺寸对齐");
{
  // 800×400 的透明底 + 中间实心块 → 放进 480 档表情框（最大 320）
  const src = boxRgba(800, 400, null, [255, 0, 0, 255], 200);   // 中间 400×400... 用 pad 控制
  const r = AS.prepareAsset(src, 800, 400, { roleId: "face_idle", targetId: "s3" });
  eq(r.errors.length, 0, "800×400 能转（没有 error）");
  eq(r.w, 320, "800×400 → 宽度收到上限 320");
  eq(r.h, 160, "高度按比例 160（保持 2:1）");
  ok(r.w / r.h === 2, "长宽比保持 2:1");
  eq(r.cf, IB.CF.RGB565A8, "表情转成 RGB565A8");
  eq(r.info.bytes, 320 * 160 * 3, "字节数按 3 字节/像素算");

  // **只缩不放**：100×100 的图放进 320 的框，输出还是 100×100
  const small = boxRgba(100, 100, null, [0, 255, 0, 255], 0);
  const r2 = AS.prepareAsset(small, 100, 100, { roleId: "face_idle", targetId: "s3" });
  eq(r2.w, 100, "小图不被放大（宽）");
  eq(r2.h, 100, "小图不被放大（高）");

  // 240 档的框不一样（同一个源图，输出上限跟着屏走）
  const r3 = AS.prepareAsset(src, 800, 400, { roleId: "face_idle", targetId: "s3_240" });
  eq(r3.w, 160, "240 档：800×400 收到 160 宽");
  eq(r3.h, 80, "240 档：高 80");

  // 手工指定更小的上限（页面里"目标宽度"输入框）
  const r4 = AS.prepareAsset(src, 800, 400, { roleId: "face_idle", targetId: "s3", maxSide: 200 });
  eq(r4.w, 200, "手工上限 200 生效");
  eq(r4.h, 100, "手工上限下高度按比例");

  // 背景：不透明角色 —— 有半透明像素时按底色合成，输出一定是不透明的
  const halfAlpha = new Uint8Array(4 * 4 * 4);
  for (let i = 0; i < halfAlpha.length; i += 4) {
    halfAlpha[i] = 255; halfAlpha[i + 3] = 128;      // 半透明红
  }
  const r5 = AS.prepareAsset(halfAlpha, 4, 4, { roleId: "background", targetId: "s3" });
  eq(r5.cf, IB.CF.RGB565, "背景转成 RGB565（没有 alpha）");
  for (let i = 3; i < r5.rgba.length; i += 4) {
    if (r5.rgba[i] !== 255) { ok(false, "背景合成后必须全不透明"); break; }
  }
  pass++;   // 上面的循环没 break 就算过
  ok(r5.warnings.some(w => /半透明/.test(w)), "背景含半透明像素时有提醒");
}

// ============================================================
section("五、转换：裁掉透明边");
{
  // 200×200 的画布，内容只占中间 60×40，四周全透明
  const w = 200, h = 200, pad = 70;
  const src = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      if (x >= pad && x < pad + 60 && y >= 90 && y < 130) {
        src[i] = 0; src[i + 1] = 0; src[i + 2] = 255; src[i + 3] = 255;
      }
    }
  }
  const r = AS.prepareAsset(src, w, h, { roleId: "face_idle", targetId: "s3", trim: true });
  eq(r.errors.length, 0, "裁透明边没有 error");
  eq(r.w, 60, "裁完之后宽 = 内容宽");
  eq(r.h, 40, "裁完之后高 = 内容高");
  ok(r.warnings.some(x => /裁掉透明边/.test(x)), "有「裁掉透明边」的提醒");

  // 不裁的时候尺寸不变（200×200，仍在 320 以内 ⇒ 不缩）
  const r2 = AS.prepareAsset(src, w, h, { roleId: "face_idle", targetId: "s3", trim: false });
  eq(r2.w, 200, "不裁时保持原尺寸");

  // **整张全透明 ⇒ 拒绝**（不是"裁成 0×0"）
  const empty = new Uint8Array(10 * 10 * 4);   // 全 0 = 全透明
  const r3 = AS.prepareAsset(empty, 10, 10, { roleId: "face_idle", targetId: "s3", trim: true });
  ok(r3.errors.length > 0, "整张透明要被拒");
  ok(/透明/.test(r3.errors[0]), "拒绝理由说得清：" + r3.errors[0]);

  // alpha 缺失的警告（全不透明的表情图）
  const opaque = boxRgba(64, 64, null, [10, 20, 30, 255], 0);
  const r4 = AS.prepareAsset(opaque, 64, 64, { roleId: "face_idle", targetId: "s3" });
  eq(r4.errors.length, 0, "全不透明的表情图**不拒绝**（技术合法）");
  ok(r4.warnings.some(x => /alpha/.test(x)), "但要警告：会盖住底下的弧线");
}

// ============================================================
section("六、转换：圆形可视区与尺寸上限的警告");
{
  // 一张 480×480 的**方**内容当表情：超过 320 上限 ⇒ 会被缩到 320（不报错）
  const big = boxRgba(480, 480, null, [255, 255, 0, 255], 0);
  const r = AS.prepareAsset(big, 480, 480, { roleId: "face_idle", targetId: "s3" });
  eq(r.w, 320, "480×480 的表情被收到 320");
  eq(r.h, 320, "高也 320（1:1）");
  // ★ 320×320 的四角在 480 圆屏内（对角线一半 226 < 240）⇒ 不该有圆约束警告
  ok(!r.warnings.some(x => /可视圆/.test(x)), "320×320 不该触发圆屏警告");

  // 背景 480×480 也是方的：它的四角在圆外 —— 背景**本来就该铺满**，
  // 所以这里要有警告（提醒"四角看不见"），但不拒绝
  const rb = AS.prepareAsset(big, 480, 480, { roleId: "background", targetId: "s3" });
  eq(rb.errors.length, 0, "背景超圆不拒绝（铺满是对的）");
  ok(rb.warnings.some(x => /可视圆/.test(x)),
     "背景 480×480 的四角超出可视圆 ⇒ 有提醒（让人知道四角看不见）");
  // 而 336×336 刚好贴住内切圆：不该有圆警告
  const safe = boxRgba(336, 336, null, [1, 2, 3, 255], 0);
  const rs = AS.prepareAsset(safe, 336, 336, { roleId: "background", targetId: "s3" });
  ok(!rs.warnings.some(x => /可视圆/.test(x)), "336×336 贴住内切圆，不触发圆警告");
}

// ============================================================
section("七、体积口径（推荐形态装得下）");
{
  for (const t of ["classic", "s3", "s3_240"]) {
    const b = AS.budget(t);
    ok(b.fullSetBytes > 0, t + " 的整套体积算得出来");
    // ★ 经典板（1MB）装不下"满屏 480 背景 + 10 张 300" —— 这正是它要
    //   默认给 128 的原因；这里把结论钉住，免得有人以为经典板也能这么干
    if (t === "classic") {
      ok(b.fullSetBytes > b.partitionBytes,
         "经典板 1MB 装不下" + b.desc + "（" + b.fullSetBytes + " > " + b.partitionBytes +
         "）⇒ 这也是它默认给 " + IB.FACE_TIER_LEGACY_DEFAULT + " 的原因");
    } else {
      ok(b.fullSetBytes < b.partitionBytes,
         t + " 装得下" + b.desc + "（" + b.fullSetBytes + " < " + b.partitionBytes + "）");
    }
  }
  // 240 档那块板（8MB）的整套：240×240×2 + 10×152×152×3 + 1420
  {
    const b = AS.budget("s3_240");
    eq(b.backgroundBytes, 240 * 240 * 2, "240 背景字节数");
    eq(b.faceBytes, 152 * 152 * 3, "一张 152 表情的字节数");
    eq(b.fullSetBytes, 1420 + 240 * 240 * 2 + 10 * 152 * 152 * 3,
       "整套 = 头 + 背景 + 10 张表情");
  }
}

// ------------------------------------------------------------
console.log("\n" + "=".repeat(56));
if (fail === 0) {
  console.log("全部通过:" + pass + " 项断言");
} else {
  console.log("失败 " + fail + " 项 / 共 " + (pass + fail) + " 项");
  failures.forEach(f => console.log("  - " + f));
}
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
