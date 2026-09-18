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
eq(Object.keys(IB.ROLE).length, 9, "角色表里正好 9 项(1 背景 + 8 表情)");
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
    ok(!!expected[id], id + " 在分区表里有 image 行");
    if (expected[id]) {
      eq("0x" + t.partitionBytes.toString(16), expected[id],
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
section("页面:导入时的默认输出尺寸(只缩不放)");
// 为什么值得单测:这里踩过一次,而且是用户踩的 ——
//   导入时的默认输出宽度**写死 240**,不管原图多大。
//   用户按建议做好了 8 张 152×152 的图,导进去却被放大成 240×240,
//   占用从 542KB 变成 1350KB,界面直接报 132% 溢出。
//   而缩略图看起来一样大,他只能看到"我明明做的是 152,怎么还溢出"。
// 现在的规则:默认 = min(原图宽, 240),并向下对齐到 4 —— 保证**不会放大**。
{
  const html = require("fs").readFileSync(__dirname + "/image-editor.html", "utf8");
  const mw = /const DEFAULT_MAX_W = (\d+);/.exec(html);
  if (!mw) throw new Error("image-editor.html 里找不到 DEFAULT_MAX_W");
  const fm = /function pickDefaultSize\(([\s\S]*?)\n\}/.exec(html);
  if (!fm) throw new Error("image-editor.html 里找不到 pickDefaultSize");
  const pick = new Function("const DEFAULT_MAX_W = " + mw[1] + ";\n" +
                            fm[0] + "\nreturn pickDefaultSize;")();

  eq(pick(152), 152, "★ 152×152 的源图输出还是 152(不许放大成 240)");
  eq(pick(100), 100, "比 240 小的源图按原尺寸");
  eq(pick(480), 240, "480 的源图缩到 240(省分区;要满屏自己点 480)");
  eq(pick(800), 240, "大图缩到 240");
  eq(pick(10), 8, "极小的图向下对齐到 4 的倍数,不会反而放大");
  eq(pick(0), 240, "拿不到原图宽度时按保守值");

  // ★ 不变式:对任何尺寸都"只缩不放"(下限 8 那次除外)
  let grew = [];
  for (let n = 8; n <= 900; n++) {
    if (pick(n) > n) grew.push(n + "→" + pick(n));
  }
  eq(grew.length, 0, "8..900 里没有任何尺寸被放大" + (grew.length ? ": " + grew.slice(0, 5) : ""));

  // ★ 用户那次的实际账:8 张 152 的表情必须装得下
  const eight152 = IB.HEADER_SIZE + 8 * (152 * 152 * 3);
  ok(eight152 <= IB.PARTITION_BYTES,
     "8 张 152×152 表情 = " + eight152 + " 字节,占分区 " +
     (100 * eight152 / IB.PARTITION_BYTES).toFixed(1) + "%");
  // 再加一张满屏 480 背景(用户的目标形态)
  const withBg = eight152 + (480 * 480 * 2);
  ok(withBg <= IB.PARTITION_BYTES,
     "8 张 152 表情 + 一张 480×480 背景 = " + withBg + " 字节,占分区 " +
     (100 * withBg / IB.PARTITION_BYTES).toFixed(1) + "%");
  // 而被放大成 240 的版本必须是超的 —— 这正是当时的现象
  const eight240 = IB.HEADER_SIZE + 8 * (240 * 240 * 3);
  ok(eight240 > IB.PARTITION_BYTES,
     "8 张 240×240 表情 = " + eight240 + " 字节,确实超了(用户看到的 132%)");
}

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
