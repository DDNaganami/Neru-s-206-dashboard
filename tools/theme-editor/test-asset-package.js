/* ============================================================
 * test-asset-package.js —— 「配色 + 图片」合并容器的单元测试(Node 运行)
 *
 *   node tools/theme-editor/test-asset-package.js
 *
 * 测的是 asset-package.js 会不会**安静地**产出一个刷上去不对的容器。
 * 这里全部用自己造的输入(临时目录里生成一份主题 JSON 与一份真 image.bin),
 * 所以不依赖车主手里那两份文件,也不会往仓库里写任何东西。
 *
 * 与它互补的两件事(不在本文件里):
 *   · 真文件的往返一致:用 .dsh-drop 那两份真文件跑一遍 pack → unpack,
 *     对 sha256(见 tools/theme-editor/ASSET-PACKAGE.md 的「验收记录」)
 *   · 固件读不读得懂 image 段:test_image_roundtrip.ps1(JS → C)
 *
 * 风格与同目录的 test-*.js 一致:纯 node、无依赖、"全部通过:N 项断言" 收尾、exit 0/1。
 * ★ 这个文件**不进** syntax-check-pages.js(那份只管 HTML 里的内联脚本)。
 * ============================================================ */
"use strict";

const fs = require("fs");
const os = require("os");
const path = require("path");
const { execFileSync } = require("child_process");

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
const readCsv = (f) => fs.readFileSync(f, "utf8");

// ------------------------------------------------------------
// 临时目录:输入与产物都放这里,仓库里一个字节都不写
// ------------------------------------------------------------
const tmp = fs.mkdtempSync(path.join(os.tmpdir(), "asset-package-test-"));
const T = (name) => path.join(tmp, name);

// 一份最小但**格式正确**的主题 JSON(与编辑器导出的同一个形状:
// 根是 {"theme": {...}},颜色是十进制 —— 固件与 theme-json.js 都这么读)
function makeThemeJson(extra) {
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
  return Buffer.from(JSON.stringify(o, null, 2) + "\n".slice(0, 0), "utf8");
}

// 一份真的 image.bin(用仓库自己的打包器造,格式天然与固件一致)
function makeImageBin() {
  const items = [
    { name: "bg", w: 4, h: 4, role: IB.ROLE.Background, order: 0,
      pixels: new Uint8Array(4 * 4 * 2) },
    { name: "idleL", w: 3, h: 3, role: IB.ROLE.FaceIdle, order: 0,
      pixels: new Uint8Array(3 * 3 * 3) }
  ];
  for (let i = 0; i < items[1].pixels.length; i++) items[1].pixels[i] = (i * 7) & 0xFF;
  const res = IB.build(items);
  return Buffer.from(res.blob);
}

const themePath = T("theme.json");
const imagePath = T("image.bin");
fs.writeFileSync(themePath, makeThemeJson());
fs.writeFileSync(imagePath, makeImageBin());
const THEME_LEN = fs.statSync(themePath).size;
const IMAGE_LEN = fs.statSync(imagePath).size;

// ============================================================
section("布局:容器的三段位置**全部从分区表读**,不写死");
{
  const t = AP.loadPartitionTable({ table: S3 });
  const L = AP.computeLayout(t);

  eq(L.base, 0x210000, "容器基准 = theme 分区偏移");
  eq(L.themePart.sizeText, "0x4000", "theme 分区大小来自分区表");
  eq(L.imagePart.offsetText, "0x254000", "image 分区偏移来自分区表");
  eq(L.themeSectionBytes, 0x4000, "theme 段长 = theme 分区大小");
  eq(L.spiffsBytes, 0x40000, "中间那一块 = spiffs 分区大小");
  eq(L.imageOffset, 0x44000, "image 段在容器里的偏移 = 0x4000 + 0x40000");
  eq(L.maxImageBytes, 8 * 1024 * 1024, "image 段最长 = 分区大小(S3 = 8MB)");
  eq(L.containerMin, 0x44000, "容器最少 0x44000 字节(theme + spiffs 两段)");
  eq(L.containerMax, 0x44000 + 8 * 1024 * 1024, "容器最多 = 0x44000 + 8MB");
  eq(L.betweenLabels.join(","), "spiffs", "中间夹着的分区是 spiffs");
  ok(AP.describeLayout(L).indexOf("0x44000") > 0, "describeLayout 印出了 image 段偏移");

  // 经典板:同一套代码换一张表,数字跟着表走(这里也顺便证明没写死)
  const c = AP.loadPartitionTable({ table: CLASSIC });
  const L2 = AP.computeLayout(c);
  eq(L2.base, 0x210000, "经典板的 theme 偏移(两张表刻意相同)");
  eq(L2.imageOffset, 0x44000, "经典板的 image 段偏移也相同");
  eq(L2.maxImageBytes, 1024 * 1024, "经典板的 image 分区只有 1MB");

  // 默认(--table 与 --target 都不给)走 image-blob-build.js 的默认目标板
  const d = AP.loadPartitionTable({});
  eq(d.csv, IB.targetInfo(IB.DEFAULT_TARGET).partitionsCsv, "默认按目标板取分区表");
  eq(AP.targetIdForCsv("partitions-s3.csv"), "s3", "csv 名字 → 目标板(取 TARGETS,不另抄)");
  eq(AP.targetIdForCsv("no-such.csv"), null, "不认识的 csv 名字返回 null");
}

// ============================================================
section("布局:分区表不连续 / 对不上时必须报错(而不是照摆)");
{
  const fake = (text) => ({ file: T("fake.csv"), csv: "fake.csv",
                            parts: AP.parsePartitionCsv(text, "fake.csv") });
  const head = "nvs, data, nvs, 0x9000, 0x5000,\n";

  // theme 与 image 之间凭空多出 0x1000 的空隙
  const gap = fake(head +
    "theme, data, 0x40, 0x210000, 0x4000,\n" +
    "spiffs, data, spiffs, 0x214000, 0x40000,\n" +
    "image, data, 0x41, 0x255000, 0x100000,\n");
  throws(() => AP.computeLayout(gap), "theme 与 image 之间有空隙 ⇒ 必须报错", "不连续");

  // 顺序反过来
  const rev = fake(head +
    "image, data, 0x41, 0x210000, 0x100000,\n" +
    "theme, data, 0x40, 0x220000, 0x4000,\n");
  throws(() => AP.computeLayout(rev), "image 在 theme 前面 ⇒ 必须报错", "不在 theme");

  // theme 分区不是 0x4000 ⇒ 布局会算错,不能悄悄按 16KB 摆
  const small = fake(head +
    "theme, data, 0x40, 0x210000, 0x1000,\n" +
    "spiffs, data, spiffs, 0x211000, 0x1000,\n" +
    "image, data, 0x41, 0x212000, 0x100000,\n");
  throws(() => AP.computeLayout(small), "theme 分区不是 0x4000 ⇒ 必须报错", "0x4000");

  // 中间那块没有名字也该算得出来(不能靠"叫 spiffs"来认)
  const unnamed = fake(head +
    "theme, data, 0x40, 0x210000, 0x4000,\n" +
    "spare, data, spiffs, 0x214000, 0x40000,\n" +
    "image, data, 0x41, 0x254000, 0x100000,\n");
  eq(AP.computeLayout(unnamed).imageOffset, 0x44000,
     "中间那块换名字也不影响布局(按偏移算,不按名字认)");
}

// ============================================================
section("分区表解析:坏行要报出来,注释与空行要跳过");
{
  const bad = "theme, data, 0x40, 210000, 0x4000,\n";   // 缺 0x 前缀
  throws(() => AP.parsePartitionCsv(bad, "x.csv"), "offset 不写 0x ⇒ 必须报错", "第 1 行");

  const dup = "theme, data, 0x40, 0x210000, 0x4000,\n" +
              "theme, data, 0x40, 0x310000, 0x4000,\n";
  throws(() => AP.parsePartitionCsv(dup, "x.csv"), "同名分区两块 ⇒ 必须报错", "两块");

  const p = AP.parsePartitionCsv(
    "# 注释\n\nspiffs,   data, spiffs,  0x214000, 0x40000,\n" +
    "theme,    data, 0x40,    0x210000, 0x4000,\n", "x.csv");
  eq(p.length, 2, "注释行与空行不算分区");
  eq(p[0].name, "theme", "按偏移排好序(表里 spiffs 写在前面)");
  eq(p[0].size, 0x4000, "size 解析成数字");
  eq(p[1].sizeText, "0x40000", "原文也留着(报错时给人看)");

  throws(() => AP.findPartition(AP.loadPartitionTable({ table: CLASSIC }), "system"),
         "表里没有这块分区 ⇒ 必须报错", "找不到");
  throws(() => AP.loadPartitionTable({ table: "partitions-nope.csv" }),
         "分区表文件不存在 ⇒ 必须报错", "找不到分区表");
  throws(() => AP.loadPartitionTable({ target: "esp32c3" }),
         "不认识的目标板 ⇒ 必须报错");
}

// ============================================================
section("theme 内容:长度靠「末尾 0 填充」,格式靠 JSON 合法性");
{
  const t = AP.loadPartitionTable({ table: S3 });
  const L = AP.computeLayout(t);

  const buf = makeThemeJson();
  ok(AP.checkThemeContent(buf, "x") !== null, "合法主题能过(返回解析出来的对象)");

  // 前面有内容、后面全是 0 ⇒ 长度 = 最后一个非 0 字节 + 1
  const sec = Buffer.alloc(0x4000, 0);
  buf.copy(sec, 0);
  eq(AP.themeContentLength(sec, "x"), buf.length, "theme 内容长度 = 最后一个非 0 字节 + 1");
  eq(AP.findNonZero(sec, buf.length), -1, "之后确实全是 0(没有夹生内容)");
  sec[0x3FFF] = 1;
  eq(AP.findNonZero(sec, buf.length), 0x3FFF, "段尾夹了非 0 内容 ⇒ 找得出来");
  eq(AP.themeContentLength(sec, "x"), 0x4000, "夹生内容会被算进长度(所以要在别处报错)");

  const empty = Buffer.alloc(0x4000, 0);
  throws(() => AP.themeContentLength(empty, "x"), "整段全 0 ⇒ 必须报错", "全是 0");

  throws(() => AP.checkThemeContent(Buffer.from("   \n", "utf8"), "x"),
         "只有空白 ⇒ 必须报错", "空白");
  throws(() => AP.checkThemeContent(Buffer.from("{ not json", "utf8"), "x"),
         "不是 JSON ⇒ 必须报错");
  throws(() => AP.checkThemeContent(Buffer.from("[1,2,3]", "utf8"), "x"),
         "根是数组 ⇒ 必须报错", "根不是对象");
  throws(() => AP.checkThemeContent(Buffer.from('{"theme":{}}' + "\n", "utf8"), "x"),
         "末尾有换行 ⇒ 必须报错(否则往返不再逐字节相同)", "空白字符");
  throws(() => AP.checkThemeContent(Buffer.from('{"theme":{}} trailing', "utf8"), "x"),
         "收尾不是括号 ⇒ 必须报错", "收尾括号");
  ok(AP.checkThemeContent(Buffer.from('{"theme":{"bg_color":0x1234}}', "utf8"), "x").theme.bg_color === 0x1234,
     "0x 十六进制与编辑器同一套规则(theme-json.js)");
  ok(AP.checkThemeContent(Buffer.from('{"bg_color":1}', "utf8"), "x").bg_color === 1,
     "裸根对象也接受(与固件同一套规则)");
}

// ============================================================
section("image 段:长度读它自己的包头,不靠猜");
{
  const t = AP.loadPartitionTable({ table: S3 });
  const L = AP.computeLayout(t);
  const bin = makeImageBin();
  const info = AP.imageBlobLength(bin, L, "x");

  eq(info.headerBytes, IB.HEADER_SIZE, "包头 = 12 + 32×44 = 1420 字节");
  eq(info.total, bin.length, "算出来的长度 = 文件长度");
  eq(info.count, 2, "图片数量从包头读");
  eq(info.dataBytes, bin.length - IB.HEADER_SIZE, "数据长度从包头读");

  throws(() => AP.imageBlobLength(bin.subarray(0, 100), L, "x"),
         "连包头都不够 ⇒ 必须报错", "包头");
  const badMagic = Buffer.from(bin); badMagic[0] = 0xFF;
  throws(() => AP.imageBlobLength(badMagic, L, "x"),
         "魔数不对 ⇒ 必须报错", "魔数");
  const badVer = Buffer.from(bin); badVer[4] = 0x09;
  throws(() => AP.imageBlobLength(badVer, L, "x"),
         "版本不对 ⇒ 必须报错", "版本");
  const badCount = Buffer.from(bin); badCount[6] = 0xFF; badCount[7] = 0xFF;
  throws(() => AP.imageBlobLength(badCount, L, "x"),
         "数量超上限 ⇒ 必须报错", "上限");

  // 包头说 9MB,而分区只有 1MB(经典板)⇒ 必须报错,不能按文件实际长度蒙过去
  const L1 = AP.computeLayout(AP.loadPartitionTable({ table: CLASSIC }));
  const big = Buffer.alloc(IB.HEADER_SIZE + 64);
  big.writeUInt32LE(IB.MAGIC, 0); big.writeUInt16LE(IB.VERSION, 4);
  big.writeUInt16LE(1, 6); big.writeUInt32LE(9 * 1024 * 1024, 8);
  throws(() => AP.imageBlobLength(big, L1, "x"),
         "清单超过分区 ⇒ 必须报错", "超过 image 分区");
}

// ============================================================
section("pack → unpack:往返逐字节一致(自造输入)");
{
  const themeFile = T("t1.json"), imageFile = T("i1.bin"), out = T("a1.bin");
  fs.writeFileSync(themeFile, makeThemeJson());
  fs.writeFileSync(imageFile, makeImageBin());

  const r = AP.pack({ themeFile: themeFile, imageFile: imageFile, outFile: out, table: S3 });
  eq(r.totalLen, 0x44000 + IMAGE_LEN, "容器长度 = 0x44000 + image 长度");
  eq(fs.statSync(out).size, r.totalLen, "落盘的容器长度与返回值一致");
  eq(r.themeBytes, THEME_LEN, "theme 段内容长度");
  eq(r.imageBytes, IMAGE_LEN, "image 段长度");

  const c = fs.readFileSync(out);
  ok(c.subarray(0, THEME_LEN).equals(fs.readFileSync(themeFile)), "容器 +0 就是 theme 内容");
  let allZero = true;
  for (let i = THEME_LEN; i < 0x44000; i++) if (c[i] !== 0) { allZero = false; break; }
  ok(allZero, "theme 填充 + spiffs 段(到 0x44000)全是 0");
  ok(c.subarray(0x44000).equals(fs.readFileSync(imageFile)), "容器 +0x44000 就是 image 内容");

  const themeOut = T("t1.out.json"), imageOut = T("i1.out.bin");
  const u = AP.unpack({ inFile: out, themeOut: themeOut, imageOut: imageOut, table: S3 });
  ok(fs.readFileSync(themeOut).equals(fs.readFileSync(themeFile)), "unpack 的 theme 逐字节相同");
  ok(fs.readFileSync(imageOut).equals(fs.readFileSync(imageFile)), "unpack 的 image 逐字节相同");
  eq(u.themeBytes, THEME_LEN, "拆出来的 theme 长度");
  eq(u.imageBytes, IMAGE_LEN, "拆出来的 image 长度");
  eq(u.containerBytes, r.totalLen, "容器长度对得上");
  // 只读:pack/unpack 不许动输入文件(这里用"内容逐字节相同"判)
  ok(fs.readFileSync(themeFile).equals(makeThemeJson()), "输入 theme 文件没被写过");
  ok(fs.readFileSync(imageFile).equals(makeImageBin()), "输入 image 文件没被写过");

  // 刷写命令:一条命令覆盖两块分区,端口与 chip 都对
  ok(r.flashCommand.indexOf("write_flash 0x210000") > 0, "刷写偏移 = theme 分区偏移(一条命令)");
  ok(r.flashCommand.indexOf("--chip esp32s3") > 0, "S3 板用 --chip esp32s3");
  ok(r.flashCommand.indexOf("--port COM6") > 0, "默认端口 COM6(要换成自己的)");
  ok(r.flashCommand.indexOf("0x254000") < 0, "★ 容器只写 0x210000 —— 不能再出现第二条偏移");
  ok(AP.flashCommand(AP.computeLayout(AP.loadPartitionTable({ table: CLASSIC })),
                     "a.bin", { csv: "partitions.csv" }).indexOf("--chip esp32") > 0,
     "经典板用 --chip esp32");
}

// ============================================================
section("unpack:容器不对时宁可报错,不猜");
{
  const good = T("a1.bin");
  const tOut = T("x.json"), iOut = T("x.bin");
  const U = (f) => AP.unpack({ inFile: f, themeOut: tOut, imageOut: iOut, table: S3 });

  // 随机字节当容器(大小刚好 = 真容器):theme 段整段被填满
  const rnd = require("crypto").randomBytes(fs.statSync(good).size);
  if (rnd[0x3FFF] === 0) rnd[0x3FFF] = 7;      // 保证段尾非 0,测的就是那条
  fs.writeFileSync(T("rnd.bin"), rnd);
  throws(() => U(T("rnd.bin")), "随机字节当容器 ⇒ 必须报错", "不是本工具产出的容器");

  // 太短(连 theme + 中间分区两段都不够)
  throws(() => U(T("theme.json")), "拿 theme.json 当容器 ⇒ 必须报错", "这不是本工具产出的容器");

  // 造一份"theme 段合法、image 段是垃圾"的容器 ⇒ 必须报错(不能只看 theme 就放行)
  const badImg = Buffer.alloc(0x44000 + 4096, 0xAB);
  makeThemeJson().copy(badImg, 0);
  for (let i = THEME_LEN; i < 0x44000; i++) badImg[i] = 0;
  fs.writeFileSync(T("badimage.bin"), badImg);
  throws(() => U(T("badimage.bin")), "image 段是垃圾 ⇒ 必须报错", "魔数");

  // 把真容器改坏:主题内容**后面**混进一个非 0 字节(内容本来是 JSON,填充本该是 0)。
  // ★ 这种坏法报的是"内容的最后一个字节不是 JSON 的收尾括号"(长度会一直算到垃圾末尾),
  //   所以断言的是"多出 N 个字节 / 混进了非 0 垃圾"这句人话,而不是内部实现细节。
  const clipped = Buffer.from(fs.readFileSync(good));
  const clipAt = THEME_LEN + 100;
  clipped[clipAt] = 0x7B;
  fs.writeFileSync(T("clip.bin"), clipped);
  throws(() => U(T("clip.bin")), "主题内容后混进非 0 字节 ⇒ 必须报错", "混进了非 0 垃圾");

  // 把真容器改坏:image 包头里的 dataBytes 少写 1 ⇒ 与容器实际长度不一致
  const short = Buffer.from(fs.readFileSync(good));
  short.writeUInt32LE(short.readUInt32LE(0x44000 + 8) - 1, 0x44000 + 8);
  fs.writeFileSync(T("short.bin"), short);
  throws(() => U(T("short.bin")), "image 清单与实际长度不一致 ⇒ 必须报错", "不一致");

  // 把真容器改坏:image 魔数没了
  const nomagic = Buffer.from(fs.readFileSync(good));
  nomagic[0x44000] = 0x00;
  fs.writeFileSync(T("nomagic.bin"), nomagic);
  throws(() => U(T("nomagic.bin")), "image 段魔数不对 ⇒ 必须报错", "魔数");

  // 拿经典板(1MB image 分区)的表去拆 S3 的容器:
  // 这份容器在 S3 上完全合法(2MB < 8MB),但经典板的 image 分区只有 1MB ⇒ 必须报错
  const big = T("big.bin");
  const bigImage = Buffer.alloc(IB.HEADER_SIZE + 2 * 1024 * 1024);
  bigImage.writeUInt32LE(IB.MAGIC, 0); bigImage.writeUInt16LE(IB.VERSION, 4);
  bigImage.writeUInt16LE(1, 6); bigImage.writeUInt32LE(2 * 1024 * 1024, 8);
  fs.writeFileSync(T("big-image.bin"), bigImage);
  const packedBig = AP.pack({ themeFile: T("theme.json"), imageFile: T("big-image.bin"),
                              outFile: big, table: S3 });
  eq(packedBig.imageBytes, IB.HEADER_SIZE + 2 * 1024 * 1024, "2MB 的 image 在 S3 上打得出来");
  throws(() => AP.unpack({ inFile: big, themeOut: tOut, imageOut: iOut, table: CLASSIC }),
         "拿错分区表拆容器 ⇒ 必须报错(不能装没事)", "超过");
}

// ============================================================
section("pack:输入不对时宁可报错,不猜");
{
  const out = T("bad.bin");
  const P = (themeFile, imageFile, table) =>
    AP.pack({ themeFile: themeFile, imageFile: imageFile, outFile: out, table: table || S3 });

  // theme 超过固件能读进来的 4095 字节(theme_load.cpp 的静态缓冲)
  const huge = T("huge.json");
  fs.writeFileSync(huge, makeThemeJson("x".repeat(6000)));
  throws(() => P(huge, T("image.bin")), "theme 超固件上限 ⇒ 必须报错", "4095");

  // theme 超过 theme 段(= 分区大小)。★ 实际分区里 theme 段是 0x4000(16384),
  //   而固件只读 4095 ⇒ 超大主题先撞上固件那条(更贴近后果),所以这里只断言
  //   "报错里说清了上限是多少",两条分支哪个先响都算过。
  const overSection = T("oversec.json");
  fs.writeFileSync(overSection, makeThemeJson("y".repeat(0x4000)));
  const msgOver = throws(() => P(overSection, T("image.bin")), "theme 超段长 ⇒ 必须报错");
  ok(msgOver !== null && (msgOver.indexOf("4095") >= 0 || msgOver.indexOf("超过 theme 分区") >= 0),
     "报错说清了上限是多少(4095 或 theme 分区大小)");

  // image 不是 image.bin
  const junk = T("junk.bin");
  fs.writeFileSync(junk, Buffer.alloc(2048, 0xAB));
  throws(() => P(T("theme.json"), junk), "拿垃圾当 image.bin ⇒ 必须报错", "魔数");

  // image 长度与自己的包头不一致(尾部被削掉一块)
  const cut = T("cut.bin");
  fs.writeFileSync(cut, makeImageBin().subarray(0, IMAGE_LEN - 8));
  throws(() => P(T("theme.json"), cut), "image 被削短 ⇒ 必须报错", "不一致");

  // image 超过分区(经典板 1MB)
  const bigImage = T("too-big.bin");
  const b = Buffer.alloc(IB.HEADER_SIZE + 1024);
  b.writeUInt32LE(IB.MAGIC, 0); b.writeUInt16LE(IB.VERSION, 4);
  b.writeUInt16LE(1, 6); b.writeUInt32LE(1024 * 1024 + 1, 8);
  fs.writeFileSync(bigImage, b);
  throws(() => P(T("theme.json"), bigImage, CLASSIC), "image 超分区 ⇒ 必须报错", "超过 image 分区");

  // 输入文件不存在 ⇒ node 的 IO 错也要变成人话(统一在 CLI 那层兜)
  throws(() => P(T("nope.json"), T("image.bin")), "theme 文件不存在 ⇒ 必须报错");
  throws(() => P(T("theme.json"), T("nope.bin")), "image 文件不存在 ⇒ 必须报错");
}

// ============================================================
section("命令行:pack / unpack 真跑一遍(子进程)");
{
  const themeFile = T("c-theme.json"), imageFile = T("c-image.bin");
  const out = T("c-assets.bin"), tOut = T("c-theme.out.json"), iOut = T("c-image.out.bin");
  fs.writeFileSync(themeFile, makeThemeJson());
  fs.writeFileSync(imageFile, makeImageBin());

  const run = (args) => {
    try {
      // stderr 也收进管道:子进程的报错不该混进本测试的输出里(输出干净才看得出结论)
      const stdout = execFileSync(process.execPath, [path.join(__dirname, "asset-package.js")].concat(args),
                                  { encoding: "utf8", cwd: ROOT, stdio: ["ignore", "pipe", "pipe"] });
      return { code: 0, out: stdout };
    } catch (e) {
      return { code: e.status === undefined ? -1 : e.status,
               out: String(e.stdout || "") + String(e.stderr || "") };
    }
  };

  const p = run(["pack", "--theme", themeFile, "--image", imageFile, "--out", out, "--table", "partitions-s3.csv"]);
  eq(p.code, 0, "pack 退出码 0");
  ok(p.out.indexOf("0x210000") > 0, "pack 印出了容器基准 0x210000");
  ok(p.out.indexOf("write_flash 0x210000") > 0, "pack 印出了一条刷写命令");
  ok(fs.existsSync(out), "容器文件真的写出来了");

  const u = run(["unpack", "--in", out, "--theme-out", tOut, "--image-out", iOut, "--table", "partitions-s3.csv"]);
  eq(u.code, 0, "unpack 退出码 0");
  ok(fs.readFileSync(tOut).equals(fs.readFileSync(themeFile)), "命令行往返:theme 逐字节相同");
  ok(fs.readFileSync(iOut).equals(fs.readFileSync(imageFile)), "命令行往返:image 逐字节相同");

  const bad = run(["unpack", "--in", themeFile, "--theme-out", tOut, "--image-out", iOut, "--table", "partitions-s3.csv"]);
  eq(bad.code, 1, "拿错文件 ⇒ 退出码 1");
  ok(bad.out.indexOf("失败:") === 0, "报错以「失败:」开头,人一眼能认出");

  eq(run([]).code, 2, "不给子命令 ⇒ 退出码 2(用法错)");
  eq(run(["bogus"]).code, 2, "不认识的子命令 ⇒ 退出码 2");
  eq(run(["pack", "--theme", themeFile]).code, 2, "pack 少参数 ⇒ 退出码 2");
  eq(run(["--help"]).code, 0, "--help 退出码 0");
  ok(run(["--help"]).out.indexOf("pack") > 0, "--help 印出了用法");
}

// ============================================================
section("兼容性:老的「两份文件、两条 write_flash」完全不受影响");
{
  // 这个工具只**读** theme.json / image.bin,不产出它们、也不改它们;
  // 老办法刷的两个偏移必须仍然与分区表一致(不合并也能刷)。
  const t = AP.loadPartitionTable({ table: S3 });
  const L = AP.computeLayout(t);
  eq(L.themePart.offsetText, "0x210000", "老办法:write_flash 0x210000 theme.json 仍然有效");
  eq(L.imagePart.offsetText, "0x254000", "老办法:write_flash 0x254000 image.bin 仍然有效");
  eq(AP.THEME_SECTION_BYTES, 0x4000, "theme 段长 = 分区表里的 theme 大小");
  eq(AP.THEME_FIRMWARE_BYTES, 4095, "固件能读的主题上限 = THEME_MAX_BYTES - 1");
}

// ------------------------------------------------------------
console.log("\n" + "=".repeat(56));
if (fail === 0) {
  console.log("全部通过:" + pass + " 项断言");
  try { fs.rmSync(tmp, { recursive: true, force: true }); } catch (e) { /* 清理失败不影响结论 */ }
} else {
  console.log("失败 " + fail + " 项 / 共 " + (pass + fail) + " 项:");
  failures.forEach(f => console.log("  - " + f));
  console.log("临时文件留在 " + tmp + "(便于查)");
}
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
