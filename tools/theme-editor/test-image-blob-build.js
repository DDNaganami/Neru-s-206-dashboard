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
// 左右屏各一套,编号错位会让"右屏显示成左屏的表情"这种问题查很久
eq(IB.ROLE.Background, 1, "背景");
eq(IB.ROLE.BootFrame, 2, "开机动画帧");
eq(IB.ROLE.FaceIdle, 3, "左屏·常态");
eq(IB.ROLE.FaceRedline, 4, "左屏·红区");
eq(IB.ROLE.FaceSurprise, 5, "左屏·惊喜");
eq(IB.ROLE.FaceIdleR, 6, "右屏·常态");
eq(IB.ROLE.FaceRedlineR, 7, "右屏·红区");
eq(IB.ROLE.FaceSurpriseR, 8, "右屏·惊喜");
// ★ 9/10 是保留编号(曾是左右屏开机图,已去掉:开机画面走程序化扫表)。
//   断言它们**没有被定义** —— 一旦有人把新角色塞进去,别人已导出的
//   image.bin 会突然变成另一个角色,而且不报错。新角色请从 11 开始。
eq(IB.ROLE.BootLogo, undefined, "9 必须保持保留(不要复用)");
eq(IB.ROLE.BootLogoR, undefined, "10 必须保持保留(不要复用)");
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
  const res = IB.build([{ name: "a", w: 2, h: 2, pixels: new Uint8Array(8) }]);
  const h = IB.toCHeader(res.blob);
  ok(h.includes("const uint8_t g_image_blob[1428]"), "C 数组长度要对(1420 头 + 8 像素)");
  ok(h.includes("0x32, 0x30, 0x36, 0x44"), "魔数字节要出现在数组里");
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
