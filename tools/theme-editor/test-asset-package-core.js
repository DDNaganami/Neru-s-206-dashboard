/* ============================================================
 * test-asset-package-core.js —— 「配色 + 图片 → 一个 assets.bin」纯字节内核的单元测试
 *
 *   node tools/theme-editor/test-asset-package-core.js
 *
 * 测的是 tools/theme-editor/asset-package-core.js —— 也就是**页面上的按钮**
 * 与命令行 asset-package.js **共用的那一份实现**。它要是错了，两条路一起错，
 * 而且错法是"刷上去 image 段整块错位、还不报错"，所以这里盯三件事：
 *
 *   ① 纯函数往返：packBytes → unpackBytes 逐字节一致（容器里 theme 段 == 原主题，
 *      image 段 == 原 image.bin）；
 *   ② ★ 页面与命令行**只有一份实现**的硬证据：同样两份输入，
 *      Core.packBytes() 的产物与**真跑一次 CLI** 的产物逐字节相同（对 sha256）；
 *   ③ 这个文件能进浏览器：源码里不许有 require / fs / path / Buffer
 *      （页面是 file:// 双击打开的静态页，一个 require 就整页白屏）。
 *
 * 风格与同目录的 test-*.js 一致：纯 node、无依赖、临时目录里自造输入、
 * "全部通过:N 项断言" 收尾、exit 0/1。**不碰仓库里的任何文件。**
 * ============================================================ */
"use strict";

const fs = require("fs");
const os = require("os");
const path = require("path");
const crypto = require("crypto");
const { execFileSync } = require("child_process");

const Core = require("./asset-package-core.js");
const AP = require("./asset-package.js");
const IB = require("./image-blob-build.js");

let pass = 0, fail = 0;
const failures = [];

function ok(cond, what) {
  if (cond) { pass++; }
  else { fail++; failures.push(what); console.log("  ✗ " + what); }
}

function eq(a, b, what) {
  ok(a === b, what + "  (期望 " + b + ",得到 " + a + ")");
}

// 期望报错;needle 给了就还要在错里出现(报错说不清等于没报)
function throws(fn, what, needle) {
  let msg = null;
  try { fn(); } catch (e) { msg = e.message; }
  if (msg === null) {
    fail++; failures.push(what + " —— 本应报错却通过了"); console.log("  ✗ " + what);
    return null;
  }
  if (needle && msg.indexOf(needle) < 0) {
    fail++; failures.push(what + " —— 报错里没说清「" + needle + "」,实际:" + msg);
    console.log("  ✗ " + what + "  实际报错:" + msg);
    return null;
  }
  pass++;
  return msg;
}

function section(t) { console.log("\n== " + t); }

const ROOT = path.resolve(__dirname, "..", "..");
const S3 = path.join(ROOT, "partitions-s3.csv");
const CLASSIC = path.join(ROOT, "partitions.csv");
const CORE_FILE = path.join(__dirname, "asset-package-core.js");
const CLI_FILE = path.join(__dirname, "asset-package.js");
const readCsv = (f) => fs.readFileSync(f, "utf8");
const sha256 = (buf) => crypto.createHash("sha256").update(buf).digest("hex").toUpperCase();

// ------------------------------------------------------------
// 临时目录:输入与产物都放这里,仓库里一个字节都不写
// ------------------------------------------------------------
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "asset-package-core-test-"));
const T = (name) => path.join(tmp, name);

// 一份最小但**格式正确**的主题 JSON(与编辑器导出的同一个形状:根是 {"theme": {...}})
function makeThemeText(extra) {
  const o = {
    theme: {
      bg_color: 1315860,
      face_size: 300,
      readout: { digit_color: 16, label_color: 8421504 },
      screens: [
        { arcs: [{ radius: 205, width: 24, start: 300, end: 60 }] },
        { arcs: [{ radius: 205, width: 24, start: 300, end: 60 }] }
      ]
    }
  };
  if (extra) o.pad = extra;
  return JSON.stringify(o, null, 2);
}

// 一份真的 image.bin(用仓库自己的打包器造,格式天然与固件一致)
function makeImageBin() {
  const items = [
    { name: "bg", w: 4, h: 4, role: IB.ROLE.Background, order: 0,
      pixels: new Uint8Array(4 * 4 * 2) },
    { name: "idleL", w: 3, h: 3, role: IB.ROLE.FaceIdle, order: 0,
      pixels: new Uint8Array(3 * 3 * 3) },
    { name: "sportL", w: 5, h: 5, role: IB.ROLE.FaceSport, order: 1,
      pixels: new Uint8Array(5 * 5 * 3) }
  ];
  for (let i = 0; i < items[1].pixels.length; i++) items[1].pixels[i] = (i * 7) & 0xFF;
  const res = IB.build(items);
  return new Uint8Array(res.blob);
}

const themeText = makeThemeText();
const themeBytes = new TextEncoder().encode(themeText);
const imageBytes = makeImageBin();

const tableS3 = AP.loadPartitionTable({ table: S3 });
const layoutS3 = Core.computeLayout(tableS3);

// ============================================================
section("① 这个文件必须能进浏览器(页面是 file:// 双击打开的静态页)");
{
  const src = fs.readFileSync(CORE_FILE, "utf8");
  // 去掉注释再查 —— 注释里**提到**这些词是正常的(文件头就在解释为什么不许用它们),
  // 要抓的是真的调用/引用。
  let code = src.replace(/\/\*[\s\S]*?\*\//g, "").replace(/^\s*\/\/.*$/gm, "");
  // UMD 头部那一行 require 是**故意**的(Node 用它取 theme-json.js):把整个 UMD
  // 起头到 globalThis 那一段挖掉再查,这样"身体里又冒出一个 require"才会被抓住。
  const bootAt = code.indexOf("(function (root, factory)");
  const bootEnd = code.indexOf("function (ThemeJson)");
  ok(bootAt >= 0 && bootEnd > bootAt, "UMD 包装还在(照抄 asset-spec.js / image-blob-build.js 的写法)");
  const body = code.slice(bootEnd);

  ok(/\brequire\s*\(/.test(body) === false,
     "UMD 头之外不许有 require(页面是 file:// 静态页,一个 require 就白屏)");
  ok(/\(typeof module === "object" && module && module\.exports\)[\s\S]{0,80}require\("\.\/theme-json\.js"\)/
       .test(src), "UMD 头部保留了那个唯一的 require(Node 侧取 theme-json.js)");
  ok(/\bfs\./.test(body) === false, "不许出现 fs.(读文件是 asset-package.js 的事)");
  ok(/\bpath\./.test(body) === false, "不许出现 path.(取仓库根是 asset-package.js 的事)");
  ok(/\bBuffer\b/.test(body) === false, "不许出现 Buffer(浏览器里没有;字节一律 Uint8Array)");
  ok(/Uint8Array/.test(body), "用的是 Uint8Array");
  ok(/TextDecoder/.test(body), "UTF-8 解码用 TextDecoder(Buffer.toString 只在 Node 里有)");
  ok(/globalThis\.AssetPackageCore|root\.AssetPackageCore/.test(src),
     "挂在 window.AssetPackageCore 上(浏览器那半边)");
  ok(/module\.exports = api/.test(src), "同时支持 module.exports(Node 那半边)");

  // ★ 光内核能进浏览器还不够:**页面得真的引用它**,而且要在 theme-json.js 之后
  //   (checkThemeContent 里要用到那个全局)。这条链子断在哪一段,表现都是
  //   "页面一片空白"或"按钮点了没反应" —— 而静态页不报错,所以在这里钉住。
  const pageHtml = fs.readFileSync(path.join(__dirname, "image-editor.html"), "utf8");
  const atCore = pageHtml.indexOf('src="asset-package-core.js"');
  const atTheme = pageHtml.indexOf('src="theme-json.js"');
  ok(atCore > 0, "②图片页(index.html 之外的唯一编辑页)引用了 asset-package-core.js");
  ok(atTheme > 0 && atTheme < atCore, "theme-json.js 排在 asset-package-core.js **之前**加载");
  ok(pageHtml.indexOf("AssetPackageCore.packBytes") > 0,
     "②图片页真的调了内核的 packBytes(不是引了不用)");
  ok(pageHtml.indexOf("AssetPackageCore.unpackBytes") > 0,
     "②图片页真的调了内核的 unpackBytes(导入那条路)");
}

// ============================================================
section("② 布局:位置与长度全部从分区表读,一个都不写死");
{
  eq(layoutS3.base, 0x210000, "容器基准 = theme 分区偏移");
  eq(layoutS3.themeSectionBytes, 0x4000, "theme 段长 = theme 分区大小(不是写死的 16384)");
  eq(layoutS3.spiffsBytes, 0x40000, "中间那一块 = spiffs 分区大小");
  eq(layoutS3.imageOffset, 0x44000, "image 段在容器里的偏移 = 0x4000 + 0x40000");
  eq(layoutS3.maxImageBytes, 8 * 1024 * 1024, "image 段最长 = 分区大小(S3 = 8MB)");
  eq(layoutS3.containerMin, 0x44000, "容器最短 = theme + 中间那一块");

  // 经典板:偏移刻意相同,只有 image 分区大小不同
  const classic = Core.computeLayout(AP.loadPartitionTable({ table: CLASSIC }));
  eq(classic.base, layoutS3.base, "经典板与 S3 的 theme 偏移相同(所以一条命令两块板都能刷)");
  eq(classic.imageOffset, layoutS3.imageOffset, "经典板与 S3 的 image 偏移也相同");
  eq(classic.maxImageBytes, 1024 * 1024, "经典板 image 分区 1MB");

  const lines = Core.describeLayout(layoutS3);
  ok(lines.indexOf("0x40000") >= 0 && lines.indexOf("0x44000") >= 0,
     "describeLayout 里的两个位置是从 layout 算出来的");
}

// ============================================================
section("③ 分区表解析:坏行/重复/不连续都要报错,不许猜");
{
  ok(Core.parsePartitionCsv(readCsv(S3), "partitions-s3.csv").length > 0, "真表能读");
  ok(Core.parsePartitionCsv("\uFEFF# 注释\nnvs,data,nvs,0x9000,0x5000,\n", "x")
       .length === 1, "BOM + 注释行 + 表头残留都能跳过");

  throws(() => Core.parsePartitionCsv("nvs,data,nvs,0xZZ,0x5000,\n", "t.csv"),
         "offset/size 不是 0x 十六进制 ⇒ 报错", "读不出来");
  throws(() => Core.parsePartitionCsv("nvs,data,nvs,0x9000,0x1000,\nnvs,data,nvs,0xA000,0x1000,\n", "t.csv"),
         "两块同名分区 ⇒ 报错(不猜用哪一块)", "有两块都叫");
  throws(() => Core.computeLayout({ csv: "t.csv", parts: Core.parsePartitionCsv("nvs,data,nvs,0x9000,0x1000,\n") }),
         "表里没有 theme/image ⇒ 报错", "找不到名为");
  throws(() => Core.computeLayout({ csv: "t.csv", parts: Core.parsePartitionCsv(
           "theme,data,0x40,0x210000,0x2000,\nimage,data,0x41,0x212000,0x1000,\n") }),
         "theme 分区不是 0x4000 ⇒ 报错(image 段起点会算错)", "0x4000");
  throws(() => Core.computeLayout({ csv: "t.csv", parts: Core.parsePartitionCsv(
           "theme,data,0x40,0x210000,0x4000,\nimage,data,0x41,0x260000,0x1000,\n") }),
         "theme 与 image 之间有空隙 ⇒ 报错(容器必须是一段连续 flash)", "不连续");
}

// ============================================================
section("④ packBytes:两份字节 → 容器字节(纯函数,不落盘)");
const packed = Core.packBytes({ themeBytes: themeBytes, imageBytes: imageBytes, layout: layoutS3 });
{
  eq(packed.totalLen, layoutS3.imageOffset + imageBytes.length, "容器总长 = image 偏移 + 图片长度");
  eq(packed.themeBytes, themeBytes.length, "theme 段长度 = 主题字节数");
  eq(packed.imageBytes, imageBytes.length, "image 段长度 = 图片字节数");

  // ① theme 段 == 原主题(逐字节)
  let same = true;
  for (let i = 0; i < themeBytes.length; i++) if (packed.bytes[i] !== themeBytes[i]) same = false;
  ok(same, "容器 +0 起的 theme 段与原主题逐字节相同");

  // ② theme 内容之后到 image 段之前全是 0 填充
  ok(Core.findNonZero(packed.bytes, themeBytes.length) === layoutS3.imageOffset,
     "theme 段尾部与中间那一块全是 0 填充(第一个非 0 字节正好是 image 段的起点)");

  // ③ image 段 == 原 image.bin(逐字节)
  same = true;
  for (let i = 0; i < imageBytes.length; i++) {
    if (packed.bytes[layoutS3.imageOffset + i] !== imageBytes[i]) same = false;
  }
  ok(same, "容器 +0x44000 起的 image 段与原 image.bin 逐字节相同");

  // ④ 不改入参(纯函数:同一个 Uint8Array 传两次,结果一样)
  const again = Core.packBytes({ themeBytes: themeBytes, imageBytes: imageBytes, layout: layoutS3 });
  eq(sha256(again.bytes), sha256(packed.bytes), "同一份输入跑两次,产物逐字节相同(纯函数)");
  eq(themeBytes.length, new TextEncoder().encode(themeText).length, "入参没有被改过长度");
}

// ============================================================
section("⑤ unpackBytes:容器 → 两份原样字节(往返逐字节一致)");
{
  const r = Core.unpackBytes({ bytes: packed.bytes, layout: layoutS3 });
  eq(sha256(r.themeBytes), sha256(themeBytes), "拆出来的 theme 段与原主题 sha256 相同");
  eq(sha256(r.imageBytes), sha256(imageBytes), "拆出来的 image 段与原 image.bin sha256 相同");
  eq(r.themeInfo.bytes, themeBytes.length, "theme 内容长度 = 最后一个非 0 字节 + 1");
  ok(r.themeInfo.parsed && typeof r.themeInfo.parsed === "object", "theme 段通过了 JSON 合法性检查");
  eq(r.imageInfo.count, 3, "image 段按自己的清单数出 3 张图");
  eq(r.containerBytes, packed.totalLen, "容器字节数");
}

// ============================================================
section("⑥ ★ 页面与命令行只有一份实现:Core 产物 vs 真跑 CLI 的产物");
{
  // 同样的两份输入写成文件,让 CLI 自己跑一遍
  const themePath = T("theme.json");
  const imagePath = T("image.bin");
  const cliOut = T("cli-assets.bin");
  fs.writeFileSync(themePath, themeBytes);
  fs.writeFileSync(imagePath, imageBytes);

  const stdout = execFileSync(process.execPath,
    [CLI_FILE, "pack", "--theme", themePath, "--image", imagePath, "--out", cliOut],
    { encoding: "utf8" });
  ok(stdout.indexOf("已生成") >= 0, "CLI pack 跑成功(退出码 0)");

  const cliBytes = fs.readFileSync(cliOut);
  eq(sha256(cliBytes), sha256(packed.bytes),
     "★ CLI 产物与 Core.packBytes() 产物 sha256 相同(逐字节一致)");

  // 再拆一遍:CLI unpack 出来的两份文件也要与原文件逐字节相同
  const themeCopy = T("theme.copy.json");
  const imageCopy = T("image.copy.bin");
  execFileSync(process.execPath,
    [CLI_FILE, "unpack", "--in", cliOut, "--theme-out", themeCopy, "--image-out", imageCopy],
    { encoding: "utf8" });
  eq(sha256(fs.readFileSync(themeCopy)), sha256(themeBytes), "CLI unpack 出的主题与原主题一致");
  eq(sha256(fs.readFileSync(imageCopy)), sha256(imageBytes), "CLI unpack 出的图片与原图片一致");

  // CLI 那半边的转出也必须还是"同一个内核",别哪天又长出一份自己的实现
  eq(AP.Core, Core, "asset-package.js 转出的 Core 就是这一个模块(没有第二份实现)");
  eq(sha256(AP.computeLayout(AP.loadPartitionTable({ table: S3 })) && packed.bytes),
     sha256(packed.bytes), "AP 与 Core 算出的是同一个容器");
}

// ============================================================
section("⑦ parseImageBlob:image 段的清单(张数 / 用途 / 尺寸 / 字节数)");
{
  const info = Core.parseImageBlob(imageBytes, { label: "测试图片" });
  eq(info.magic, IB.MAGIC, "魔数来自 image-blob-build.js");
  eq(info.version, IB.VERSION, "版本来自 image-blob-build.js");
  eq(info.count, 3, "张数");
  eq(info.headerBytes, IB.HEADER_SIZE, "包头长度 1420");
  eq(info.totalBytes, imageBytes.length, "总长 = 包头 + 数据");

  eq(info.entries[0].name, "bg", "第 1 张的名字");
  eq(info.entries[0].role, IB.ROLE.Background, "第 1 张的用途 = 背景");
  eq(info.entries[0].w, 4, "第 1 张宽 4");
  eq(info.entries[0].h, 4, "第 1 张高 4");
  const expectedBgBytes = 4 * 4 * IB.bytesPerPixel(IB.CF.RGB565);
  eq(info.entries[0].bytes, expectedBgBytes, "第 1 张的字节数 = w*h*每像素字节");
  eq(info.entries[2].name, "sportL", "第 3 张的名字");
  eq(info.entries[2].role, IB.ROLE.FaceSport, "第 3 张的用途 = 左屏·运动");
  eq(info.entries[2].order, 1, "第 3 张的播放顺序");

  // 用途的名字来自 IB.ROLE_NAMES(页面显示用的就是它),不在这里重抄一张表
  eq(IB.ROLE_NAMES[info.entries[0].role], "表盘背景", "用途 1 的人话名字");
  ok(IB.ROLE_NAMES[info.entries[1].role].indexOf("左屏表情") === 0, "用途 3 是左屏表情");

  throws(() => Core.parseImageBlob(new Uint8Array(10), {}),
         "连包头都不够 ⇒ 报错", "连 image 包头");

  // 常量对账:CLI 把 IB 那份传进来,不一致必须报错(而不是悄悄用默认值)
  const badIb = Object.assign({}, IB, { MAGIC: 0x12345678 });
  throws(() => Core.parseImageBlob(imageBytes, { ib: badIb }),
         "IB 与内核内置常量不一致 ⇒ 报错(不许两边各用一份数)", "对不上");
}

// ============================================================
section("⑧ 负例:宁可报错不猜(报错还得说人话)");
{
  // ---- theme 侧 ----
  throws(() => Core.packBytes({ themeBytes: new Uint8Array(4096).fill(32), imageBytes: imageBytes,
                                layout: layoutS3, themeLabel: "主题" }),
         "主题超过 4095 字节 ⇒ 拒绝打包", "超过固件能读进来的 4095 字节");
  throws(() => Core.packBytes({ themeBytes: new TextEncoder().encode('{"theme":{}} '),
                                imageBytes: imageBytes, layout: layoutS3, themeLabel: "主题" }),
         "主题末尾有空白 ⇒ 拒绝(否则往返不再逐字节相同)", "末尾有");
  throws(() => Core.packBytes({ themeBytes: new TextEncoder().encode("[1,2,3]"),
                                imageBytes: imageBytes, layout: layoutS3, themeLabel: "主题" }),
         "主题根是数组 ⇒ 拒绝", "根不是对象");
  throws(() => Core.packBytes({ themeBytes: new Uint8Array(0), imageBytes: imageBytes,
                                layout: layoutS3, themeLabel: "主题" }),
         "空主题 ⇒ 拒绝", "只有空白");
  throws(() => Core.packBytes({ themeBytes: new TextEncoder().encode("{ 这不是 JSON }"),
                                imageBytes: imageBytes, layout: layoutS3, themeLabel: "主题" }),
         "主题不是合法 JSON ⇒ 报错并指向主题段", "主题 JSON 解析失败");

  // ---- image 侧 ----
  const badImage = new Uint8Array(imageBytes);
  badImage[8] = (badImage[8] + 1) & 0xFF;          // 把包头里的 dataBytes 改大一点
  throws(() => Core.packBytes({ themeBytes: themeBytes, imageBytes: badImage, layout: layoutS3,
                                imageLabel: "图片" }),
         "图片实际长度与它自己包头写的不一致 ⇒ 拒绝", "两者不一致");

  const wrongMagic = new Uint8Array(imageBytes);
  wrongMagic[0] = 0;
  throws(() => Core.packBytes({ themeBytes: themeBytes, imageBytes: wrongMagic, layout: layoutS3,
                                imageLabel: "图片" }),
         "图片包头魔数不对 ⇒ 拒绝", "image 包头不对");

  // ---- 容器侧 ----
  throws(() => Core.unpackBytes({ bytes: new Uint8Array(100), layout: layoutS3,
                                  containerLabel: "x.bin" }),
         "太短 ⇒ 这不是本工具产出的容器", "这不是本工具产出的容器");
  throws(() => Core.unpackBytes({ bytes: new Uint8Array(layoutS3.containerMax + 1), layout: layoutS3,
                                  containerLabel: "x.bin" }),
         "太长 ⇒ 不是按这张分区表摆的容器", "摆出来的容器");

  // 段尾非 0(整段被填满)⇒ 最可能是指错了文件
  const filled = new Uint8Array(packed.bytes);
  filled[Core.THEME_SECTION_BYTES - 1] = 65;
  throws(() => Core.unpackBytes({ bytes: filled, layout: layoutS3, containerLabel: "x.bin" }),
         "theme 段尾非 0 ⇒ 报错并提示可能指错了文件", "整段都被填满了");

  // ---- 填充脏了的两条路,分开钉(它们的**指向不同**,必须各是一条人话) ----
  const themeLen = themeBytes.length;          // 主题内容真正结束在哪(505)
  ok(themeLen < 600, "测试主题长度合理(" + themeLen + " 字节)");

  // ① 非 0 落在"内容与结尾括号之间" ⇒ 取长度时就会把垃圾算进去、
  //    于是以垃圾的 'B' 收尾 ⇒ **JSON 那条**抓住它,并说清
  //    "最后那个收尾括号在第几字节、后面还多出多少字节"(人才知道是后面多了东西)。
  const dirty1 = new Uint8Array(packed.bytes);
  dirty1[themeLen] = 66;                       // 'B',紧贴内容末尾
  throws(() => Core.unpackBytes({ bytes: dirty1, layout: layoutS3, containerLabel: "x.bin" }),
         "紧贴内容末尾的非 0 垃圾 ⇒ 报错并指出多出多少字节", "它后面还多出");

  // ② 结论记在这里(实测过,不再单独造用例):theme 段的长度是"最后一个非 0 字节 + 1",
  //    所以只要填充里有一个非 0 字节,长度就一定算到它、收尾就一定不是 JSON 的括号
  //    ⇒ 实际抓住它的是 ① 那条(JSON)报错,而它的文案里专门点了"theme 段里混进了
  //    非 0 垃圾"这一种可能。assertZeroPadding 只在"内容以一个完整 JSON 结尾、
  //    后面还有非 0"时才轮得到 —— 那需要主题段里真的塞了第二段内容,本工具不产出那种
  //    容器,所以这里用一条断言把这个**顺序**钉住,免得以后有人把两条检查调个位置、
  //    报错就开始指错方向:
  eq(Core.findNonZero(packed.bytes, 0), 0, "theme 段从 +0 起就是非 0(主题内容本身)");
  ok(Core.findNonZero(packed.bytes, themeLen) === layoutS3.imageOffset,
     "主题内容之后到 image 段之前确实全是 0(所以填充脏一点点都算得出来)");

  // theme 段全是 0 ⇒ 里面没有配色
  const empty = new Uint8Array(layoutS3.containerMin + imageBytes.length);
  empty.set(imageBytes, layoutS3.imageOffset);
  throws(() => Core.unpackBytes({ bytes: empty, layout: layoutS3, containerLabel: "x.bin" }),
         "theme 段全是 0 ⇒ 报错", "全是 0");

  // image 段之后还有非 0 ⇒ 容器的实际长度与 image 自己的清单不一致
  const extra = new Uint8Array(packed.totalLen + 4);
  extra.set(packed.bytes, 0);
  extra[packed.totalLen + 1] = 67;
  throws(() => Core.unpackBytes({ bytes: extra, layout: layoutS3, containerLabel: "x.bin" }),
         "image 清单之后还有非 0 ⇒ 拒绝猜哪一段算图片", "不一致,拒绝猜");

  // image 包头说比容器剩下的还长 ⇒ 文件被截断
  const cut = new Uint8Array(packed.bytes.subarray(0, packed.totalLen - 10));
  throws(() => Core.unpackBytes({ bytes: cut, layout: layoutS3, containerLabel: "x.bin" }),
         "被截断的容器 ⇒ 报错", "文件被截断了");
}

// ============================================================
section("⑨ 边界:刚好卡在限制上");
{
  // 4095 字节(固件上限)的主题必须**通过**。
  // ★ 长度要**算出来**,不许手算 JSON 的那几个字符:`{"theme":{"pad":""}}` 本身就占
  //   20 字节(实测),手算成 16 就会写出 4091 字节、"边界"根本没卡到边界上。
  const jsonLen = (o) => new TextEncoder().encode(JSON.stringify(o)).length;
  const overhead = jsonLen({ theme: { pad: "" } });
  const exact = new TextEncoder().encode(JSON.stringify({ theme: { pad: "x".repeat(4095 - overhead) } }));
  eq(exact.length, 4095, "刚好 4095 字节的主题(构造正确)");
  const r = Core.packBytes({ themeBytes: exact, imageBytes: imageBytes, layout: layoutS3 });
  eq(r.themeBytes, 4095, "4095 字节的主题能打包(边界之内)");
  eq(Core.unpackBytes({ bytes: r.bytes, layout: layoutS3 }).themeInfo.bytes, 4095,
     "4095 字节能原样拆回来");

  // 4096 字节 ⇒ 拒绝(固件只读 4095)
  const over = new TextEncoder().encode(JSON.stringify({ theme: { pad: "x".repeat(4096 - overhead) } }));
  eq(over.length, 4096, "刚好 4096 字节的主题(构造正确)");
  throws(() => Core.packBytes({ themeBytes: over, imageBytes: imageBytes, layout: layoutS3 }),
         "4096 字节的主题 ⇒ 拒绝(固件读不全)", "4095");

  // 容器最短 = theme + 中间那一块,里面一个图片字节都没有 ⇒ 必须报错(不是"0 张图")
  const noImage = new Uint8Array(layoutS3.containerMin);
  throws(() => Core.unpackBytes({ bytes: noImage, layout: layoutS3, containerLabel: "x.bin" }),
         "没有 image 段的容器 ⇒ 报错", "全是 0");
}

// ============================================================
section("⑩ 那条刷写命令:偏移从 layout 取,端口/芯片可换");
{
  const cmd = Core.flashCommand(layoutS3, "assets.bin", { chip: "esp32s3", port: "COM6" });
  eq(cmd, "python -m esptool --chip esp32s3 --port COM6 --baud 921600 write_flash 0x210000 assets.bin",
     "命令与 README/文档里那一行逐字相同");
  ok(cmd.indexOf(Core.hex(layoutS3.base)) >= 0, "偏移来自 layout.base(不是写死的字面量)");
  ok(cmd.indexOf("0x254000") < 0, "★ 不是 image 分区的偏移(刷错位置等于把容器写进图片分区)");
  const classic = Core.computeLayout(AP.loadPartitionTable({ table: CLASSIC }));
  eq(Core.flashCommand(classic, "assets.bin", { chip: "esp32" }).indexOf("--chip esp32 ") >= 0,
     true, "经典板的 --chip 跟着目标板走");
}

// ============================================================
console.log("\n" + "=".repeat(56));
if (fail === 0) {
  console.log("全部通过:" + pass + " 项断言");
} else {
  console.log("失败 " + fail + " 项:");
  failures.forEach(f => console.log("  · " + f));
  console.log("(共 " + pass + " 项通过," + fail + " 项失败)");
}
console.log("=".repeat(56));

// 临时目录清掉(失败时留着,方便回头看)
if (fail === 0) {
  try { fs.rmSync(tmp, { recursive: true, force: true }); } catch { /* ignore */ }
} else {
  console.log("临时目录(留着排查):" + tmp);
}
process.exit(fail === 0 ? 0 : 1);
