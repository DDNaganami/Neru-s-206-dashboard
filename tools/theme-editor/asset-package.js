/* ============================================================
 * asset-package.js —— 把「配色(theme.json) + 图片(image.bin)」打成一个
 *                     可以直接 write_flash 的文件
 *
 *   node tools/theme-editor/asset-package.js pack   --theme theme.json --image image.bin --out assets.bin
 *   node tools/theme-editor/asset-package.js unpack --in assets.bin --theme-out theme.json --image-out image.bin
 *
 * 车主的需求(原话):
 *   "theme 和 image 这两个 bin 我觉得最好导出导入的文件可以合并,
 *    这样每次只需要拷贝一个文件就完事了。"
 * ⇒ 一个文件里同时装着配色与图片,拷一次、刷一次。
 *
 * ============================================================
 * 这个文件现在只做**命令行/文件**那一半
 * ============================================================
 * 纯字节部分(布局怎么算、段多长、什么时候报错、报错怎么说)全部搬到了
 * **asset-package-core.js**,页面(②图片页)与这里**共用那一份**。
 * 搬家的原因、以及"为什么两边不许各写一份",写在那个文件的头注释里。
 *
 * 这里剩下的只有这些"必须有文件系统才成立"的事:
 *   · 读/写文件(readFileSync / writeFileSync)
 *   · 分区表按仓库根或路径找(ROOT / path)
 *   · 打印给人看的命令行输出、退出码
 *   · Buffer ↔ Uint8Array 的互转(Buffer 是 Uint8Array 的子类,直接传即可)
 *
 * ★ 页面上那个按钮与这里跑的是**同一段字节逻辑**:同一个 packBytes()。
 *   测试 tools/theme-editor/test-asset-package-core.js 里有一条
 *   "同样两份输入,CLI 产物与(页面那半也在用的)内核产物逐字节相同"盯着它。
 *
 * ============================================================
 * 一、为什么"一个文件"能成立 —— 两段是**连续**的
 * ============================================================
 * 固件里这是**两块独立分区**(partitions-s3.csv,与 partitions.csv 同值):
 *
 *      theme,    data, 0x40,    0x210000, 0x4000,      ← 配色
 *      spiffs,   data, spiffs,  0x214000, 0x40000,     ← 本项目不用文件系统
 *      image,    data, 0x41,    0x254000, 0x800000,    ← 图片
 *
 * 0x210000 + 0x4000 = 0x214000(theme 与 spiffs 首尾相接)
 * 0x214000 + 0x40000 = 0x254000(spiffs 与 image 首尾相接)
 * ⇒ 从 theme 的偏移开始,**三块分区的字节在 flash 上是一条连续的带子**。
 *   于是"一个文件"不是把两份数据塞进一个壳,而是**这一段 flash 的原样拷贝**:
 *   文件本身就是镜像,可以直接
 *
 *      python -m esptool --chip esp32s3 --port COM6 --baud 921600 \
 *             write_flash 0x210000 assets.bin
 *
 *   一条命令刷完 theme 与 image 两块分区(esptool 只写文件覆盖的那些字节,
 *   文件里没有的地址一个都不碰 —— 所以它**不会**动 app0/app1/nvs)。
 *
 * ★ 所以容器**不加任何自定义文件头**:加了就不能直接 write_flash 了
 *   (文件开头必须是"要写进 0x210000 的第一个字节"),那正是这个工具要避免的。
 *   自描述能力由**分区表 + 段内容自身**提供(见第三节)。
 *
 * ★ 偏移**一个都不写死**:全部从分区表读。分区表里那两处偏移是**刻意保持一致**的
 *   (partitions-s3.csv 的文件头注释:"THEME / IMAGE OFFSETS ARE DELIBERATELY
 *   IDENTICAL TO partitions.csv … Keeping them equal means ONE set of flash
 *   commands works on both boards"),本文件只是把这条约定用机器再确认一次:
 *   表变了,这里跟着变,不需要改代码。
 *
 * ★ 固件与分区表**一个字都不用改**:合并只是"打包与刷写的便利",
 *   内容仍然落在原来那两块分区上,设备端读的还是同样的地方
 *   (src/theme_load.cpp 读 theme 分区;lib/themetool/image_blob.cpp mmap image 分区)。
 *
 * ============================================================
 * 二、容器布局与自描述 —— 见 asset-package-core.js
 * ============================================================
 *   +0x00000   theme.json 的内容,不足 theme 分区大小处补 **0x00**
 *   (中间)     theme 与 image 之间那一整块分区(本项目 = spiffs),全部 0x00
 *   +0x44000   image.bin 的内容(长度 = image 自己包头里写的那个数)
 *
 * 为什么补 0x00 而不是 0xFF(设备端 src/theme_load.cpp 两种终止符都认,
 * 但布局上必须填满)、spiffs 那段为什么可以整块填 0(partitions.csv 原话:
 * "This project uses no filesystem at all")、以及 unpack 怎么只凭
 * **分区表 + 内容自身**就知道每段多长(theme 段 = 最后一个非 0 字节 + 1;
 * image 段 = 读它自己的包头 data_bytes;两者与实际长度不一致就报错、不猜)
 * —— 完整依据与那些"宁可报错不猜"的理由都在
 * **asset-package-core.js** 里(computeLayout / themeContentLength /
 * assertZeroPadding / checkThemeContent / parseImageBlob),ASSET-PACKAGE.md
 * 第 2~3 节是给人看的同一份说明。
 *
 * ★ 两份输入只读:pack/unpack 都不写 --theme/--image/--in 指向的文件。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");
const IB = require("./image-blob-build.js");       // 魔数/版本/项大小/分区大小的唯一权威
const Core = require("./asset-package-core.js");   // ★ 纯字节内核:页面与命令行共用这一份

const ROOT = path.resolve(__dirname, "..", "..");

// ★ 这两个数(以及所有布局/校验逻辑)的**唯一实现**在 asset-package-core.js。
//   这里原样转出去,是为了不让已经 require 本文件的人(单测、别处的脚本)改一行代码。
const THEME_SECTION_BYTES = Core.THEME_SECTION_BYTES;
const THEME_FIRMWARE_BYTES = Core.THEME_FIRMWARE_BYTES;

const PARTITION_LABEL_THEME = "theme";
const PARTITION_LABEL_IMAGE = "image";

function hex(n) { return Core.hex(n); }

// 内核要按 CLI 那份权威常量对账(见 core 的 imageConstants),所以每次都传进去
function coreOpts(extra) {
  return Object.assign({ ib: IB }, extra || {});
}

function parsePartitionCsv(text, file) {
  return Core.parsePartitionCsv(text, file);
}

function partitionTablePath(csvName) {
  const p = /[\\/]/.test(csvName) || path.isAbsolute(csvName)
    ? csvName                                  // 给了路径就按路径走
    : path.join(ROOT, csvName);                // 只给文件名 ⇒ 按仓库根找(与 TARGETS 里的写法一致)
  if (!fs.existsSync(p)) {
    throw new Error("找不到分区表 " + p +
                    "。--table 可以给文件名(partitions.csv / partitions-s3.csv,按仓库根找)" +
                    "或给完整路径;也可以改用它对应的目标板名字(--target classic|s3|s3_240)");
  }
  return p;
}

// csv 名字 → 目标板 id(拿 IB.TARGETS 里现有的对应关系,不另抄一张表)
function targetIdForCsv(csvName) {
  for (const id of Object.keys(IB.TARGETS)) {
    if (IB.TARGETS[id].partitionsCsv === csvName) return id;
  }
  return null;
}

function loadPartitionTable(opts) {
  opts = opts || {};
  let csvName = opts.table;
  if (!csvName) {
    // 没给 --table 就按目标板走(默认板 = s3,与编辑器一致)
    const targetId = opts.target || IB.DEFAULT_TARGET;
    csvName = IB.targetInfo(targetId).partitionsCsv;   // 未知目标板会在里面抛错
  }
  const p = partitionTablePath(csvName);
  return {
    file: p,
    csv: path.basename(p),
    parts: Core.parsePartitionCsv(fs.readFileSync(p, "utf8"), path.basename(p))
  };
}

// ------------------------------------------------------------
// 以下四个都是 core 的转出。留着它们是为了"调用点一行都不用改",
// 同时也让 test-asset-package.js 里那批 AP.computeLayout(...) 继续有效。
// ★ 它们**没有**自己的实现:一行委托,分叉不了。
// ------------------------------------------------------------
function findPartition(table, name) { return Core.findPartition(table, name); }
function computeLayout(table) { return Core.computeLayout(table); }
function describeLayout(layout) { return Core.describeLayout(layout); }
function themeContentLength(themeSection, where) { return Core.themeContentLength(themeSection, where); }
function findNonZero(buf, from) { return Core.findNonZero(buf, from); }

function checkThemeContent(buf, label) {
  return Core.checkThemeContent(Buffer.isBuffer(buf) ? buf : Buffer.from(buf), label);
}

// parseImageBlob:CLI 侧也暴露一个(页面里直接用 core 那个)。
// 传 Buffer 进去没问题 —— Buffer 就是 Uint8Array。
function parseImageBlob(buf, opts) {
  return Core.parseImageBlob(buf, coreOpts(opts));
}

function imageBlobLength(buf, layout, label) {
  return Core.imageBlobLength(buf, layout, label);
}

// ------------------------------------------------------------
// pack:两份输入文件 → 一个容器文件
//   ★ 校验与拼字节全在 Core.packBytes 里;**报错文字一个字都没改**,
//     只是标签从"theme 文件 X"改成 CLI 那套带文件名的写法。
// ------------------------------------------------------------
function pack(opts) {
  const { themeFile, imageFile, outFile } = opts;
  const table = loadPartitionTable(opts);
  const layout = Core.computeLayout(table);

  const themeBuf = fs.readFileSync(themeFile);
  const imageBuf = fs.readFileSync(imageFile);

  const r = Core.packBytes(coreOpts({
    themeBytes: themeBuf, imageBytes: imageBuf,
    layout: layout, table: table,
    themeLabel: "theme 文件 " + themeFile,
    imageLabel: "image 文件 " + imageFile
  }));

  if (outFile) fs.writeFileSync(outFile, Buffer.from(r.bytes));

  return {
    layout: layout, table: table, buf: Buffer.from(r.bytes), totalLen: r.totalLen,
    themeBytes: r.themeBytes, imageBytes: r.imageBytes,
    imageInfo: r.imageInfo,
    flashCommand: flashCommand(layout, outFile ? path.basename(outFile) : "assets.bin", table)
  };
}

// ------------------------------------------------------------
// unpack:容器文件 → 两份原样的文件
// ------------------------------------------------------------
function unpack(opts) {
  const { inFile, themeOut, imageOut } = opts;
  const table = loadPartitionTable(opts);
  const layout = Core.computeLayout(table);

  const buf = fs.readFileSync(inFile);

  const r = Core.unpackBytes(coreOpts({
    bytes: buf, layout: layout, table: table,
    containerLabel: inFile,
    themeLabel: inFile + " 的 theme 段",
    imageLabel: inFile + " 的 image 段"
  }));

  const themeBuf = Buffer.from(r.themeBytes);
  const imageBuf = Buffer.from(r.imageBytes);
  if (themeOut) fs.writeFileSync(themeOut, themeBuf);
  if (imageOut) fs.writeFileSync(imageOut, imageBuf);

  return {
    layout: layout, table: table,
    themeBuf: themeBuf, imageBuf: imageBuf,
    themeBytes: r.themeBytes.length, imageBytes: r.imageBytes.length,
    imageInfo: r.imageInfo,
    containerBytes: r.containerBytes
  };
}

// ------------------------------------------------------------
// 刷写命令(容器只有**一条**命令 —— 这正是合并的意义)
//   ★ 端口要换成自己的(COM6 是车主这台机器上那块 S3 的口)
//   ★ --chip 跟着目标板走(与 image-blob-build.js 的 esptoolCommand 同一套数据)
//   ★ 偏移从 layout 取(= 分区表里 theme 分区的偏移),不写死
// ------------------------------------------------------------
function flashCommand(layout, binPath, table, port, targetId) {
  const csv = table ? table.csv : null;
  const id = targetId || (csv ? targetIdForCsv(csv) : null);
  const chip = id ? IB.targetInfo(id).chip
                  : (csv === "partitions-s3.csv" ? "esp32s3" : "esp32");
  return Core.flashCommand(layout, binPath, { chip: chip, port: port || "COM6" });
}

// ============================================================
// 命令行
// ============================================================
function parseArgs(argv) {
  let cmd = argv[2];
  // 裸的 --help / -h(argv[2] 就是它)也要认 —— 不然会被当成"不认识的子命令"
  const o = { cmd: cmd, table: null, target: null, port: null, help: false };
  if (cmd === "--help" || cmd === "-h") { o.cmd = null; o.help = true; }
  for (let i = 3; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--theme") o.themeFile = argv[++i];
    else if (a === "--image") o.imageFile = argv[++i];
    else if (a === "--out") o.outFile = argv[++i];
    else if (a === "--in") o.inFile = argv[++i];
    else if (a === "--theme-out") o.themeOut = argv[++i];
    else if (a === "--image-out") o.imageOut = argv[++i];
    else if (a === "--table") o.table = argv[++i];
    else if (a === "--target") o.target = argv[++i];
    else if (a === "--port") o.port = argv[++i];
    else if (a === "--help" || a === "-h") o.help = true;
    else throw new Error("不认识的参数: " + a + "(--help 看用法)");
  }
  return o;
}

function usage() {
  const L = [];
  L.push("把「配色 + 图片」打成一个可以直接刷的文件(或反过来拆开)。");
  L.push("");
  L.push("  node tools/theme-editor/asset-package.js pack \\");
  L.push("       --theme <theme.json> --image <image.bin> --out <assets.bin> [--table partitions-s3.csv]");
  L.push("  node tools/theme-editor/asset-package.js unpack \\");
  L.push("       --in <assets.bin> --theme-out <theme.json> --image-out <image.bin> [--table …]");
  L.push("");
  L.push("--table   分区表(默认按 --target 取,只给文件名时按仓库根找)");
  L.push("--target  目标板,决定用哪份分区表与刷写命令的 --chip(默认 " + IB.DEFAULT_TARGET + "):");
  for (const id of Object.keys(IB.TARGETS)) {
    const t = IB.TARGETS[id];
    L.push("            " + id.padEnd(8) + t.label + "  (" + t.partitionsCsv +
           ", --chip " + t.chip + ")");
  }
  L.push("--port    刷写命令里的串口(默认 COM6 —— 换成自己那块板的口)");
  L.push("");
  L.push("容器 = 从 theme 分区偏移开始的一段连续 flash(不分块刷,见文件头说明)。");
  L.push("同一件事在 ②图片页上也有按钮(导出/导入 assets.bin),两边是同一份内核:");
  L.push("  tools/theme-editor/asset-package-core.js");
  return L.join("\n");
}

function main() {
  const a = parseArgs(process.argv);
  if (a.help) { console.log(usage()); return 0; }
  if (!a.cmd) { console.error(usage()); return 2; }   // 什么都没给:当用法错误

  if (a.cmd === "pack") {
    if (!a.themeFile || !a.imageFile || !a.outFile) {
      console.error("pack 需要 --theme / --image / --out 三个参数(--help 看用法)");
      return 2;
    }
    const r = pack({ themeFile: a.themeFile, imageFile: a.imageFile, outFile: a.outFile,
                     table: a.table, target: a.target, port: a.port });
    const L = r.layout;
    console.log("已生成 " + a.outFile + "  (" + r.totalLen + " 字节)");
    console.log("  分区表 " + r.table.file);
    console.log(describeLayout(L));
    console.log("  theme 段 : " + r.themeBytes + " 字节内容 + " +
                (L.themeSectionBytes - r.themeBytes) + " 字节 0 填充");
    console.log("  image 段 : " + r.imageBytes + " 字节(包头 " + r.imageInfo.headerBytes +
                " + 数据 " + r.imageInfo.dataBytes + ",共 " + r.imageInfo.count + " 张图)");
    console.log("  刷写(端口换成自己的): " + flashCommand(L, path.basename(a.outFile), r.table, a.port));
    return 0;
  }

  if (a.cmd === "unpack") {
    if (!a.inFile || !a.themeOut || !a.imageOut) {
      console.error("unpack 需要 --in / --theme-out / --image-out 三个参数(--help 看用法)");
      return 2;
    }
    const r = unpack({ inFile: a.inFile, themeOut: a.themeOut, imageOut: a.imageOut,
                       table: a.table, target: a.target });
    console.log("已拆开 " + a.inFile + "  (" + r.containerBytes + " 字节)");
    console.log("  " + a.themeOut + "  (" + r.themeBytes + " 字节)");
    console.log("  " + a.imageOut + "  (" + r.imageBytes + " 字节,包头 " +
                r.imageInfo.headerBytes + " + 数据 " + r.imageInfo.dataBytes +
                ",共 " + r.imageInfo.count + " 张图)");
    console.log("  分区表 " + r.table.file + "(容器基准 " + hex(r.layout.base) + ")");
    return 0;
  }

  console.error("不认识的子命令「" + a.cmd + "」,只有 pack / unpack(--help 看用法)");
  return 2;
}

if (require.main === module) {
  try {
    process.exit(main());
  } catch (e) {
    console.error("失败: " + e.message);
    process.exit(1);
  }
}

module.exports = {
  THEME_SECTION_BYTES: THEME_SECTION_BYTES,
  THEME_FIRMWARE_BYTES: THEME_FIRMWARE_BYTES,
  ROOT: ROOT,
  // ★ 纯字节内核(页面与命令行共用的那一份)。页面里是 window.AssetPackageCore。
  Core: Core,
  parsePartitionCsv: parsePartitionCsv,
  loadPartitionTable: loadPartitionTable,
  findPartition: findPartition,
  computeLayout: computeLayout,
  describeLayout: describeLayout,
  themeContentLength: themeContentLength,
  findNonZero: findNonZero,
  checkThemeContent: checkThemeContent,
  parseImageBlob: parseImageBlob,
  imageBlobLength: imageBlobLength,
  pack: pack,
  unpack: unpack,
  flashCommand: flashCommand,
  targetIdForCsv: targetIdForCsv
};
