/* ============================================================
 * test-image-blob-build.js —— 打包器自己的单元测试(Node 运行)
 *
 *   node tools/theme-editor/test-image-blob-build.js
 *
 * 测的是"打包器会不会安静地产生坏数据"。
 * 浏览器里的图形界面用的就是同一个 build(),所以这里测过就等于
 * 界面导出的 bin 也过了这些校验。
 *
 * 与固件解析器的一致性由 test-image-roundtrip.ps1 负责,两者互补:
 *   · 这里:打包器算得对不对(纯 JS)
 *   · 那边:固件读不读得懂(JS → C)
 * ============================================================ */
"use strict";

const IB = require("./image-blob-build.js");
const B = require("./build-image-bin.js");

let pass = 0, fail = 0;
const failures = [];

function ok(cond, what) {
  if (cond) { pass++; }
  else { fail++; failures.push(what); console.log("  ✗ " + what); }
}

function eq(a, b, what) {
  ok(a === b, what + "  (期望 " + b + ",得到 " + a + ")");
}

function throws(fn, what) {
  try { fn(); fail++; failures.push(what + " —— 本应报错却通过了"); console.log("  ✗ " + what); }
  catch { pass++; }
}

function section(t) { console.log("\n== " + t); }

// ------------------------------------------------------------
section("常量必须与 image_blob.h 一致");
// 这几个数字是格式契约。改了 C 头文件却忘了改这里,设备端会解析失败。
eq(IB.MAGIC, 0x44363032, "魔数");
eq(IB.VERSION, 1, "版本");
eq(IB.NAME_MAX, 24, "名字长度");
eq(IB.MAX_COUNT, 32, "图片数量上限");
eq(IB.ENTRY_SIZE, 44, "索引项大小");
eq(IB.HEADER_SIZE, 1420, "镜像头大小(12 + 32×44)");
eq(IB.PARTITION_BYTES, 1024 * 1024, "image 分区大小(partitions.csv: 0x100000)");
eq(IB.CF.RGB565, 0x12, "LV_COLOR_FORMAT_RGB565");

// ------------------------------------------------------------
section("表情画布按**屏的分辨率**分档(480 / 240)");
// ★ 为什么值得测(2026-09-20,owner 实屏报上来的):
//   现在实配的板子是 240×240 双圆屏,而画布上限一直是按 480 基准几何算的 320。
//   一张 300×300 的图(半径 150)在 240 屏上比整块屏还大 —— 会盖住内圈两条副弧,
//   而且**不报错**,只有刷进去才看得出来。所以上限必须跟屏走。
//   这两条同时钉住"数值互相自洽"和"和固件/主题文件的弧几何一致"。
{
  const ThemeJson = require("./theme-json.js");
  const fs = require("fs");
  const root = require("path").resolve(__dirname, "..", "..");
  const theme = ThemeJson.parseThemeJson(
    fs.readFileSync(require("path").join(__dirname, "theme-default.json"), "utf8")).theme;

  eq(IB.FACE_SIZE_TIERS.length, 2, "只有两个分辨率档(480 与 240)");
  eq(IB.FACE_SIZE_TIERS[0].id, "res480", "第 0 档 = 480 基准");
  eq(IB.FACE_SIZE_TIERS[1].id, "res240", "第 1 档 = 240×240");

  // ① 每档的上限都必须**由内圈副弧内沿半径**兜住:画布不越过那条弧,
  //    而且是 4 的倍数(输入框 step=4)。具体值再单独钉一遍(②下面的 eq)。
  //    为什么不用"公式"钉:480 档历史上取的是 320(= floor(326/4)*4 再让 2px),
  //    那是已经实测定稿、S3/480 的行为**不许变**的数,所以不能反推成一个统一公式。
  for (const t of IB.FACE_SIZE_TIERS) {
    ok(t.faceCanvasMax <= t.faceInnerMostRadius * 2,
       t.id + ":画布(" + t.faceCanvasMax + ")不能超过内沿直径(" +
       t.faceInnerMostRadius * 2 + "),否则盖住副弧");
    eq(t.faceCanvasMax % 4, 0, t.id + ":画布上限要是 4 的倍数");
    ok(t.faceSizeRecommended <= t.faceCanvasMax,
       t.id + ":推荐值不能超过上限");
    ok(t.faceCanvasMax - t.faceSizeRecommended >= 4,
       t.id + ":推荐值要离上限留一点余量(留 " +
       (t.faceCanvasMax - t.faceSizeRecommended) + "px)");
  }
  // ② 两档的具体数值:480 那档是**老值,不许动**;240 那档是这次新加的。
  eq(IB.FACE_SIZE_TIERS[0].faceCanvasMax, 320, "480 档画布上限仍是 320");
  eq(IB.FACE_SIZE_TIERS[0].faceSizeRecommended, 300, "480 档推荐值仍是 300");
  eq(IB.FACE_SIZE_TIERS[1].faceCanvasMax, 160, "240 档画布上限 160(内沿直径 162)");
  eq(IB.FACE_SIZE_TIERS[1].faceSizeRecommended, 152, "240 档推荐值 152(与 480 档同样留 8px)");

  // ③ 内沿半径必须能从 theme-default.json 的副弧几何直接算出来:
  //      内沿 = radius - width/2
  //    240 档 = 上面那套 × 240/480(theme_scale())。
  //    ★ 这两个数分别对应固件 ui_theme.h 的 arcs[1](radius 168 / width 10)
  //      与 esp32s3-spi 那个 env 的 -DTHEME_DISPLAY_RES=240。
  //      改了主题里的副弧几何却没改颜色档,这里会红。
  const coolantArc = theme.screens[0].arcs.find(a => a.kind === 2);
  ok(!!coolantArc, "theme-default.json 里有水温弧(kind 2)");
  const inner480 = coolantArc.radius - Math.floor(coolantArc.width / 2);
  eq(IB.FACE_SIZE_TIERS[0].faceInnerMostRadius, inner480,
     "480 档的内沿半径 = " + coolantArc.radius + " - " + coolantArc.width + "/2");
  eq(IB.FACE_SIZE_TIERS[1].faceInnerMostRadius, Math.floor(inner480 * 240 / 480),
     "240 档的内沿半径 = 480 那套 × 240/480(theme_scale())");

  // ④ 老常量的语义一个字节都不许变(网页里那三处引用还在用它们)
  eq(IB.FACE_CANVAS_MAX, 320, "FACE_CANVAS_MAX 仍是 480 档的 320");
  eq(IB.FACE_SIZE_RECOMMENDED, 300, "FACE_SIZE_RECOMMENDED 仍是 480 档的 300");
  eq(IB.ARC_INNER_MOST_RADIUS, 163, "ARC_INNER_MOST_RADIUS 仍是 480 档的 163");
  eq(IB.faceCanvasMax(), IB.FACE_CANVAS_MAX, "无参数的 faceCanvasMax() = 480 档");
  eq(IB.faceSizeRecommended(), IB.FACE_SIZE_RECOMMENDED, "无参数的 faceSizeRecommended() = 480 档");

  // ⑤ 目标板 → 分辨率档的映射:经典板与 S3 是 480 那块屏,240 验证板是 240
  eq(IB.TARGETS.classic.faceTier, "res480", "经典板 = 480 屏");
  eq(IB.TARGETS.s3.faceTier, "res480", "S3(最终 2.8\" 屏)= 480 屏");
  eq(IB.TARGETS.s3_240.faceTier, "res240", "微雪双屏验证板 = 240 屏");
  eq(IB.faceCanvasMaxFor("s3_240"), 160, "240 档的画布上限");
  eq(IB.faceSizeRecommendedFor("s3_240"), 152, "240 档的推荐值");
  eq(IB.arcInnerMostRadiusFor("s3_240"), 81, "240 档的内沿半径");
  eq(IB.faceCanvasMaxFor("s3"), 320, "S3 仍是 320");
  eq(IB.faceSizeRecommendedFor("s3"), 300, "S3 仍是 300");
  eq(IB.faceCanvasMaxFor("classic"), 320, "经典板同样是 480 屏 → 320");
  eq(IB.faceCanvasMaxFor(), IB.FACE_CANVAS_MAX, "不传参数用默认目标板(经典板 = 480 档)");
  // 两块板共用一份分区表(同一块 S3 板,只有屏不同)—— 分区大小必须一致,
  // 不然"按目标板选大小"这件事在页面上会给出两个不同的分母。
  eq(IB.TARGETS.s3_240.partitionBytes, IB.TARGETS.s3.partitionBytes,
     "s3_240 与 s3 是同一块板(同一份 partitions-s3.csv)");
  eq(IB.TARGETS.s3_240.partitionsCsv, "partitions-s3.csv", "240 那块板的分区表");
  throws(() => IB.faceCanvasMaxFor("esp32c3"), "未知目标板要报错,不能悄悄按默认算");
}

section("角色编号必须与 image_blob.h 的 ImageRole 一致");
// 两屏各自独立、状态集合还不一样(左有红区、右有惊喜),编号错位会让
// "右屏显示成左屏的表情"这种问题查很久
eq(IB.ROLE.Background, 1, "背景");
// 左屏(转速表):常态 / 红区 / 巡航 / 运动
eq(IB.ROLE.FaceIdle, 3, "左屏·常态");
eq(IB.ROLE.FaceRedline, 4, "左屏·红区");
eq(IB.ROLE.FaceCruise, 12, "左屏·巡航");
eq(IB.ROLE.FaceSport, 13, "左屏·运动");
// 右屏(速度表):常态 / 惊喜 / 巡航 / 运动
eq(IB.ROLE.FaceIdleR, 6, "右屏·常态");
eq(IB.ROLE.FaceOverspeedR, 8, "右屏·超速(号 8 沿用当年的'惊喜')");
eq(IB.ROLE.FaceCruiseR, 17, "右屏·巡航");
eq(IB.ROLE.FaceSportR, 18, "右屏·运动");
// ★ 保留编号一律不服复用(2=开机帧、5=左屏惊喜、7=右屏红区、9/10=开机图、
//   11/16=眨眼图、14/15/19/20=冷车/过热图)。断言它们**没有被定义** ——
//   一旦有人把新角色塞进去,别人已导出的 image.bin 会突然变成另一个角色,
//   而且不报错。新角色请从 21 开始。
for (var reserved of [2, 5, 7, 9, 10, 11, 14, 15, 16, 19, 20]) {
  eq(Object.values(IB.ROLE).indexOf(reserved) >= 0, false,
     reserved + " 是保留编号,不能被复用");
  eq(IB.ROLE_NAMES[reserved], undefined, reserved + " 不该有显示名");
}
// 一共 8 张表情 + 1 张背景
eq(Object.keys(IB.ROLE).length, 11, "角色表里正好 11 项(1 背景 + 10 表情 = 每屏 5 档)");
// 每个角色都要有中文名(界面下拉框要用),漏了会显示成 undefined
for (var rk in IB.ROLE) {
  ok(IB.ROLE_NAMES[IB.ROLE[rk]] !== undefined, "角色 " + rk + " 要有显示名");
}
// 反查:ROLE_NAMES 里不许有指向不存在角色的条目(删角色时最容易漏)
for (var nk in IB.ROLE_NAMES) {
  ok(Object.values(IB.ROLE).indexOf(Number(nk)) >= 0,
     "ROLE_NAMES 里的 " + nk + " 要有对应的角色");
}

// ------------------------------------------------------------
section("RGB565 打包:字节序与位移");
// 纯红 → 0xF800,小端存成 00 F8
{
  const px = IB.rgbaToRgb565(new Uint8Array([255, 0, 0, 255]), 1, 1);
  eq(px.length, 2, "1×1 RGB565 占 2 字节");
  eq(px[0], 0x00, "纯红低字节");
  eq(px[1], 0xF8, "纯红高字节(小端!写反了会变蓝)");
}
{
  const px = IB.rgbaToRgb565(new Uint8Array([0, 0, 255, 255]), 1, 1);
  eq(px[0], 0x1F, "纯蓝低字节");
  eq(px[1], 0x00, "纯蓝高字节");
}
{
  const px = IB.rgbaToRgb565(new Uint8Array([0, 255, 0, 255]), 1, 1);
  eq(px[1], 0x07, "纯绿高字节");
  eq(px[0], 0xE0, "纯绿低字节");
}
eq(IB.pack565(255, 255, 255), 0xFFFF, "纯白");
eq(IB.pack565(0, 0, 0), 0x0000, "纯黑");

// ------------------------------------------------------------
section("行尾填充与行序");
{
  // 2×2,每行补 2 字节 → 每行 6 字节,共 12
  // RGBA 必须写全 2×2×4 = 16 字节(少写会读越界,数错会得到"看起来对"的错觉)
  const rgba = new Uint8Array([
    255, 0, 0, 255,   0, 255, 0, 255,     // 第 0 行:红(偏移0), 绿(偏移4)
    0, 0, 255, 255,   255, 255, 0, 255    // 第 1 行:蓝(偏移8), 黄(偏移12)
  ]);
  const px = IB.rgbaToRgb565(rgba, 2, 2, { stridePad: 2 });
  eq(px.length, 12, "2×2 + 2 字节填充 = 12 字节");

  // 第 0 行:红=0xF800 → 00 F8;绿=0x07E0 → E0 07;再补 2 字节 0
  eq(px[0], 0x00, "第0行像素0=红 低字节");
  eq(px[1], 0xF8, "第0行像素0=红 高字节");
  eq(px[2], 0xE0, "第0行像素1=绿 低字节");
  eq(px[3], 0x07, "第0行像素1=绿 高字节");
  eq(px[4], 0x00, "填充字节为 0");
  eq(px[5], 0x00, "填充字节为 0");

  // ★ 第 1 行必须从第 6 字节(stride)开始 —— 这里错了整张图会斜
  eq(px[6], 0x1F, "第1行像素0=蓝 低字节");
  eq(px[7], 0x00, "第1行像素0=蓝 高字节");
  eq(px[8], 0xE0, "第1行像素1=黄 低字节");
  eq(px[9], 0xFF, "第1行像素1=黄 高字节");
  eq(px[10], 0x00, "第1行填充字节");
  eq(px[11], 0x00, "第1行填充字节");
}

// ------------------------------------------------------------
section("alpha 混合:PNG 透明边不能变黑边");
{
  const rgba = new Uint8Array([255, 255, 255, 0]);   // 全透明白
  const px = IB.rgbaToRgb565(rgba, 1, 1, { alphaBg: [0, 0, 0] });
  eq(px[0], 0x00, "全透明 → 纯背景色(黑)");
  eq(px[1], 0x00, "全透明 → 纯背景色(黑)");
}

// ------------------------------------------------------------
section("build():头部字段");
{
  const px = IB.rgbaToRgb565(new Uint8Array(4 * 4 * 4).fill(0x80), 4, 4);
  const res = IB.build([{ name: "a", w: 4, h: 4, cf: IB.CF.RGB565, role: IB.ROLE.Background, order: 0, pixels: px }]);
  const dv = new DataView(res.blob.buffer);
  eq(res.totalBytes, 1420 + 32, "总长 = 头 + 像素");
  eq(dv.getUint32(0, true), IB.MAGIC, "魔数在偏移 0");
  eq(dv.getUint16(4, true), IB.VERSION, "版本在偏移 4");
  eq(dv.getUint16(6, true), 1, "数量在偏移 6");
  eq(dv.getUint32(8, true), 32, "data_bytes 在偏移 8");

  // ★ 项内偏移是 C 对齐规则推出来的,不是字段顺序。每一条都要钉住 ——
  //   错一位的后果是设备端读出乱七八糟的宽高,然后拿着越界指针去画。
  const B = 12;
  eq(dv.getUint32(B + 0, true), 0, "offset 在项内偏移 0(u32)");
  eq(dv.getUint32(B + 4, true), 32, "size 在项内偏移 4");
  eq(dv.getUint16(B + 8, true), 4, "w 在项内偏移 8");
  eq(dv.getUint16(B + 10, true), 4, "h 在项内偏移 10");
  eq(res.blob[B + 12], IB.CF.RGB565, "cf 在项内偏移 12");
  eq(res.blob[B + 13], 0, "stride_pad 在项内偏移 13");
  eq(dv.getUint16(B + 14, true), 1, "role 在项内偏移 14");
  eq(dv.getUint16(B + 16, true), 0, "order 在项内偏移 16");
  eq(res.blob[B + 18], "a".charCodeAt(0), "名字在项内偏移 18");
  eq(res.blob[B + 19], 0, "名字后有终止符");
  eq(res.blob[B + 41], 0, "名字区到项内偏移 41 为止(24 字节)");
  // ★ 没有尾部填充:44 = 4+4+2+2+1+1+2+2+24,本来就是 4 的倍数,编译器不用补
  eq(res.blob[B + 43], 0, "项的最后 1 字节(共 44 字节)");

  // 像素紧跟在头后面:0x80 灰(128,128,128) → RGB565 = 0x8410
  // 注意是 0x80=128 而不是 200:pack565(200,200,200) = 0xCE59,低字节是 0x59
  eq(res.blob[1420], 0x10, "偏移 1420 = 第 1 个像素的低字节");
  eq(res.blob[1421], 0x84, "偏移 1421 = 第 1 个像素的高字节");
}

// ------------------------------------------------------------
section("build():超过 64KB 的图必须能装下");
// ★ 回归测试。offset 曾经是 u16,把整个镜像的像素数据卡在 64KB 以内 ——
//   而一张 480×480 的 RGB565 背景就要 450KB。
//   当时的代码会直接抛错("数据偏移超过 65535 字节"),看上去像"图太大"这个
//   正常限制,其实是格式缺陷:1MB 的分区等于白买。
//   这条测试保证以后没人把它改回 u16。
{
  const w = 300, h = 300;                       // 300×300×2 = 180000 字节 > 64KB
  const px = new Uint8Array(w * h * 2);
  for (let i = 0; i < px.length; i++) px[i] = i & 0xFF;
  const res = IB.build([
    { name: "big",  w: w, h: h, pixels: px },
    { name: "big2", w: w, h: h, pixels: px }    // 第二项偏移 180000,远超 65535
  ]);
  eq(res.entries[0].offset, 0, "第 0 项偏移");
  eq(res.entries[1].offset, 180000, "第 1 项偏移 > 65535 必须能表达");

  const dv = new DataView(res.blob.buffer);
  // ★ 真去读二进制里的偏移字段 —— 这才是设备端看到的值
  eq(dv.getUint32(12 + 44 + 0, true), 180000, "第 1 项的 offset 字段读回来要对");
  eq(res.totalBytes, 1420 + 360000, "总长");
  ok(res.totalBytes <= IB.PARTITION_BYTES, "360KB 要能放进 1MB 分区");
}

// ------------------------------------------------------------
section("build():每项的名字必须各写各的");
// 回归测试。曾经的 bug:写索引项时用的是上一轮循环遗留的 `name` 变量
// (var 没有块作用域),于是**每一项都被写成最后一张图的名字**。
// 这个错误编译不报、肉眼看不出来,但设备端"按名字找图"会全部失效。
{
  const mk = (w) => new Uint8Array(w * 2 * 2);
  const res = IB.build([
    { name: "first",  w: 2, h: 2, pixels: mk(2) },
    { name: "second", w: 2, h: 2, pixels: mk(2) },
    { name: "third",  w: 2, h: 2, pixels: mk(2) }
  ]);
  const readName = (i) => {
    const base = 12 + i * IB.ENTRY_SIZE;
    let s = "";
    for (let c = 0; c < IB.NAME_MAX; c++) {
      const ch = res.blob[base + 18 + c];
      if (ch === 0) break;
      s += String.fromCharCode(ch);
    }
    return s;
  };
  eq(readName(0), "first", "第 0 项名字");
  eq(readName(1), "second", "第 1 项名字");
  eq(readName(2), "third", "第 2 项名字");
}

// ------------------------------------------------------------
section("build():多项偏移要累加");
{
  // 每张图的大小必须和声明的 w/h/cf/pad 自洽,否则 build() 会拒收
  const rgb565 = (w, h, pad) => IB.rgbaToRgb565(new Uint8Array(w * h * 4).fill(200), w, h, { stridePad: pad });
  const rgb888 = (w, h) => {
    // RGB888 是紧排的 3 字节/像素:直接造字节,不走 rgbaToRgb565
    const out = new Uint8Array(w * h * 3);
    for (let i = 0; i < out.length; i++) out[i] = i & 0xFF;
    return out;
  };
  const res = IB.build([
    { name: "one",   w: 4, h: 4, cf: IB.CF.RGB565, role: 1, order: 0, pixels: rgb565(4, 4, 0) },
    { name: "two",   w: 2, h: 2, cf: IB.CF.RGB565, role: 2, order: 1, pixels: rgb565(2, 2, 2) },
    { name: "three", w: 8, h: 8, cf: IB.CF.RGB888, role: 3, order: 2, pixels: rgb888(8, 8) }
  ]);
  eq(res.entries[0].size, 32, "第 0 项大小(4×4×2)");
  eq(res.entries[1].size, 12, "第 1 项大小(2×2×2 + 每行 2 填充)");
  eq(res.entries[2].size, 192, "第 2 项大小(8×8×3)");
  eq(res.entries[0].offset, 0, "第 0 项 offset");
  eq(res.entries[1].offset, 32, "第 1 项 offset = 第 0 项大小");
  eq(res.entries[2].offset, 44, "第 2 项 offset 累加");
  eq(res.dataBytes, 236, "data_bytes");
  eq(res.totalBytes, 1420 + 236, "总长");
  eq(res.blob[1420], res.entries[0].pixels[0], "第 0 项的像素紧跟在头后面");
}

// ------------------------------------------------------------
section("build():坏输入必须报错(不能安静地打包坏数据)");
throws(() => IB.build([]), "空列表");
throws(() => IB.build(new Array(33).fill({ w: 1, h: 1, pixels: new Uint8Array(2) })), "超过 32 张");
// 刚好 32 张要能通过(上限是 32,不是 31)
{
  const items = [];
  for (let i = 0; i < IB.MAX_COUNT; i++) {
    items.push({ name: "i" + i, w: 2, h: 2, pixels: new Uint8Array(8) });
  }
  const r = IB.build(items);
  eq(r.entries.length, IB.MAX_COUNT, "正好 32 张应该通过");
}
throws(() => IB.build([{ name: "x", w: 4, h: 4, pixels: new Uint8Array(31) }]), "像素字节数不够");
throws(() => IB.build([{ name: "x", w: 0, h: 4, pixels: new Uint8Array(0) }]), "宽度为 0");
throws(() => IB.build([{ name: "x".repeat(24), w: 2, h: 2, pixels: new Uint8Array(8) }]), "名字太长");
throws(() => IB.build([{ name: "x", w: 2, h: 2, cf: 0xEE, pixels: new Uint8Array(8) }]), "未知颜色格式");
throws(() => IB.build([{ name: "x", w: 2, h: 2, stridePad: 300, pixels: new Uint8Array(8) }]), "stride_pad 越界");
// 注:这里曾经有一条"累计超过 65535 字节要报错"的测试。
// 那条测试在**保护一个缺陷** —— offset 是 u16 导致整个镜像装不下 64KB,
// 而一张 480×480 背景就要 450KB。格式改成 u32 之后这条限制不存在了,
// 换成了上面那条"超过 64KB 必须能装下"。别再把它加回来。

// ------------------------------------------------------------
section("刷写命令与 C 头文件");
{
  const cmd = IB.esptoolCommand("COM7", "image.bin");
  // ★ 偏移必须跟 partitions.csv 的 image 行一致。改了分区表忘了改这里,
  //   症状是"刷进去了但设备说没有图片资源"(刷到了别的地方)。
  ok(cmd.includes("0x254000"), "刷写偏移必须是 image 分区起始 0x254000");
  ok(cmd.includes("COM7"), "端口要带上");
  // ★ --chip 必须跟着目标板:拿 --chip esp32 去刷 S3 会被 esptool 当场拒掉
  ok(cmd.includes("--chip esp32 "), "经典板用 --chip esp32");
  const cmdS3 = IB.esptoolCommand("COM7", "image.bin", "s3");
  ok(cmdS3.includes("--chip esp32s3 "), "S3 板必须用 --chip esp32s3");
  ok(cmdS3.includes("0x254000"),
     "两块板的刷写偏移相同(S3 也走 0x254000,所以只有 chip 是变量)");
  // ★ 微雪 240 那块板**也是 ESP32-S3**(同一份 partitions-s3.csv、同样的 8MB
  //   image 分区),所以它必须是 --chip esp32s3。这条是回归测试:
  //   以前这里写的是 `targetId === "s3" ? "esp32s3" : "esp32"`,加第三块板时
  //   240 板会印出 `--chip esp32` —— 而这条命令是页面直接给用户复制的,
  //   复制到终端就是 esptool 的一句话拒绝("Chip is ESP32-S3 ... but
  //   --chip esp32 was specified")。选对目标板反而拿到不能用的命令。
  const cmd240 = IB.esptoolCommand("COM7", "image.bin", "s3_240");
  ok(cmd240.includes("--chip esp32s3 "), "微雪 240 那块板也是 ESP32-S3,必须 --chip esp32s3");
  ok(cmd240.includes("0x254000"), "240 板的刷写偏移同样是 0x254000");
  ok(cmd240.includes("COM7"), "端口要带上");
  // ★ 每一块目标板的 chip 都必须真的出现在它自己的命令里 ——
  //   以后加板子忘了填 chip 字段,这里会立刻红(而不是印一条不能用的命令)。
  for (const id of Object.keys(IB.TARGETS)) {
    const c = IB.esptoolCommand("COM7", "image.bin", id);
    ok(c.includes("--chip " + IB.TARGETS[id].chip + " "),
       id + " 的刷写命令要用它自己的 chip(" + IB.TARGETS[id].chip + ")");
    ok(c.includes("0x254000"), id + " 的刷写偏移");
  }
  throws(() => IB.esptoolCommand("COM7", "image.bin", "esp32c3"),
         "未知目标板不许悄悄按经典板印命令");
  const res = IB.build([{ name: "a", w: 2, h: 2, pixels: new Uint8Array(8) }]);
  const h = IB.toCHeader(res.blob);
  ok(h.includes("const uint8_t g_image_blob[1428]"), "C 数组长度要对(1420 头 + 8 像素)");
  ok(h.includes("0x32, 0x30, 0x36, 0x44"), "魔数字节要出现在数组里");
}

// ------------------------------------------------------------
section("分区表必须是纯 ASCII(否则 PlatformIO 在中文 Windows 上直接报错)");
// ★ 这条守卫是踩出来的:2026-09-18 给 S3 加 partitions-s3.csv 时,
//   注释里写了一个"★"(UTF-8 = E2 98 85)。PlatformIO 的
//   builder/main.py 用 `open(partitions_csv)`(**不指定编码**)读它 →
//   中文 Windows 上按 GBK 解码 → UnicodeDecodeError 0x85。
//   症状很迷惑:固件**已经编译并链接成功**("Successfully created esp32s3 image"),
//   只在最后一步 checkprogsize 里炸,看起来像"编译失败",其实是注释里一个星号。
//   partitions.csv 的注释里本来就写着"keep this file pure ASCII",现在用机器盯着。
{
  const fs = require("fs");
  const path = require("path");
  const root = path.resolve(__dirname, "..", "..");
  const files = ["partitions.csv", "partitions-s3.csv"];
  for (const f of files) {
    const p = path.join(root, f);
    if (!fs.existsSync(p)) { ok(false, f + " 不存在"); continue; }
    const buf = fs.readFileSync(p);
    const bad = [];
    for (let i = 0; i < buf.length; i++) if (buf[i] >= 0x80) bad.push(i);
    ok(bad.length === 0,
       f + " 必须纯 ASCII" + (bad.length
         ? "(有 " + bad.length + " 个非 ASCII 字节,第一个在 offset " + bad[0] +
           ":0x" + buf[bad[0]].toString(16).toUpperCase() + ")"
         : ""));
    // 顺带把 BOM 也挡住:CSV 开头的 BOM 会让 PlatformIO 把第一个字段读成空
    ok(!(buf[0] === 0xEF && buf[1] === 0xBB && buf[2] === 0xBF), f + " 不能有 UTF-8 BOM");
  }
  // 两份分区表的 theme/image 偏移必须一致 —— 网页编辑器只印一套 esptool 命令,
  // 偏移一旦分叉,其中一块板子就会刷到错误的地方(而且不报错)。
  const parse = (f) => {
    const out = {};
    for (const line of fs.readFileSync(path.join(root, f), "utf8").split(/\r?\n/)) {
      // subtype 有两种写法:nvs / ota / spiffs 这种**名字**,或 theme/image 那种 0x40/0x41
      const m = /^([a-z0-9_]+),\s*data,\s*([0-9a-z]+),\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),\s*$/i
        .exec(line.trim());
      if (m) out[m[1]] = { subtype: m[2].toLowerCase(), offset: m[3].toLowerCase(), size: m[4].toLowerCase() };
    }
    return out;
  };
  const a = parse("partitions.csv"), b = parse("partitions-s3.csv");
  // ★ TARGETS 里那两块板的 image 分区大小必须**等于分区表里写的**
  //   (2026-09-18 审核指出:固件放宽到 8MB 了,编辑器还按 1MB 拦,
  //    S3 上多出来的 7MB 从网页根本导出不进去)。
  //   直接读 csv 对账,而不是再抄一遍数字 —— 抄的那份迟早会和分区表分叉。
  const expected = { classic: a.image.size, s3: b.image.size };
  for (const id of Object.keys(IB.TARGETS)) {
    const t = IB.TARGETS[id];
    // ★ 几块板可以共用同一份分区表(经典板是 partitions.csv;S3 的两块 ——
    //   480 那块与微雪 240 那块 —— 共用 partitions-s3.csv)。
    //   所以这里按**各自的 partitionsCsv** 找期望值,而不是要求每个 id 都有一行。
    const csv = id === "classic" ? a : b;
    const exp = csv.image.size;
    ok(!!exp, id + " 的分区表(" + t.partitionsCsv + ")里有 image 行");
    if (exp) {
      eq("0x" + t.partitionBytes.toString(16), exp,
         id + " 的 image 分区大小必须与 " + t.partitionsCsv + " 一致");
    }
    eq(IB.partitionBytesFor(id), t.partitionBytes, "partitionBytesFor(" + id + ") 要对上");
  }
  eq(IB.partitionBytesFor(), IB.TARGETS[IB.DEFAULT_TARGET].partitionBytes,
     "不传参数时用默认目标板");
  eq(IB.PARTITION_BYTES, IB.TARGETS.classic.partitionBytes,
     "PARTITION_BYTES(老代码用的那个名字)必须还是经典板的值");
  ok(IB.TARGETS.s3.partitionBytes > IB.TARGETS.classic.partitionBytes,
     "S3 的 image 分区必须比经典板大(不然加这个开关没意义)");
  throws(() => IB.partitionBytesFor("esp32c3"), "未知目标板要报错,不能悄悄按默认算");
  const need = ["theme", "image", "spiffs", "nvs", "otadata"];
  for (const k of need) {
    ok(!!a[k] && !!b[k], "两份分区表都有 " + k);
    if (a[k] && b[k]) {
      eq(b[k].offset, a[k].offset, k + " 的偏移两份必须一致(网页只印一套刷写命令)");
      eq(b[k].subtype, a[k].subtype, k + " 的 subtype 两份必须一致");
    }
  }
  // 大小可以不同(S3 那份就是要把 image 变大),但**必须更大**,否则加它没意义
  ok(parseInt(b.image.size, 16) > parseInt(a.image.size, 16),
     "S3 的 image 分区要比 4MB 那份大(" + b.image.size + " > " + a.image.size + ")");
}

// ------------------------------------------------------------
section("页面里两个纯函数:名字截断与字节长度");
// 从 image-editor.html 里抽出来跑,而不是抄一份 —— 抄一份就会漂移。
// 为什么值得测:设备端 name 是 char[24],**按字节**算。
// 一个汉字 3 字节,所以"24 个字符"的中文名会写坏设备端的索引表。
{
  const html = require("fs").readFileSync(__dirname + "/image-editor.html", "utf8");
  const grab = (name) => {
    const m = new RegExp("function " + name + "\\(([\\s\\S]*?)\\n\\}").exec(html);
    if (!m) throw new Error("image-editor.html 里找不到函数 " + name);
    return m[0];
  };
  const src = grab("byteLen") + "\n" + grab("safeName") +
              "\nreturn { byteLen: byteLen, safeName: safeName };";
  const sb = new Function(src)();

  eq(sb.byteLen("abc"), 3, "ASCII 字符 1 字节");
  eq(sb.byteLen("车"), 3, "汉字 3 字节(UTF-8)");
  eq(sb.byteLen("a车"), 4, "混合");

  ok(sb.safeName("abc.png").length > 0, "普通名字保留");
  // ★ 关键:截断必须按字节,确保留得下终止符
  const longCn = sb.safeName("车".repeat(30));
  ok(sb.byteLen(longCn) <= IB.NAME_MAX - 1,
     "超长中文名截断后字节数 ≤ " + (IB.NAME_MAX - 1) + "(实际 " + sb.byteLen(longCn) + ")");
  const longAscii = sb.safeName("x".repeat(40));
  ok(longAscii.length <= IB.NAME_MAX - 1, "超长 ASCII 名截断按字节");
  eq(sb.safeName(""), "img", "空名字有兜底");
}

// ------------------------------------------------------------
section("页面:导入时的默认输出尺寸(只缩不放 + 按角色设上限)");
// 为什么值得单测:这里踩过一次,而且是用户踩的 ——
//   导入时的默认输出宽度**写死一个数**,不管原图多大也不管角色。
//   用户按建议做好的图导进去却被放大,占用直接翻倍到报溢出,
//   而缩略图看起来一样大,他只能看到"我明明做的是那个尺寸,怎么还溢出"。
// 现在的规则(2026-09-18 重排,2026-09-20 按分辨率分档):上限**按角色**取,
//   而且**从共享常量读**:
//   表情 → min(原图, 该目标板的推荐值):480 屏 300、240 屏 152、
//          经典板(1MB 装不下 300)128
//   背景 → min(原图, 480)
//   再向下对齐到 4。保证不会放大,也保证不会超过几何上限(480 屏 320 / 240 屏 160:
//   再大盖住副弧)。
section("页面与共享常量没有分叉");
{
  const html = require("fs").readFileSync(__dirname + "/image-editor.html", "utf8");
  // ★ 页面里**不许**再出现写死的表情上限 —— 一律问 ImageBlob 的常量。
  //   这条是"改了共享常量但页面没跟上"的唯一守卫(那种错不报错,只是导出的图不对)。
  ok(html.indexOf("ImageBlob.faceCanvasMaxFor") >= 0,
     "页面用 ImageBlob.faceCanvasMaxFor(按目标板取,不写死 320)");
  ok(html.indexOf("ImageBlob.faceSizeRecommendedFor") >= 0,
     "页面用 ImageBlob.faceSizeRecommendedFor(按目标板取,不写死 300)");
  ok(html.indexOf("ImageBlob.arcInnerMostRadiusFor") >= 0,
     "页面用 ImageBlob.arcInnerMostRadiusFor(按目标板取,不写死 163)");
  // ★ 刷写那栏的分区大小也不许写死:曾经写着"大小 1024 KB",S3(8MB)与
  //   微雪 240(也是 8MB)上都是错的,用户会以为装不下。
  ok(html.indexOf("part-info") >= 0,
     "刷写那栏的分区大小由页面按目标板填(span#part-info)");
  ok(html.indexOf("，大小 1024 KB") < 0,
     "不许再写死「，大小 1024 KB」(S3 两版都是 8MB)");
  // ★ 预览画布必须跟着目标板的**屏**走:在 480 的画布里预览 240 的图会失真
  //   (240 的底图够不到弧带、152 的表情看着只有一点点大)。
  const pm = /function previewSide\(([\s\S]*?)\n\}/.exec(html);
  if (!pm) throw new Error("image-editor.html 里找不到 previewSide");
  const makeSide = (target) => new Function(
    "ImageBlob", "curTarget", pm[0] + "\nreturn previewSide;")(IB, target);
  eq(makeSide("s3_240")(), 240, "240 板:预览画布 240(1 像素 = 屏上 1 像素)");
  eq(makeSide("classic")(), 480, "经典板:预览画布仍是 480");
  eq(makeSide("s3")(), 480, "S3(480 屏):预览画布仍是 480");
  // 老常量仍在共享模块里(480 档),但不能被页面当成"唯一的上限"用
  ok(IB.FACE_CANVAS_MAX === 320 && IB.FACE_SIZE_RECOMMENDED === 300,
     "老常量仍在(480 档),行为不变");
  const fm = /function pickDefaultSize\(([\s\S]*?)\n\}/.exec(html);
  if (!fm) throw new Error("image-editor.html 里找不到 pickDefaultSize");
  // ★ 用真的共享常量 + 真的页面函数跑一遍(不复制逻辑)。
  //   注意:new Function 的函数体在**全局作用域**,拿不到模块的 require ——
  //   所以依赖一律走参数传进去(踩过:直接写 require 会 ReferenceError)。
  const FaceStages = require("./face-stages.js");
  const roles = new Set();
  FaceStages.SCREENS.forEach(sc => sc.states.forEach(x => roles.add(x.role)));
  const makePick = (target) => new Function(
    "ImageBlob", "FACE_ROLES", "curTarget",
    fm[0] + "\nreturn pickDefaultSize;")(IB, roles, target);
  const pick = makePick("s3");               // 480 屏:表情默认给推荐值 300
  const pickClassic = makePick("classic");   // 经典板:放不下 300,默认 128
  const pick240 = makePick("s3_240");        // 240 屏:推荐 152、上限 160

  eq(pick(152, 3), 152, "★ 152×152 的表情源图输出还是 152(不许放大)");
  eq(pick(100, 3), 100, "比上限小的源图按原尺寸");
  eq(pick(480, 3), IB.faceSizeRecommendedFor("s3"),
     "480 的表情源图缩到 480 屏的推荐值 300(不是缩小到 240 —— 那是旧策略)");
  eq(pick(800, 3), IB.faceSizeRecommendedFor("s3"), "大图缩到推荐值");
  eq(pick(10, 3), 8, "极小的图向下对齐到 4 的倍数,不会反而放大");
  eq(pick(0, 3), IB.faceSizeRecommendedFor("s3"), "拿不到原图宽度时按推荐值");
  eq(pick(480, 1), 480, "★ 背景可以到 480(它不需要躲开弧)");
  eq(pick(800, 1), 480, "背景大图缩到 480");
  eq(pickClassic(480, 3), IB.FACE_TIER_LEGACY_DEFAULT,
     "经典板(1MB)放不下 10 张 300,表情默认 128(一整套 + 背景 ≈ 0.93MB)");
  eq(pickClassic(480, 1), 480, "经典板的背景同样可以到 480");
  // ★ 240 屏(现在实配的那块板):一张 300×300 的图导进来会盖住副弧,
  //   所以默认与上限都必须按 240 档走 —— 这是这次加档要解决的那件事。
  eq(pick240(480, 3), 152, "240 屏:大表情源图缩到推荐值 152");
  eq(pick240(800, 3), 152, "240 屏:更大的源图同样是 152");
  eq(pick240(100, 3), 100, "240 屏:小图仍按原尺寸,不放大");
  eq(pick240(0, 3), 152, "240 屏:拿不到原图宽度时按推荐值");
  ok(pick240(480, 3) <= IB.faceCanvasMaxFor("s3_240"),
     "240 屏的默认输出(152)必须 ≤ 240 档上限(160)");
  // 背景在 240 屏上照样按 480 封顶:多出来的是浪费空间,但不是错误,
  // 而且"背景要铺满屏"这条在 240 上意味着 240 就够(页面提示里会讲)。
  eq(pick240(800, 1), 480, "240 屏:背景仍按 480 封顶(几何上不越界)");

  // ★ 不变式一:对任何尺寸、任何目标板都"只缩不放"(下限 8 那次除外)
  let grew = [];
  for (const tgt of Object.keys(IB.TARGETS)) {
    const p = makePick(tgt);
    for (let n = 8; n <= 900; n++) {
      if (p(n, 3) > n) grew.push(tgt + ":" + n + "→" + p(n, 3));
      if (p(n, 1) > n) grew.push(tgt + ":bg:" + n + "→" + p(n, 1));
    }
  }
  eq(grew.length, 0, "8..900 里没有任何尺寸被放大" + (grew.length ? ": " + grew.slice(0, 5) : ""));

  // ★ 不变式二:**每一块板**上表情的默认输出都不超它自己那档的几何上限
  //   (超了就会盖住副弧;240 那块板正是为此才要分档)
  for (const tgt of Object.keys(IB.TARGETS)) {
    const p = makePick(tgt);
    const cap = IB.faceCanvasMaxFor(tgt);
    let bad = 0;
    for (let n = 8; n <= 900; n++) if (p(n, 3) > cap) bad++;
    eq(bad, 0, tgt + ":表情默认输出永远 ≤ 画布上限 " + cap);
  }
}

  // ★ 用户那次的实际账:经典板(1MB)按新推荐值 128 必须装得下
  const n = IB.FACE_COUNT_PER_SET;
  const setClassic = IB.HEADER_SIZE + n * (IB.FACE_TIER_LEGACY_DEFAULT * IB.FACE_TIER_LEGACY_DEFAULT * 3);
  const bg480 = 480 * 480 * 2;
  ok(setClassic + bg480 <= IB.PARTITION_BYTES,
     n + " 张 " + IB.FACE_TIER_LEGACY_DEFAULT + "×" + IB.FACE_TIER_LEGACY_DEFAULT +
     " 表情 + 一张 480×480 背景 = " + (setClassic + bg480) +
     " 字节,占 1MB 分区 " + (100 * (setClassic + bg480) / IB.PARTITION_BYTES).toFixed(1) + "%");
  // 经典板放不下推荐的 300 —— 这正是"要用满 5 档就上 S3"的依据
  const setClassic300 = IB.HEADER_SIZE + n * (300 * 300 * 3);
  ok(setClassic300 > IB.PARTITION_BYTES,
     n + " 张 300×300 表情 = " + setClassic300 + " 字节,经典板 1MB 确实放不下");
  // ★ S3(8MB)上推荐的 300×300 一整套 + 480 背景必须放得下(3.0MB 左右)
  const setS3 = IB.HEADER_SIZE + n * (IB.faceSizeRecommendedFor("s3") * IB.faceSizeRecommendedFor("s3") * 3) + bg480;
  ok(setS3 <= IB.partitionBytesFor("s3"),
     "S3:" + n + " 张 " + IB.faceSizeRecommendedFor("s3") + "×" + IB.faceSizeRecommendedFor("s3") +
     " + 480 背景 = " + setS3 + " 字节,占 8MB 分区 " +
     (100 * setS3 / IB.partitionBytesFor("s3")).toFixed(1) + "%");
  // ★ 240×240 那块验证板:推荐 152 一整套 + 一张 240 背景,在 8MB 里几乎不占地方
  const rec240 = IB.faceSizeRecommendedFor("s3_240");
  const bg240 = 240 * 240 * 2;
  const set240 = IB.HEADER_SIZE + n * (rec240 * rec240 * 3) + bg240;
  ok(set240 <= IB.partitionBytesFor("s3_240"),
     "240 板:" + n + " 张 " + rec240 + "×" + rec240 + " + 一张 240 背景 = " + set240 +
     " 字节,占 8MB 分区 " + (100 * set240 / IB.partitionBytesFor("s3_240")).toFixed(1) + "%");
  ok(set240 < setS3,
     "240 档那一套(" + set240 + ")必须比 480 档(" + setS3 + ")小 —— 屏小就该更省");

// ------------------------------------------------------------
section("RGB565A8(带透明):布局与尺寸契约");
// 表情图要叠在弧线上面,所以必须带透明通道。这个格式的布局是
// 「上半部 RGB565 平面 + 下半部独立 A8 平面」(已在 LVGL 源码确认),
// 每像素 3 字节 —— 不是 2。
{
  // 2×2:红/绿 上排,蓝/黄 下排,alpha 分别 255/128
  const rgba = new Uint8Array([
    255, 0, 0, 255,    0, 255, 0, 128,
    0, 0, 255, 200,    255, 255, 0, 0
  ]);
  const px = IB.rgbaToRgb565A8(rgba, 2, 2);

  // 色平面 2×2×2 = 8 字节,A8 平面 2×2 = 4 字节,合计 12
  eq(px.length, 12, "2×2 RGB565A8 = 8(色) + 4(alpha) = 12 字节");
  eq(IB.packedBytesPerPixel(IB.CF.RGB565A8), 3, "RGB565A8 每像素 3 字节");

  // 色平面(小端)
  eq(px[0], 0x00, "红 低字节");
  eq(px[1], 0xF8, "红 高字节");
  eq(px[2], 0xE0, "绿 低字节");
  eq(px[3], 0x07, "绿 高字节");

  // A8 平面从第 8 字节开始,每像素 1 字节
  eq(px[8], 255, "像素0 alpha");
  eq(px[9], 128, "像素1 alpha");
  eq(px[10], 200, "像素2 alpha");
  eq(px[11], 0, "像素3 alpha(全透明)");

  // ★ 关键:build() 必须接受这个尺寸。
  //   曾经用 2 字节/像素校验,把合法镜像拒收(30000 vs 20000)。
  const res = IB.build([{ name: "t", w: 2, h: 2, cf: IB.CF.RGB565A8,
                          role: IB.ROLE.FaceIdle, order: 0, pixels: px }]);
  eq(res.entries[0].size, 12, "build 应接受 3 字节/像素的 RGB565A8");
  eq(res.blob[1420 + 8], 255, "blob 里 A8 平面第一字节");

  // 尺寸对不上必须报错(而不是安静地打包坏数据)
  throws(() => IB.build([{ name: "t", w: 2, h: 2, cf: IB.CF.RGB565A8,
                           role: IB.ROLE.FaceIdle, pixels: new Uint8Array(8) }]),
         "RGB565A8 少了 alpha 平面要报错");
}

// ------------------------------------------------------------
section("配方 → 图案");
{
  // 图案必须随位置变化,否则测不出行列错位
  const ramp = B.makePattern("ramp", 8, 4);
  eq(ramp.length, 8 * 4 * 4, "ramp RGBA 长度");
  // ★ 必须按**字节**比较某个通道,不能拿 rgba[0] 和 rgba[8*4] 比 ——
  //   那样比的是两个像素的 R 通道,而这个图案的 R 只随 x 变化,永远是 0。
  ok(ramp[0] !== ramp[7 * 4], "ramp 的 R 随 x 变化");
  ok(ramp[1] !== ramp[8 * 4 + 1], "ramp 的 G 随 y 变化");
  const chk = B.makePattern("checker", 8, 4);
  ok(chk[0] !== chk[4], "checker 相邻像素要不同");
  throws(() => B.makePattern("nope", 4, 4), "未知图案要报错");
}

// ------------------------------------------------------------
section("raw 规格解析");
{
  const s = B.parseRawSpec("bg.raw:480x480:rgb565:background:3:mybg");
  eq(s.w, 480, "宽"); eq(s.h, 480, "高");
  eq(s.cf, IB.CF.RGB565, "格式");
  eq(s.role, IB.ROLE.Background, "角色");
  eq(s.order, 3, "order");
  eq(s.name, "mybg", "名字");
  throws(() => B.parseRawSpec("a.raw:480:rgb565"), "尺寸格式错要报错");
  throws(() => B.parseRawSpec("a.raw:8x8:bogus"), "未知格式要报错");
  throws(() => B.parseRawSpec("a.raw:8x8:rgb565:notarole"), "未知角色要报错");
  // ★ rgb565a8:表情必须用的格式(带透明通道)。命令行的 --raw 以前只认
  //   rgb565/rgb888/a8/l8 —— 于是"用命令行打一套表情"根本打不出页面那种带
  //   透明的图,只能去改 JS。240 那块板的整套表情正是 3 字节/像素的。
  const a8 = B.parseRawSpec("f.raw:152x152:rgb565a8:face_idle:0:idleL");
  eq(a8.cf, IB.CF.RGB565A8, "rgb565a8 要能解析");
  eq(a8.role, IB.ROLE.FaceIdle, "表情角色");
  eq(IB.packedBytesPerPixel(a8.cf) * a8.w * a8.h, 152 * 152 * 3,
     "152×152 的 rgb565a8 = 69312 字节(--raw 的大小检查就按这个数)");
}

// ------------------------------------------------------------
section("命令行的角色表不许落后于 IB.ROLE(打不出一整套 10 张)");
// ★ 回归测试(2026-09-20):build-image-bin.js 的 ROLE_ID 曾经漏了
//   face_high(21) 与 face_city(22) —— 于是"命令行打一整套 10 张表情"
//   到第五档就报"不认识的角色: face_high",只有网页能导出全套。
//   角色表有两份(IB.ROLE 与命令行的名字表),这一条保证两份**同一个集合**。
{
  const setOf = (o) => new Set(Object.values(o));
  const have = setOf(B.ROLE_ID), want = setOf(IB.ROLE);
  for (const id of want) {
    ok(have.has(id), "命令行角色表要能打到角色号 " + id +
       "(" + (IB.ROLE_NAMES[id] || "?") + ")");
  }
  for (const id of have) {
    ok(want.has(id), "命令行角色表里的 " + id + " 必须是 IB.ROLE 里真实存在的角色");
  }
  eq(have.size, want.size, "两份角色表的集合大小要一致");
  // 名字要能真的解析(别只是表里有、roleId 却不认)
  eq(B.roleId("face_high"), IB.ROLE.FaceHigh, "face_high → 21");
  eq(B.roleId("face_city"), IB.ROLE.FaceCityR, "face_city → 22");
  eq(B.roleId("face_surprise_r"), IB.ROLE.FaceOverspeedR, "老别名 face_surprise_r 仍可用");
}

// ------------------------------------------------------------
section("spec-240.json:微雪 240 那块板的一整套素材必须打得出来");
// 这是 README「微雪双屏 240×240」一节里那条命令用的配方,所以在这里钉住
//   三件事:能打、多大、角色齐不齐。
// ★ 尺寸数字一旦写进文档就得有机器盯着 —— 改了推荐值/颜色格式之后,
//   文档里那句"809740 字节"会立刻变成谣言,而没人会去重算。
{
  const fs = require("fs"), path = require("path");
  const spec = JSON.parse(fs.readFileSync(path.join(__dirname, "spec-240.json"), "utf8"));
  eq(spec.images.length, 11, "配方 11 张(1 背景 + 10 表情)");
  const res = IB.build(B.specItems(spec));
  eq(res.entries.length, 11, "打出来 11 张");
  // 1420(头) + 240×240×2(背景,RGB565) + 10 × 152×152×3(表情,RGB565A8)
  eq(res.totalBytes, 809740, "总字节数(README 里引用的就是这个数)");
  eq(res.headerBytes, IB.HEADER_SIZE, "头 1420");
  eq(res.entries[0].cf, IB.CF.RGB565, "背景是 RGB565(2 字节/像素)");
  eq(res.entries[0].w, 240, "背景 240 宽");
  eq(res.entries[0].h, 240, "背景 240 高");
  for (let i = 1; i < res.entries.length; i++) {
    const e = res.entries[i];
    eq(e.cf, IB.CF.RGB565A8, "第 " + (i + 1) + " 张表情是 RGB565A8(要透出底下的弧)");
    eq(e.w, IB.faceSizeRecommendedFor("s3_240"), "表情宽度 = 240 档的推荐值 152");
    eq(e.size, 152 * 152 * 3, "一张表情 69312 字节");
  }
  // ★ 角色号必须**每个都在、且只出现一次**:少一个 = 那一档永远显示不出来
  //   (不报错,只会走降级链);多/重复 = 有人复制粘贴时忘了改用途。
  const roles = res.entries.map(e => e.role).sort((a, b) => a - b);
  const want = Object.values(IB.ROLE).sort((a, b) => a - b);
  eq(roles.join(","), want.join(","), "11 个角色号一个不少、一个不重");
  ok(res.totalBytes <= IB.partitionBytesFor("s3_240"),
     "占得进 8MB 分区(" + (100 * res.totalBytes / IB.partitionBytesFor("s3_240")).toFixed(1) + "%)");
  // 上限那一档也要算一遍:用满 160 时仍然装得下(宿主机那道 1MB 闸门也过)
  const maxSet = IB.HEADER_SIZE + 10 * (160 * 160 * 3) + 240 * 240 * 2;
  ok(maxSet <= IB.partitionBytesFor("s3_240"), "用满 160 也远小于 8MB");
  ok(maxSet <= IB.PARTITION_BYTES,
     "用满 160 的一整套(" + maxSet + " 字节)还要能过宿主机的 1MB 闸门" +
     "(IMAGE_BLOB_MAX_BYTES 在经典板口径下是 1MB;过不了就没法用往返测试核对)");
}

// ------------------------------------------------------------
section("配方 → items:颜色格式按角色显式选");
// ★ 为什么单独测:页面按角色**自动**选格式(背景 RGB565 / 表情 RGB565A8),
//   而命令行以前一律 RGB565 —— 同一条"表情"角色,页面导出的带透明、
//   命令行导出的不透明,刷进去前者正常、后者盖住弧线。两边格式必须一致。
{
  const it = B.specItems({
    images: [
      { pattern: "ramp",    w: 8, h: 4, role: "background",  name: "bg" },
      { pattern: "checker", w: 8, h: 8, role: "face_idle",   name: "idleL", cf: "rgb565a8" }
    ]
  });
  eq(it[0].cf, IB.CF.RGB565, "配方不写 cf → 还是 RGB565(老 spec 行为不变)");
  eq(it[0].pixels.length, 8 * 4 * 2, "背景 2 字节/像素");
  eq(it[1].cf, IB.CF.RGB565A8, "写了 cf=rgb565a8 → 用带透明的格式");
  eq(it[1].pixels.length, 8 * 8 * 3, "表情 3 字节/像素(含 A8 平面)");
  eq(it[1].role, IB.ROLE.FaceIdle, "角色编号");
  // 打包要接受它(否则"配方能写、打包报错"就是白写)
  const res = IB.build(it);
  eq(res.entries[1].size, 8 * 8 * 3, "打出来的表情项大小");
  // 不认识的 cf 要报错,不能悄悄当成 rgb565
  throws(() => B.specItems({ images: [{ pattern: "ramp", w: 4, h: 4, role: "face_idle", cf: "bogus" }] }),
         "未知 cf 要报错");
  // RGB565A8 没有行尾填充这一说,写了必须报错(不然尺寸对不上,报的还是别的原因)
  throws(() => B.specItems({ images: [{ pattern: "ramp", w: 4, h: 4, role: "face_idle",
                                        cf: "rgb565a8", stride_pad: 2 }] }),
         "rgb565a8 + stride_pad 要报错");
}

// ------------------------------------------------------------
section("配方 vs 素材规格:命令行也要给出同样的提醒(2026-09-24)");
// ★ 为什么放在这里而不是 test-asset-spec.js:这一组测的是**打包器**的行为
//   （specItems 顺手做的规格对账），规格本身的数字与拒绝路径在
//   test-asset-spec.js 里。两边各测自己那一半，不重不漏。
{
  // 表情用了 rgb565（没有 alpha）⇒ 必须有一条提醒
  const it1 = B.specItems({ images: [{ pattern: "ramp", w: 32, h: 32, role: "face_idle",
                                       cf: "rgb565" }] });
  ok(!!it1.assetWarnings, "表情用 rgb565 ⇒ 有提醒");
  ok((it1.assetWarnings || []).some(w => /rgb565a8/.test(w)),
     "提醒里点名了该怎么改：" + (it1.assetWarnings || []).join(" | "));

  // 背景用了 rgb565a8 ⇒ 白占 1/3 空间，也要提醒
  const it2 = B.specItems({ images: [{ pattern: "ramp", w: 32, h: 32, role: "background",
                                       cf: "rgb565a8" }] });
  ok(!!it2.assetWarnings, "背景用 rgb565a8 ⇒ 有提醒");

  // 超上限（480 档表情上限 320）⇒ 提醒里要有"盖住内圈副弧"
  const it3 = B.specItems({ images: [{ pattern: "ramp", w: 324, h: 324, role: "face_idle",
                                       cf: "rgb565a8" }] });
  ok(!!it3.assetWarnings, "324×324 超上限 ⇒ 有提醒");
  ok((it3.assetWarnings || []).some(w => /盖住内圈副弧/.test(w)), "提醒里说清了后果");

  // ★ 合规的一套**不许**有提醒（否则这个检查会变成噪音，人人学会忽略它）
  const it4 = B.specItems({ images: [
    { pattern: "solid", w: 480, h: 480, role: "background", cf: "rgb565" },
    { pattern: "transparent", w: 300, h: 300, role: "face_idle", cf: "rgb565a8" }
  ] });
  eq(it4.assetWarnings, undefined, "合规的一套没有提醒");

  // 240 档：300×300 的表情在 240 屏上超上限（160）⇒ 提醒必须跟着**目标板**变
  const it5 = B.specItems({ target: "s3_240", images: [
    { pattern: "transparent", w: 300, h: 300, role: "face_idle", cf: "rgb565a8" }] });
  ok(!!it5.assetWarnings, "240 档上 300×300 的表情要有提醒（上限只 160）");
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
