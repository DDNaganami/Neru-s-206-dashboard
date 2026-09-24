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
 *   自描述能力由**分区表 + 段内容自身**提供,见下面第三节。
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
 * 二、容器布局(基准 = theme 分区的偏移,所以 +0 就是 0x210000)
 * ============================================================
 *   +0x00000   theme.json 的内容,不足 themePartSize 处补 **0x00**
 *   (中间)     spiffs 那一整块,全部 0x00        ← 布局算出来的,不写死 0x40000
 *   +tail      image.bin 的内容(长度 = 文件里 image 段的实际长度)
 *
 * 为什么补 0x00(而不是像 flash 空白那样是 0xFF),两个理由都要说清:
 *   1) **布局**:image 段的起点必须正好落在 image 分区的偏移上,所以 theme
 *      与 spiffs 两段必须填满,不能"写过就算";
 *   2) **设备端两种终止符都认**:src/theme_load.cpp 的读取循环遇到
 *      '\0'(第 41 行)或 0xFF(第 42 行)都停。补 0 走的是第一条,
 *      与"直接刷一份 theme.json"留下的 0xFF 效果等价。
 *
 * spiffs 段为什么可以整块填 0 —— **依据**:
 *   · partitions.csv 的文件头原话:"This project uses no filesystem at all.
 *     spiffs is kept only so the partition table has a spare data slot"
 *     ⇒ 那块分区里没有任何需要有内容的数据;
 *   · 当前分区表里它只有名字叫 spiffs,固件没有挂载任何文件系统
 *     (没有 SPIFFS/LittleFS 挂载点,所以里面是不是合法的文件系统镜像无所谓);
 *   · 它的**大小与偏移仍然从分区表读** —— 换表就自动跟着变。
 *   ★ 边界:如果将来真有人往那块分区放了文件系统,刷这个容器会把它**清成 0**。
 *     那时请改用"两份文件、两条 write_flash"的老办法刷 theme 与 image 两块。
 *
 * ============================================================
 * 三、自描述:unpack 怎么知道每段多长
 * ============================================================
 * 不靠容器带头,靠**内容自身 + 分区表**(两者不一致时报错,不猜):
 *
 *   theme 段:分区末尾的 0 是填充 ⇒ 内容长度 = **最后一个非 0 字节 + 1**。
 *            不变量:填充必须全是 0(有一个非 0 字节在这个位置,说明容器
 *            不是本工具产出的,或者 theme 段被写坏),否则报错。
 *            长度取回后仍要过一遍 JSON 合法性(theme-json.js 的解析器,
 *            与网页/固件同一套规则),不合法就报错。
 *   image 段:长度**优先读 image 自己的清单** —— 头 12 字节的 dataBytes
 *            (与 lib/themetool/image_blob.h 的 ImageBlobHeader 逐字段对应),
 *            总长 = 包头大小 + dataBytes(包头大小按同一份表算出来:
 *            12 + 32×44 = 1420 字节)。绝不用"文件剩下的字节数"当长度。
 *            ★ 一致时:容器必须**正好**这么长(多出来的必须全是 0 填充,
 *              否则报错 —— 这正是"不许悄悄出错"那条)。
 *
 * 两份输入只读:pack/unpack 都不写 --theme/--image/--in 指向的文件。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");
const IB = require("./image-blob-build.js");       // 魔数/版本/项大小/分区大小的唯一权威
const ThemeJson = require("./theme-json.js");      // 主题 JSON 的解析规则(与网页/固件同一套)

const ROOT = path.resolve(__dirname, "..", "..");

// 主题分区里"配色"那一段的长度。
// ★ 等于分区表里 theme 分区的 Size(0x4000)。它是**容器布局的一部分**
//   (image 段的起点由它决定),分区表里 theme 不是 0x4000 就会直接报错,
//   不会悄悄按 16KB 摆。
const THEME_SECTION_BYTES = 16384;

// 固件真正能读进来的主题字节数上限,由 src/theme_load.cpp 决定:
//     static char buf[THEME_MAX_BYTES];                      // = (4 * 1024)
//     esp_partition_read(part, 0, buf, sizeof(buf) - 1);     // 只读 4095 字节
// ⇒ 超过 4095 字节的主题**在设备上会被截断**,而截断的 JSON 解析必然失败,
//   表现是"刷进去了,但屏上是默认配色"——不报错的那种失败。所以这里当硬闸门。
//   ★ 注意它与 theme 分区大小(0x4000)是**两个不同的数**:后者管布局,
//     前者管固件读得进来多少。两者都检查。
//     (THEME_MAX_BYTES 的定义在 lib/themetool/theme_store.h,注释里写明了
//      它"直接决定 DRAM 占用",所以不能为了迁就大主题随手放大。)
const THEME_FIRMWARE_BYTES = 4095;

const PARTITION_LABEL_THEME = "theme";
const PARTITION_LABEL_IMAGE = "image";

function hex(n) { return "0x" + n.toString(16); }

// ------------------------------------------------------------
// 分区表解析(与 lib/themetool/image_blob.h / test-image-blob-build.js
// 用的是同一套列:Name,Type,SubType,Offset,Size,Flags)
//
// 为什么单独写一小段而不是复用测试里的解析:那份在 test-*.js 里、
// 用的是"整行正则 + 要求行尾正好是逗号"的写法,只够读它自己的两张表;
// 这里要按**分区名**取任意一块、还要把解析错误说清楚。CSV 本身是 dead simple,
// 不值得为它建一个共享模块(也没有别人在用)。
// ------------------------------------------------------------
function parsePartitionCsv(text, file) {
  const where = file ? "(" + file + ")" : "";
  const out = [];
  const seen = Object.create(null);
  const lines = String(text).replace(/^\uFEFF/, "").split(/\r?\n/);

  for (let i = 0; i < lines.length; i++) {
    const raw = lines[i];
    const line = raw.trim();
    if (!line || line.charAt(0) === "#") continue;
    if (/^name\s*,\s*type/i.test(line)) continue;   // 表头那行(如果有)

    const c = line.split(",").map(s => s.trim());
    // 两列的行(比如表头残留)直接跳过,不猜
    if (c.length < 5) continue;

    const name = c[0], type = c[1], subtype = c[2], offStr = c[3], sizeStr = c[4];
    if (!/^[A-Za-z0-9_]+$/.test(name)) continue;      // 注释尾巴之类,不当数据
    if (!/^0x[0-9a-fA-F]+$/.test(offStr) || !/^0x[0-9a-fA-F]+$/.test(sizeStr)) {
      // 名字像分区、数字却不对 ⇒ 这是真的写错了,必须报出来(不能跳过)
      throw new Error("分区表" + where + "第 " + (i + 1) + " 行的 offset/size 读不出来:" +
                      "「" + line + "」(要写成 0x… 的十六进制)");
    }
    if (seen[name]) {
      throw new Error("分区表" + where + "里有两块都叫「" + name + "」的分区(第 " +
                      seen[name] + " 行和第 " + (i + 1) + " 行)⇒ 不知道该用哪一块,拒绝继续");
    }
    seen[name] = i + 1;
    out.push({
      name: name, type: type, subtype: subtype,
      offset: parseInt(offStr, 16), offsetText: offStr.toLowerCase(),
      size: parseInt(sizeStr, 16), sizeText: sizeStr.toLowerCase()
    });
  }

  // 按偏移排好,后面算"两段之间夹着什么"时就不用管表里的书写顺序
  out.sort((a, b) => a.offset - b.offset);
  return out;
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
    parts: parsePartitionCsv(fs.readFileSync(p, "utf8"), path.basename(p))
  };
}

function findPartition(table, name) {
  const hit = table.parts.filter(p => p.name === name);
  if (hit.length !== 1) {
    throw new Error("分区表 " + table.csv + " 里" +
                    (hit.length ? "有两块叫「" + name + "」的分区" : "找不到名为「" + name + "」的分区") +
                    "。这张表里的分区是:" + table.parts.map(p => p.name).join(" / ") +
                    " ⇒ 请确认 --table 指向的是这台设备正在用的分区表");
  }
  return hit[0];
}

// ------------------------------------------------------------
// 布局:从分区表算出三段的位置。任何不满足前置条件的地方都**报错**。
// ------------------------------------------------------------
function computeLayout(table) {
  const theme = findPartition(table, PARTITION_LABEL_THEME);
  const image = findPartition(table, PARTITION_LABEL_IMAGE);

  if (theme.size !== THEME_SECTION_BYTES) {
    throw new Error("分区表 " + table.csv + " 里 theme 分区是 " + theme.sizeText +
                    "(" + theme.size + " 字节),本工具按 " + THEME_SECTION_BYTES +
                    " 字节(0x" + THEME_SECTION_BYTES.toString(16) + ")摆布局。" +
                    "两者不一致时容器里 image 段的起点会算错 ⇒ 拒绝继续:" +
                    "要么改回分区表,要么改这里的 THEME_SECTION_BYTES");
  }
  if (image.offset <= theme.offset) {
    throw new Error("分区表 " + table.csv + " 里 image(" + image.offsetText +
                    ")不在 theme(" + theme.offsetText + ")后面 ⇒ 两段不连续,合并不了");
  }

  const between = table.parts
    .filter(p => p.offset > theme.offset && p.offset < image.offset)
    .sort((a, b) => a.offset - b.offset);

  let cursor = theme.offset + theme.size;
  for (const p of between) {
    if (p.offset !== cursor) {
      throw new Error("分区表 " + table.csv + " 里 theme 与 image 之间有空隙或重叠:" +
                      "theme 一块到 " + hex(cursor) + " 就该结束,下一块「" + p.name +
                      "」却从 " + p.offsetText + " 开始" +
                      " ⇒ 合并文件必须是一段连续 flash,空隙会让刷写照错地址");
    }
    cursor += p.size;
  }
  if (cursor !== image.offset) {
    throw new Error("分区表 " + table.csv + " 里 theme 与 image 不连续:theme(+中间的分区)到 " +
                    hex(cursor) + " 结束,而 image 从 " + image.offsetText +
                    " 开始 ⇒ 合并的容器不能直接 write_flash");
  }

  return {
    themePart: theme,
    imagePart: image,
    base: theme.offset,                        // 容器的 +0 对应 flash 的哪个地址
    themeSectionBytes: theme.size,             // theme 那一段的长度(= 分区大小)
    spiffsBytes: between.reduce((s, p) => s + p.size, 0),  // theme 与 image 之间那一整块
    betweenLabels: between.map(p => p.name),
    imageOffset: image.offset - theme.offset,  // image 在容器里的偏移
    maxImageBytes: image.size,                 // image 段最多能写多少(分区大小)
    containerMax: image.offset + image.size - theme.offset,
    containerMin: image.offset - theme.offset
  };
}

function describeLayout(layout) {
  const L = [];
  L.push("  容器基准 = theme 分区偏移 " + hex(layout.base) + "(文件里 +0 就是这里)");
  L.push("  +0x0      theme.json(不足 " + layout.themeSectionBytes + " = " +
         hex(layout.themeSectionBytes) + " 字节处补 0)");
  if (layout.spiffsBytes > 0) {
    L.push("  +0x" + layout.spiffsBytes.toString(16) + "       …中间 " + layout.spiffsBytes +
           " 字节(" + (layout.betweenLabels.join(" / ") || "未命名分区") + ",全 0 填充)");
  }
  L.push("  +0x" + layout.imageOffset.toString(16) + "   image.bin(最长 " +
         layout.maxImageBytes + " 字节 = " + hex(layout.maxImageBytes) + ")");
  return L.join("\n");
}

// ------------------------------------------------------------
// 取 theme 段的内容长度:末尾的 0 是填充 ⇒ 最后一个非 0 字节 + 1
// ------------------------------------------------------------
function themeContentLength(themeSection, where) {
  for (let i = themeSection.length - 1; i >= 0; i--) {
    if (themeSection[i] !== 0) return i + 1;
  }
  throw new Error(where + ":theme 那一段(" + themeSection.length + " 字节)全是 0 —— " +
                  "里面没有配色内容。这不是本工具产出的容器(或者 theme 段被清掉了)");
}

// 从 from 起找第一个非 0 字节,找不到返回 -1
function findNonZero(buf, from) {
  for (let i = from; i < buf.length; i++) if (buf[i] !== 0) return i;
  return -1;
}

// theme 段的 0 填充必须真的全是 0,否则报错并**指出第几个字节**。
// ★ 这条检查必须排在 JSON 解析**之前**:填充里混了垃圾时,内容长度会一直算到垃圾末尾,
//   于是 JSON 解析会报"最后一个字节不是收尾括号"——那是指错方向(真正的原因是填充脏了)。
function assertZeroPadding(themeSection, contentLen, where) {
  const off = findNonZero(themeSection, contentLen);
  if (off < 0) return;
  throw new Error(where + ":theme 段在第 " + off + " 字节(0x" + off.toString(16) +
                  ",内容 " + JSON.stringify(String.fromCharCode(themeSection[off])) +
                  ")之后还有非 0 内容 —— 主题内容本应在第 " + contentLen +
                  " 字节结束、之后全是 0 填充 ⇒ 这不是本工具产出的容器," +
                  "或者 theme 段被写坏了(拒绝猜哪一段才是配色)");
}

// 主题内容是否通过(格式 + 大小 + 收尾)。返回解析出来的对象;失败时抛带人话的错。
function checkThemeContent(buf, label) {
  const text = Buffer.from(buf).toString("utf8");
  const trimmed = text.replace(/\s+$/, "");
  if (!trimmed) {
    throw new Error(label + ":内容只有空白,不是主题 JSON");
  }
  // 收尾必须是 JSON 的右括号:允许的收尾只有这一种,别的都说明是"截断/拼接"
  const last = trimmed.charAt(trimmed.length - 1);
  if (last !== "}" && last !== "]") {
    // 长度算到哪、多出来多少:混进垃圾时长度会一直算到垃圾末尾,
    // 于是"最后一个字节"其实是垃圾 —— 把这两个数一起报出来,原因就看得见了。
    const cIdx = Math.max(trimmed.lastIndexOf("}"), trimmed.lastIndexOf("]"));
    const extra = trimmed.length - cIdx - 1;
    throw new Error(label + ":内容的最后一个字节不是 JSON 的收尾括号(是 " + JSON.stringify(last) +
                    (extra > 0 ? ";最后那个收尾括号在第 " + (cIdx + 1) + " 字节," +
                                 "它后面还多出 " + extra + " 个字节" : "") +
                    ")⇒ 这段不是完整的主题文件:" +
                    "要么 --in 指错了文件,要么 theme 段里混进了非 0 垃圾");
  }
  let obj;
  try {
    obj = ThemeJson.parseThemeJson(trimmed);        // 与网页/固件同一套规则
  } catch (e) {
    // ★ 这条报错要能指向**真正的原因**:取回来的内容不是合法 JSON,最常见的两种是
    //   ① --in 指错了文件;② theme 段里混进了非 0 垃圾(那时"长度"会一直算到垃圾末尾)。
    throw new Error(label + ":主题 JSON 解析失败 —— " + e.message +
                    "(这段是 theme 分区里从 +0 起、到最后一个非 0 字节为止的内容;" +
                    "如果它本来就不是主题文件,说明 --in 指错了文件;" +
                    "如果这个文件是别处弄来的,还要怀疑 theme 段里混进了非 0 垃圾" +
                    " —— 本工具产出的容器里,主题内容之后必须全是 0)");
  }
  if (!ThemeJson.isPlainObject(obj)) {
    throw new Error(label + ":主题 JSON 的根不是对象(是 " +
                    (Array.isArray(obj) ? "数组" : typeof obj) + ")⇒ 不收");
  }
  if (trimmed.length !== text.length) {
    // 末尾有空白:固件无所谓,但**往返就不逐字节相同**了(往返一致是本工具的核心判据),
    // 所以宁可在这里挡下来,也不要让"pack 再 unpack 之后文件变了"这种事发生。
    throw new Error(label + ":末尾有 " + (text.length - trimmed.length) +
                    " 个空白字符(空格/换行)。它们在设备端无所谓,但会让" +
                    "「导出→导入」不再逐字节相同 ⇒ 请去掉文件末尾的空白再打包");
  }
  return obj;
}

// ------------------------------------------------------------
// 取 image 段的长度:读 image 自己的清单(头 12 字节),不猜
// ------------------------------------------------------------
function imageBlobLength(buf, layout, label) {
  if (buf.length < IB.HEADER_SIZE) {
    throw new Error(label + ":只有 " + buf.length + " 字节,连 image 包头(" +
                    IB.HEADER_SIZE + " 字节)都不够 ⇒ 不是一份 image.bin");
  }
  const dv = new DataView(buf.buffer, buf.byteOffset, buf.byteLength);
  const magic = dv.getUint32(0, true);
  const version = dv.getUint16(4, true);
  const count = dv.getUint16(6, true);
  const dataBytes = dv.getUint32(8, true);

  if (magic !== IB.MAGIC || version !== IB.VERSION) {
    throw new Error(label + ":image 包头不对(魔数 0x" + magic.toString(16) +
                    " 期望 0x" + IB.MAGIC.toString(16) + ",版本 " + version +
                    " 期望 " + IB.VERSION + ")⇒ 这段不是 image.bin。" +
                    "常见原因:theme 段与 spiffs 段的长度摆错了(请用 --table 指定正确的分区表)");
  }
  if (count > IB.MAX_COUNT) {
    throw new Error(label + ":image 包头里的图片数量 " + count + " 超过上限 " +
                    IB.MAX_COUNT + " ⇒ 包头已被写坏");
  }
  const total = IB.HEADER_SIZE + dataBytes;
  if (total > layout.maxImageBytes) {
    throw new Error(label + ":按清单算出的长度 " + total + " 字节超过 image 分区(" +
                    layout.maxImageBytes + " 字节 = " + layout.imagePart.sizeText +
                    ")⇒ 这份容器装不下/被截断过。请重新打包(或用 --target 指定分区更大的那块板)");
  }
  return { total: total, headerBytes: IB.HEADER_SIZE, dataBytes: dataBytes, count: count };
}

// ------------------------------------------------------------
// pack:两份输入 → 一个容器
// ------------------------------------------------------------
function pack(opts) {
  const { themeFile, imageFile, outFile } = opts;
  const table = loadPartitionTable(opts);
  const layout = computeLayout(table);

  const themeBuf = fs.readFileSync(themeFile);
  const imageBuf = fs.readFileSync(imageFile);

  // ---- theme 的三种"太大"分别说清楚,不要合成一句 ----
  if (themeBuf.length > THEME_FIRMWARE_BYTES) {
    throw new Error("theme 文件 " + themeFile + " 有 " + themeBuf.length +
                    " 字节,超过固件能读进来的 " + THEME_FIRMWARE_BYTES + " 字节" +
                    "(src/theme_load.cpp 的 buf[THEME_MAX_BYTES];THEME_MAX_BYTES = 4KB," +
                    "只读 sizeof(buf)-1 = 4095 字节)。刷进去会被截断、解析失败," +
                    "结果是「屏上还是默认配色」而且不报错 ⇒ 拒绝打包。" +
                    "主题是纯文本,删掉用不上的字段即可(正常一份约 1.2~2KB)");
  }
  if (themeBuf.length > layout.themeSectionBytes) {
    throw new Error("theme 文件 " + themeFile + " 有 " + themeBuf.length +
                    " 字节,超过 theme 分区 / 段长 " + layout.themeSectionBytes +
                    "(0x" + layout.themeSectionBytes.toString(16) + ")⇒ 装不下");
  }
  checkThemeContent(themeBuf, "theme 文件 " + themeFile);

  // ---- image 段长度由它自己的包头决定 ----
  const info = imageBlobLength(imageBuf, layout, "image 文件 " + imageFile);
  if (info.total !== imageBuf.length) {
    throw new Error("image 文件 " + imageFile + " 是 " + imageBuf.length +
                    " 字节,但它自己的包头写的是 " + info.total + " 字节" +
                    "(包头 " + info.headerBytes + " + 数据 " + info.dataBytes +
                    ",共 " + info.count + " 张图)⇒ 两者不一致,不猜:请重新导出这份 image.bin");
  }
  if (imageBuf.length > layout.maxImageBytes) {
    throw new Error("image 有 " + imageBuf.length + " 字节,超过 image 分区 " +
                    layout.maxImageBytes + " 字节(" + layout.imagePart.sizeText +
                    ")⇒ 刷进去会被截断。请减少/缩小图片,或用 --table 指定分区更大的那块板");
  }

  // ---- 拼容器 ----
  const totalLen = layout.imageOffset + imageBuf.length;
  const out = Buffer.alloc(totalLen, 0);              // 全 0:theme 段尾部与中间那段都是 0
  themeBuf.copy(out, 0);
  imageBuf.copy(out, layout.imageOffset);

  if (outFile) fs.writeFileSync(outFile, out);

  return {
    layout: layout, table: table, buf: out, totalLen: totalLen,
    themeBytes: themeBuf.length, imageBytes: imageBuf.length,
    imageInfo: info,
    flashCommand: flashCommand(layout, outFile ? path.basename(outFile) : "assets.bin", table)
  };
}

// ------------------------------------------------------------
// unpack:容器 → 两份原样的文件
// ------------------------------------------------------------
function unpack(opts) {
  const { inFile, themeOut, imageOut } = opts;
  const table = loadPartitionTable(opts);
  const layout = computeLayout(table);

  const buf = fs.readFileSync(inFile);

  if (buf.length < layout.containerMin) {
    throw new Error(inFile + ":只有 " + buf.length + " 字节,而 theme + 中间分区两段就要 " +
                    layout.containerMin + " 字节(" + "0x" + layout.containerMin.toString(16) +
                    ")⇒ 这不是本工具产出的容器");
  }
  if (buf.length > layout.containerMax) {
    throw new Error(inFile + ":" + buf.length + " 字节超过「theme 段 + 中间分区 + image 分区」的 " +
                    layout.containerMax + " 字节 ⇒ 这不是按 " + table.csv + " 摆出来的容器" +
                    "(是不是拿了另一块板的分区表?)");
  }

  // ---- theme 段 ----
  const themeSection = buf.subarray(0, layout.themeSectionBytes);
  // ★ 先看"段尾有没有 0 填充":内容再长也长不过段长(16384),所以**段尾必须是 0**。
  //   段尾非 0 ⇒ theme 段整个填满了 ⇒ 这份文件不是本工具产出的。
  //   这条必须放在最前面:把它当主题去解析,报错会指向"JSON 不合法 / 超过 4095 字节",
  //   而真正的原因(这压根不是容器)就看不出来了 —— 把随机字节当容器时正是这种情况。
  if (themeSection[themeSection.length - 1] !== 0) {
    throw new Error(inFile + ":theme 段的最后一个字节(第 " + (themeSection.length - 1) +
                    " 字节 = 0x" + themeSection[themeSection.length - 1].toString(16) +
                    ")不是 0 —— 主题内容之后必须是 0 填充,而这一段整段都被填满了" +
                    " ⇒ 这不是本工具产出的容器(最可能:--in 指的是一份 image.bin 或 "
                    + "theme.json,而不是 pack 生成的那个 .bin)");
  }
  const contentLen = themeContentLength(themeSection, inFile);
  assertZeroPadding(themeSection, contentLen, inFile);   // ★ 先确认填充干净,再谈内容
  if (contentLen > THEME_FIRMWARE_BYTES) {
    throw new Error(inFile + ":theme 段有 " + contentLen + " 字节,超过固件能读进来的 " +
                    THEME_FIRMWARE_BYTES + " 字节" +
                    "(src/theme_load.cpp 只读 4095 字节)⇒ 刷上去设备也读不全,拒绝拆包。" +
                    "本工具不会产出这种容器,所以先确认 --in 指的是 pack 生成的那个 .bin");
  }
  const themeBuf = Buffer.from(buf.subarray(0, contentLen));
  checkThemeContent(themeBuf, inFile + " 的 theme 段");

  // ---- image 段:长度读它自己的清单 ----
  const imageStart = layout.imageOffset;
  const info = imageBlobLength(buf.subarray(imageStart), layout, inFile + " 的 image 段");
  const imageEnd = imageStart + info.total;

  if (imageEnd > buf.length) {
    throw new Error(inFile + ":image 包头说这一段有 " + info.total + " 字节,但容器里从 0x" +
                    imageStart.toString(16) + " 起只剩 " + (buf.length - imageStart) +
                    " 字节 ⇒ 文件被截断了(拷贝没拷完?)");
  }
  // ★ 两者不一致时报错而不是猜:多出来的必须真的是 0 填充
  for (let i = imageEnd; i < buf.length; i++) {
    if (buf[i] !== 0) {
      throw new Error(inFile + ":image 段按包头算到第 " + imageEnd + " 字节结束,但后面还有非 0 内容" +
                      "(第一个在第 " + i + " 字节)⇒ 容器的实际长度与 image 自己的清单" +
                      "(" + info.total + " 字节)不一致,拒绝猜哪一段算图片");
    }
  }
  const imageBuf = Buffer.from(buf.subarray(imageStart, imageEnd));

  if (themeOut) fs.writeFileSync(themeOut, themeBuf);
  if (imageOut) fs.writeFileSync(imageOut, imageBuf);

  return {
    layout: layout, table: table,
    themeBuf: themeBuf, imageBuf: imageBuf,
    themeBytes: themeBuf.length, imageBytes: imageBuf.length,
    imageInfo: info,
    containerBytes: buf.length
  };
}

// ------------------------------------------------------------
// 刷写命令(容器只有**一条**命令 —— 这正是合并的意义)
//   ★ 端口要换成自己的(COM6 是车主这台机器上那块 S3 的口)
//   ★ --chip 跟着目标板走(与 image-blob-build.js 的 esptoolCommand 同一套数据)
// ------------------------------------------------------------
function flashCommand(layout, binPath, table, port, targetId) {
  const csv = table ? table.csv : null;
  const id = targetId || (csv ? targetIdForCsv(csv) : null);
  const chip = id ? IB.targetInfo(id).chip
                  : (csv === "partitions-s3.csv" ? "esp32s3" : "esp32");
  const p = port || "COM6";
  return "python -m esptool --chip " + chip + " --port " + p + " --baud 921600 write_flash 0x" +
         layout.base.toString(16) + " " + (binPath || "assets.bin");
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
  return L.join("\n");
}

function hex(n) { return "0x" + n.toString(16); }

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
  parsePartitionCsv: parsePartitionCsv,
  loadPartitionTable: loadPartitionTable,
  findPartition: findPartition,
  computeLayout: computeLayout,
  describeLayout: describeLayout,
  themeContentLength: themeContentLength,
  findNonZero: findNonZero,
  checkThemeContent: checkThemeContent,
  imageBlobLength: imageBlobLength,
  pack: pack,
  unpack: unpack,
  flashCommand: flashCommand,
  targetIdForCsv: targetIdForCsv
};
