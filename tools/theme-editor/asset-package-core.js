/* ============================================================
 * asset-package-core.js —— 「配色 + 图片 → 一个 assets.bin」的**纯字节**内核
 *
 *   浏览器:  <script src="asset-package-core.js"></script>  →  window.AssetPackageCore
 *   Node:    const Core = require("./asset-package-core.js")
 *
 * ============================================================
 * 一、为什么要有这个文件(它是从 asset-package.js 里抽出来的)
 * ============================================================
 * 命令行那一半(asset-package.js,提交 3b147e6)先做完了:一个文件、一条
 * write_flash 刷完 theme 与 image 两块分区。但页面上的按钮一直没接,原因就
 * 写在 ASSET-PACKAGE.md 第 8 节最后一段:
 *
 *   "asset-package.js 同时给浏览器与 Node 用,但**注意**它用了 fs / path / Buffer
 *    ⇒ 页面里要么走 Node 侧(如果将来有),要么把 pack() 的**纯字节部分**抽出来
 *    给浏览器(现在的 pack() 直接落盘,不是纯函数)。"
 *
 * 这个文件就是那件事:**把纯字节部分抽出来,而且只留一份**。
 *
 *   · 容器怎么摆(从分区表算)、theme 段多长、image 段多长、什么时候报错、
 *     报错怎么说 —— **全部只有这一份实现**;
 *   · asset-package.js(CLI)与 ②图片页(image-editor.html)**都调这里**。
 *
 * ★ 为什么不许两边各写一份:两边分叉的后果不是"页面导出的文件难看",而是
 *   **页面导出的容器刷上去不对**(段的位置差一个字节,image 就整块错位),
 *   而且不报错。测试里那条"页面产物与 CLI 产物逐字节相同"就是为了钉死这件事。
 *
 * ============================================================
 * 二、这个文件里**不许**出现什么(硬规矩)
 * ============================================================
 *   不许 fs / path / Buffer / require(除了 UMD 那一行给 Node 取 ThemeJson)
 *   —— 只用 Uint8Array / DataView,以及 TextDecoder(浏览器与 Node 都有)。
 *   凡是"读文件、写文件、取仓库根"的事一律留在 asset-package.js 里。
 *   这条不是洁癖:页面是 file:// 双击打开的静态页,没有 Node,
 *   一个 require 就让整页白屏。
 *
 * ============================================================
 * 三、容器布局(基准 = theme 分区偏移,所以文件里的 +0 就是 flash 的 0x210000)
 * ============================================================
 *   +0x00000   theme.json 的内容,不足 themeSectionBytes 处补 **0x00**
 *   (中间)     theme 与 image 之间那一整块分区(本项目 = spiffs),全部 0x00
 *   +0x44000   image.bin 的内容(长度 = image 自己包头里写的那个数)
 *
 * 为什么补 0x00、为什么中间那块可以整块填 0、为什么容器里**不加自定义文件头**
 * (加了就不能直接 write_flash)—— 那三条的完整理由与依据在
 * ASSET-PACKAGE.md 第 1~2 节与 asset-package.js 的文件头注释里,这里不重抄。
 * 这里只留一句结论:**文件本身就是可以烧的镜像**。
 *
 * ============================================================
 * 四、对外只有这几个函数
 * ============================================================
 *   parsePartitionCsv(text, file)   分区表文本 → 分区数组(带 offset/size)
 *   findPartition(table, name)      按名字取一块分区(找不到/重名 ⇒ 报错)
 *   computeLayout(table)            分区表 → 三段的长度与位置(不满足就报错)
 *   describeLayout(layout)          给人看的那几行("+0x40000 …中间 262144 字节")
 *   themeContentLength / findNonZero / assertZeroPadding / checkThemeContent
 *   parseImageBlob(buf, o)          image 包头 + 清单 → 对象(每张图的名字/用途/尺寸/字节数)
 *   imageBlobLength(buf, layout, o) 只要长度(CLI 用,与 parseImageBlob 同一套检查)
 *   packBytes(o)                    两份字节 → 容器字节
 *   unpackBytes(o)                  容器字节 → 两份字节
 *   flashCommand(layout, bin, o)    那条 write_flash(偏移从 layout 取,不写死)
 *   wrap / utf8Decode / hex         小工具
 *
 * 有单测:tools/theme-editor/test-asset-package-core.js
 * ============================================================ */
(function (root, factory) {
  "use strict";
  // Node 里把 ThemeJson 取进来(网页里它是 <script src="theme-json.js"> 挂的全局,
  // 所以浏览器分支传 null,函数内部再取 root.ThemeJson)
  var ThemeJson = (typeof module === "object" && module && module.exports)
    ? require("./theme-json.js") : null;
  var api = factory(ThemeJson);
  if (typeof module === "object" && module && module.exports) module.exports = api;
  root.AssetPackageCore = api;            // 浏览器
})(typeof globalThis !== "undefined" ? globalThis : this, function (ThemeJson) {
  "use strict";

  // ★ ThemeJson 的来路:
  //   Node  → 上面那行 require("./theme-json.js")(与网页/固件同一套解析规则)
  //   网页  → null,checkThemeContent 里现取 globalThis.ThemeJson
  //          (<script src="theme-json.js"> 挂的全局,所以那个脚本要先加载)
  function themeJsonApi() {
    var T = ThemeJson || (typeof globalThis !== "undefined" ? globalThis.ThemeJson : null);
    if (!T || typeof T.parseThemeJson !== "function") {
      throw new Error("找不到 theme-json.js(主题文件的解析规则)。" +
                      "这一页要能离线双击打开,所以不许从网上取 —— " +
                      "请确认 theme-json.js 与它放在同一个目录里、并且在本文件**之前**加载");
    }
    return T;
  }

  // 主题分区里"配色"那一段的长度。
  // ★ 等于分区表里 theme 分区的 Size(0x4000)。它是**容器布局的一部分**
  //   (image 段的起点由它决定),分区表里 theme 不是 0x4000 就会直接报错,
  //   不会悄悄按 16KB 摆。
  var THEME_SECTION_BYTES = 16384;

  // 固件真正能读进来的主题字节数上限,由 src/theme_load.cpp 决定:
  //     static char buf[THEME_MAX_BYTES];                      // = (4 * 1024)
  //     esp_partition_read(part, 0, buf, sizeof(buf) - 1);     // 只读 4095 字节
  // ⇒ 超过 4095 字节的主题**在设备上会被截断**,而截断的 JSON 解析必然失败,
  //   表现是"刷进去了,但屏上是默认配色"——不报错的那种失败。所以这里当硬闸门。
  //   ★ 它与 theme 分区大小(0x4000)是**两个不同的数**:后者管布局,前者管
  //     固件读得进来多少。两者都检查。
  var THEME_FIRMWARE_BYTES = 4095;

  var PARTITION_LABEL_THEME = "theme";
  var PARTITION_LABEL_IMAGE = "image";

  // image 包头的默认常量(与 lib/themetool/image_blob.h 逐字段对应)。
  //   ★ 权威在 image-blob-build.js;CLI 会把它那一份通过 opts.ib 传进来**对账**,
  //     所以两边不会各自漂移(传进来的值与这里不一致 ⇒ 立刻报错,而不是悄悄用默认值)。
  //   页面里不用传:页面已经加载了 image-blob-build.js,值完全一样。
  var IMAGE_MAGIC = 0x44363032;           // '2','0','6','D'
  var IMAGE_VERSION = 1;
  var IMAGE_NAME_MAX = 24;
  var IMAGE_MAX_COUNT = 32;
  var IMAGE_ENTRY_SIZE = 44;              // 4+4+2+2+1+1+2+2+24,没有对齐空洞
  var IMAGE_HEADER_SIZE = 12 + IMAGE_MAX_COUNT * IMAGE_ENTRY_SIZE;   // = 1420

  // 每个段的包装标签(报错里怎么称呼它)。CLI 传自己的那套(带文件名),页面传
  // "页面里这份配色"/"页面里的图片"。**只影响报错文字,不影响一个字节**。
  var DEFAULT_LABELS = {
    theme: "配色(theme)",
    image: "图片(image)",
    container: "容器(assets.bin)"
  };

  function hex(n) { return "0x" + n.toString(16); }

  // Uint8Array 的字节有可能落在别的 ArrayBuffer 上(buf.subarray 就是),
  // 所以每次建 DataView 都要带 byteOffset/byteLength —— 少一个就是"读到别人的字节"。
  function dv(bytes) {
    return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  }

  // 每次都拷贝一份。理由:浏览器里 canvas 的 ImageData 缓冲会被复用,
  // 交给容器的那份字节必须是**自己的**(否则后面谁再画一笔,容器内容就变了)。
  function wrap(bytes) {
    if (bytes instanceof Uint8Array) return new Uint8Array(bytes);   // 拷贝
    if (bytes instanceof ArrayBuffer) return new Uint8Array(bytes.slice(0));
    if (ArrayBuffer.isView(bytes)) {
      return new Uint8Array(bytes.buffer.slice(bytes.byteOffset, bytes.byteOffset + bytes.byteLength));
    }
    throw new Error("要的是一段字节(Uint8Array / ArrayBuffer),给的是 " +
                    (bytes === null ? "null" : typeof bytes));
  }

  // ------------------------------------------------------------
  // 字节 → 字符串。
  // ★ 这里**不用 Buffer.toString("utf8")**:那个只在 Node 里有,页面里没有。
  //   用 TextDecoder(fatal:false) —— 与 Buffer 的宽松解码一致:非法序列换成
  //   U+FFFD,不抛错。**BOM 不剥**(Node 的 toString("utf8") 也不剥),
  //   这一条很重要:主题里的 BOM 会让 JSON 解析失败,而"报 JSON 解析失败"
  //   正是应该发生的事;悄悄剥掉 BOM 就等于把"文件本来有问题"藏起来。
  //   两侧逐字节一致的前提就是这里的解码结果与 Buffer 完全一样。
  // ------------------------------------------------------------
  var UTF8_DECODER = new TextDecoder("utf-8", { fatal: false });
  function utf8Decode(bytes) {
    return UTF8_DECODER.decode(bytes);
  }

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
    var where = file ? "(" + file + ")" : "";
    var out = [];
    var seen = Object.create(null);
    var lines = String(text).replace(/^\uFEFF/, "").split(/\r?\n/);

    for (var i = 0; i < lines.length; i++) {
      var raw = lines[i];
      var line = raw.trim();
      if (!line || line.charAt(0) === "#") continue;
      if (/^name\s*,\s*type/i.test(line)) continue;   // 表头那行(如果有)

      var c = line.split(",").map(function (s) { return s.trim(); });
      // 两列的行(比如表头残留)直接跳过,不猜
      if (c.length < 5) continue;

      var name = c[0], type = c[1], subtype = c[2], offStr = c[3], sizeStr = c[4];
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
    out.sort(function (a, b) { return a.offset - b.offset; });
    return out;
  }

  function findPartition(table, name) {
    var hit = table.parts.filter(function (p) { return p.name === name; });
    if (hit.length !== 1) {
      throw new Error("分区表 " + table.csv + " 里" +
                      (hit.length ? "有两块叫「" + name + "」的分区" : "找不到名为「" + name + "」的分区") +
                      "。这张表里的分区是:" + table.parts.map(function (p) { return p.name; }).join(" / ") +
                      " ⇒ 请确认 --table 指向的是这台设备正在用的分区表");
    }
    return hit[0];
  }

  // ------------------------------------------------------------
  // 布局:从分区表算出三段的位置。任何不满足前置条件的地方都**报错**。
  // ------------------------------------------------------------
  function computeLayout(table) {
    var theme = findPartition(table, PARTITION_LABEL_THEME);
    var image = findPartition(table, PARTITION_LABEL_IMAGE);

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

    var between = table.parts
      .filter(function (p) { return p.offset > theme.offset && p.offset < image.offset; })
      .sort(function (a, b) { return a.offset - b.offset; });

    var cursor = theme.offset + theme.size;
    for (var i = 0; i < between.length; i++) {
      var p = between[i];
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
      spiffsBytes: between.reduce(function (s, p) { return s + p.size; }, 0),  // theme 与 image 之间那一整块
      betweenLabels: between.map(function (p) { return p.name; }),
      imageOffset: image.offset - theme.offset,  // image 在容器里的偏移
      maxImageBytes: image.size,                 // image 段最多能写多少(分区大小)
      containerMax: image.offset + image.size - theme.offset,
      containerMin: image.offset - theme.offset
    };
  }

  function describeLayout(layout) {
    var L = [];
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
    for (var i = themeSection.length - 1; i >= 0; i--) {
      if (themeSection[i] !== 0) return i + 1;
    }
    throw new Error(where + ":theme 那一段(" + themeSection.length + " 字节)全是 0 —— " +
                    "里面没有配色内容。这不是本工具产出的容器(或者 theme 段被清掉了)");
  }

  // 从 from 起找第一个非 0 字节,找不到返回 -1
  function findNonZero(buf, from) {
    for (var i = from; i < buf.length; i++) if (buf[i] !== 0) return i;
    return -1;
  }

  // theme 段的 0 填充必须真的全是 0,否则报错并**指出第几个字节**。
  // ★ 这条检查必须排在 JSON 解析**之前**:填充里混了垃圾时,内容长度会一直算到垃圾末尾,
  //   于是 JSON 解析会报"最后一个字节不是收尾括号"——那是指错方向(真正的原因是填充脏了)。
  function assertZeroPadding(themeSection, contentLen, where) {
    var off = findNonZero(themeSection, contentLen);
    if (off < 0) return;
    throw new Error(where + ":theme 段在第 " + off + " 字节(0x" + off.toString(16) +
                    ",内容 " + JSON.stringify(String.fromCharCode(themeSection[off])) +
                    ")之后还有非 0 内容 —— 主题内容本应在第 " + contentLen +
                    " 字节结束、之后全是 0 填充 ⇒ 这不是本工具产出的容器," +
                    "或者 theme 段被写坏了(拒绝猜哪一段才是配色)");
  }

  // 主题内容是否通过(格式 + 大小 + 收尾)。返回解析出来的对象;失败时抛带人话的错。
  function checkThemeContent(buf, label) {
    var text = utf8Decode(buf);
    var trimmed = text.replace(/\s+$/, "");
    if (!trimmed) {
      throw new Error(label + ":内容只有空白,不是主题 JSON");
    }
    // 收尾必须是 JSON 的右括号:允许的收尾只有这一种,别的都说明是"截断/拼接"
    var last = trimmed.charAt(trimmed.length - 1);
    if (last !== "}" && last !== "]") {
      // 长度算到哪、多出来多少:混进垃圾时长度会一直算到垃圾末尾,
      // 于是"最后一个字节"其实是垃圾 —— 把这两个数一起报出来,原因就看得见了。
      var cIdx = Math.max(trimmed.lastIndexOf("}"), trimmed.lastIndexOf("]"));
      var extra = trimmed.length - cIdx - 1;
      throw new Error(label + ":内容的最后一个字节不是 JSON 的收尾括号(是 " + JSON.stringify(last) +
                      (extra > 0 ? ";最后那个收尾括号在第 " + (cIdx + 1) + " 字节," +
                                   "它后面还多出 " + extra + " 个字节" : "") +
                      ")⇒ 这段不是完整的主题文件:" +
                      "要么 --in 指错了文件,要么 theme 段里混进了非 0 垃圾");
    }
    var obj;
    try {
      obj = themeJsonApi().parseThemeJson(trimmed);        // 与网页/固件同一套规则
    } catch (e) {
      // ★ 这条报错要能指向**真正的原因**:取回来的内容不是合法 JSON,最常见的两种是
      //   ① --in 指错了文件;② theme 段里混进了非 0 垃圾(那时"长度"会一直算到垃圾末尾)。
      throw new Error(label + ":主题 JSON 解析失败 —— " + e.message +
                      "(这段是 theme 分区里从 +0 起、到最后一个非 0 字节为止的内容;" +
                      "如果它本来就不是主题文件,说明 --in 指错了文件;" +
                      "如果这个文件是别处弄来的,还要怀疑 theme 段里混进了非 0 垃圾" +
                      " —— 本工具产出的容器里,主题内容之后必须全是 0)");
    }
    if (!themeJsonApi().isPlainObject(obj)) {
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
  // image 包头的常量对账:CLI 把自己那份 IB 传进来,不一致就报错
  // (宁可报错也不许"两边各用一份数"—— 那正是分叉的开始)
  // ------------------------------------------------------------
  function imageConstants(opts) {
    var ib = opts && opts.ib;
    if (!ib) {
      return { magic: IMAGE_MAGIC, version: IMAGE_VERSION, nameMax: IMAGE_NAME_MAX,
               maxCount: IMAGE_MAX_COUNT, entrySize: IMAGE_ENTRY_SIZE,
               headerSize: IMAGE_HEADER_SIZE };
    }
    var v = {
      magic: ib.MAGIC, version: ib.VERSION, nameMax: ib.NAME_MAX,
      maxCount: ib.MAX_COUNT, entrySize: ib.ENTRY_SIZE, headerSize: ib.HEADER_SIZE
    };
    var mine = { magic: IMAGE_MAGIC, version: IMAGE_VERSION, nameMax: IMAGE_NAME_MAX,
                 maxCount: IMAGE_MAX_COUNT, entrySize: IMAGE_ENTRY_SIZE,
                 headerSize: IMAGE_HEADER_SIZE };
    var keys = ["magic", "version", "nameMax", "maxCount", "entrySize", "headerSize"];
    for (var i = 0; i < keys.length; i++) {
      if (v[keys[i]] !== mine[keys[i]]) {
        throw new Error("image 包头常量对不上(image-blob-build.js 的 " + keys[i] + " = " +
                        v[keys[i]] + ",本文件内置的 = " + mine[keys[i]] +
                        ")⇒ 设备端格式改过了,两处必须一起改:" +
                        "改 lib/themetool/image_blob.h、image-blob-build.js 与本文件的默认值");
      }
    }
    return v;
  }

  // ------------------------------------------------------------
  // 读 image 自己的清单。**这是"每张图是什么"的唯一来源**
  // (用途 role / 名字 / 宽高 / 字节数全在包头里,不去猜、也不看文件剩下的长度)。
  //
  // 返回:
  //   { magic, version, count, dataBytes, headerBytes, totalBytes,
  //     entries:[{ index, name, role, w, h, bytes, offset, cf, order }] }
  // 任何一项不对 ⇒ 抛带人话的错(与 CLI 那套文案是同一份)。
  // ------------------------------------------------------------
  function parseImageBlob(buf, opts) {
    opts = opts || {};
    var C = imageConstants(opts);
    var label = opts.label || DEFAULT_LABELS.image;

    if (buf.length < C.headerSize) {
      throw new Error(label + ":只有 " + buf.length + " 字节,连 image 包头(" +
                      C.headerSize + " 字节)都不够 ⇒ 不是一份 image.bin");
    }
    var view = dv(buf);
    var magic = view.getUint32(0, true);
    var version = view.getUint16(4, true);
    var count = view.getUint16(6, true);
    var dataBytes = view.getUint32(8, true);

    if (magic !== C.magic || version !== C.version) {
      throw new Error(label + ":image 包头不对(魔数 0x" + magic.toString(16) +
                      " 期望 0x" + C.magic.toString(16) + ",版本 " + version +
                      " 期望 " + C.version + ")⇒ 这段不是 image.bin。" +
                      "常见原因:theme 段与 spiffs 段的长度摆错了(请用 --table 指定正确的分区表)");
    }
    if (count > C.maxCount) {
      throw new Error(label + ":image 包头里的图片数量 " + count + " 超过上限 " +
                      C.maxCount + " ⇒ 包头已被写坏");
    }

    var entries = [];
    for (var i = 0; i < count; i++) {
      var at = 12 + i * C.entrySize;
      // ★ name 从项的 **+18** 起(char[24]),不是 +20 —— 权威是 image-blob-build.js
      //   的 build():它就用 base+18,而且那里写着"曾经以为要从 20 起"这个坑。
      //   写错一个字节的后果是名字整体错位(例如 "bg" 读成空、"sportL" 读成 "ortL"),
      //   不报错、只是显示得莫名其妙 ⇒ 单测里有一条专门盯名字。
      //   名字以 0 结尾;取到第一个 0 为止(UTF-8 解码,与设备端一致)。
      var raw = new Uint8Array(buf.buffer, buf.byteOffset + at + 18, C.nameMax);
      var end = raw.indexOf(0);
      var nameBytes = end < 0 ? raw : raw.subarray(0, end);
      entries.push({
        index: i,
        name: utf8Decode(nameBytes),
        role: view.getUint16(at + 14, true),
        order: view.getUint16(at + 16, true),
        w: view.getUint16(at + 8, true),
        h: view.getUint16(at + 10, true),
        bytes: view.getUint32(at + 4, true),        // size:这张图的像素数据字节数
        offset: view.getUint32(at + 0, true),
        cf: view.getUint8(at + 12),
        stridePad: view.getUint8(at + 13)
      });
    }

    return {
      magic: magic, version: version, count: count,
      dataBytes: dataBytes,
      headerBytes: C.headerSize,
      totalBytes: C.headerSize + dataBytes,
      entries: entries
    };
  }

  // 只要长度(CLI 的 pack/unpack 用)。与 parseImageBlob **同一套检查**,
  // 只是再多一条"装不装得进 image 分区"。返回形状与老版本一致(count/dataBytes/
  // headerBytes/total),这样 CLI 的输出一个字都不用改。
  function imageBlobLength(buf, layout, label) {
    var info = parseImageBlob(buf, { label: label });
    var total = info.totalBytes;
    if (total > layout.maxImageBytes) {
      throw new Error(label + ":按清单算出的长度 " + total + " 字节超过 image 分区(" +
                      layout.maxImageBytes + " 字节 = " + layout.imagePart.sizeText +
                      ")⇒ 这份容器装不下/被截断过。请重新打包(或用 --target 指定分区更大的那块板)");
    }
    return { total: total, headerBytes: info.headerBytes, dataBytes: info.dataBytes,
             count: info.count, entries: info.entries };
  }

  // ------------------------------------------------------------
  // packBytes:两份**字节** → 容器字节(纯函数,不落盘、不读文件)
  //
  // opts: { themeBytes, imageBytes, layout, table,
  //         themeLabel, imageLabel, ib }
  // 返回: { bytes, totalLen, themeBytes, imageBytes, imageInfo, layout }
  // ------------------------------------------------------------
  function packBytes(opts) {
    opts = opts || {};
    var layout = opts.layout;
    var themeBytes = wrap(opts.themeBytes);
    var imageBytes = wrap(opts.imageBytes);
    var themeLabel = opts.themeLabel || DEFAULT_LABELS.theme;
    var imageLabel = opts.imageLabel || DEFAULT_LABELS.image;

    // ---- theme 的三种"太大"分别说清楚,不要合成一句 ----
    if (themeBytes.length > THEME_FIRMWARE_BYTES) {
      throw new Error(themeLabel + "有 " + themeBytes.length +
                      " 字节,超过固件能读进来的 " + THEME_FIRMWARE_BYTES + " 字节" +
                      "(src/theme_load.cpp 的 buf[THEME_MAX_BYTES];THEME_MAX_BYTES = 4KB," +
                      "只读 sizeof(buf)-1 = 4095 字节)。刷进去会被截断、解析失败," +
                      "结果是「屏上还是默认配色」而且不报错 ⇒ 拒绝打包。" +
                      "主题是纯文本,删掉用不上的字段即可(正常一份约 1.2~2KB)");
    }
    if (themeBytes.length > layout.themeSectionBytes) {
      throw new Error(themeLabel + "有 " + themeBytes.length +
                      " 字节,超过 theme 分区 / 段长 " + layout.themeSectionBytes +
                      "(0x" + layout.themeSectionBytes.toString(16) + ")⇒ 装不下");
    }
    checkThemeContent(themeBytes, themeLabel);

    // ---- image 段长度由它自己的包头决定 ----
    var info = imageBlobLength(imageBytes, layout, imageLabel);
    if (info.total !== imageBytes.length) {
      throw new Error(imageLabel + "是 " + imageBytes.length +
                      " 字节,但它自己的包头写的是 " + info.total + " 字节" +
                      "(包头 " + info.headerBytes + " + 数据 " + info.dataBytes +
                      ",共 " + info.count + " 张图)⇒ 两者不一致,不猜:请重新导出这份 image.bin");
    }
    if (imageBytes.length > layout.maxImageBytes) {
      throw new Error("image 有 " + imageBytes.length + " 字节,超过 image 分区 " +
                      layout.maxImageBytes + " 字节(" + layout.imagePart.sizeText +
                      ")⇒ 刷进去会被截断。请减少/缩小图片,或用 --table 指定分区更大的那块板");
    }

    // ---- 拼容器 ----
    var totalLen = layout.imageOffset + imageBytes.length;
    var out = new Uint8Array(totalLen);            // 全 0:theme 段尾部与中间那段都是 0
    out.set(themeBytes, 0);
    out.set(imageBytes, layout.imageOffset);

    return {
      bytes: out, layout: layout, table: opts.table || null,
      totalLen: totalLen,
      themeBytes: themeBytes.length, imageBytes: imageBytes.length,
      imageInfo: info
    };
  }

  // ------------------------------------------------------------
  // unpackBytes:容器字节 → 两份字节(纯函数,不落盘)
  //
  // opts: { bytes, layout, table, containerLabel, ib }
  // 返回: { bytes, layout, table, themeBytes, imageBytes, themeInfo, imageInfo }
  //   themeInfo = { bytes: 内容长度, parsed: 解析出来的对象 }
  //   imageInfo = parseImageBlob() 那个清单(每张图的名字/用途/尺寸/字节数)
  // ------------------------------------------------------------
  function unpackBytes(opts) {
    opts = opts || {};
    var layout = opts.layout;
    var buf = wrap(opts.bytes);
    var label = opts.containerLabel || DEFAULT_LABELS.container;
    var imageLabel = opts.imageLabel || (opts.containerLabel ? label + " 的 image 段"
                                                             : DEFAULT_LABELS.image);
    var themeLabel = opts.themeLabel || (opts.containerLabel ? label + " 的 theme 段"
                                                             : DEFAULT_LABELS.theme);

    if (buf.length < layout.containerMin) {
      throw new Error(label + ":只有 " + buf.length + " 字节,而 theme + 中间分区两段就要 " +
                      layout.containerMin + " 字节(0x" + layout.containerMin.toString(16) +
                      ")⇒ 这不是本工具产出的容器");
    }
    if (buf.length > layout.containerMax) {
      throw new Error(label + ":" + buf.length + " 字节超过「theme 段 + 中间分区 + image 分区」的 " +
                      layout.containerMax + " 字节 ⇒ 这不是按 " + (opts.table ? opts.table.csv : "") +
                      " 摆出来的容器(是不是拿了另一块板的分区表?)");
    }

    // ---- theme 段 ----
    var themeSection = buf.subarray(0, layout.themeSectionBytes);
    // ★ 先看"段尾有没有 0 填充":内容再长也长不过段长(16384),所以**段尾必须是 0**。
    //   段尾非 0 ⇒ theme 段整个填满了 ⇒ 这份文件不是本工具产出的。
    //   这条必须放在最前面:把它当主题去解析,报错会指向"JSON 不合法 / 超过 4095 字节",
    //   而真正的原因(这压根不是容器)就看不出来了 —— 把随机字节当容器时正是这种情况。
    if (themeSection[themeSection.length - 1] !== 0) {
      throw new Error(label + ":theme 段的最后一个字节(第 " + (themeSection.length - 1) +
                      " 字节 = 0x" + themeSection[themeSection.length - 1].toString(16) +
                      ")不是 0 —— 主题内容之后必须是 0 填充,而这一段整段都被填满了" +
                      " ⇒ 这不是本工具产出的容器(最可能:指的是一份 image.bin 或 " +
                      "theme.json,而不是 pack 生成的那个 .bin)");
    }
    var contentLen = themeContentLength(themeSection, label);
    assertZeroPadding(themeSection, contentLen, label);   // ★ 先确认填充干净,再谈内容
    if (contentLen > THEME_FIRMWARE_BYTES) {
      throw new Error(label + ":theme 段有 " + contentLen + " 字节,超过固件能读进来的 " +
                      THEME_FIRMWARE_BYTES + " 字节" +
                      "(src/theme_load.cpp 只读 4095 字节)⇒ 刷上去设备也读不全,拒绝拆包。" +
                      "本工具不会产出这种容器,所以先确认指的是 pack 生成的那个 .bin");
    }
    var themeBytes = wrap(themeSection.subarray(0, contentLen));
    var themeObj = checkThemeContent(themeBytes, themeLabel);

    // ---- image 段:长度读它自己的清单 ----
    var imageStart = layout.imageOffset;
    var info = imageBlobLength(buf.subarray(imageStart), layout, imageLabel);
    var imageEnd = imageStart + info.total;

    if (imageEnd > buf.length) {
      throw new Error(label + ":image 包头说这一段有 " + info.total + " 字节,但容器里从 0x" +
                      imageStart.toString(16) + " 起只剩 " + (buf.length - imageStart) +
                      " 字节 ⇒ 文件被截断了(拷贝没拷完?)");
    }
    // ★ 两者不一致时报错而不是猜:多出来的必须真的是 0 填充
    for (var i = imageEnd; i < buf.length; i++) {
      if (buf[i] !== 0) {
        throw new Error(label + ":image 段按包头算到第 " + imageEnd + " 字节结束,但后面还有非 0 内容" +
                        "(第一个在第 " + i + " 字节)⇒ 容器的实际长度与 image 自己的清单" +
                        "(" + info.total + " 字节)不一致,拒绝猜哪一段算图片");
      }
    }
    var imageBytes = wrap(buf.subarray(imageStart, imageEnd));

    return {
      bytes: buf, layout: layout, table: opts.table || null,
      themeBytes: themeBytes, imageBytes: imageBytes,
      themeInfo: { bytes: themeBytes.length, parsed: themeObj },
      imageInfo: info,
      containerBytes: buf.length
    };
  }

  // ------------------------------------------------------------
  // 刷写命令(容器只有**一条**命令 —— 这正是合并的意义)
  //   ★ 端口要换成自己的(COM6 是车主这台机器上那块 S3 的口)
  //   ★ 偏移**从 layout 取**(= 分区表里 theme 分区的偏移),一个字都不许写死
  // ------------------------------------------------------------
  function flashCommand(layout, binPath, opts) {
    opts = opts || {};
    var chip = opts.chip || "esp32s3";
    var port = opts.port || "COM6";
    return "python -m esptool --chip " + chip + " --port " + port + " --baud 921600 write_flash 0x" +
           layout.base.toString(16) + " " + (binPath || "assets.bin");
  }

  return {
    THEME_SECTION_BYTES: THEME_SECTION_BYTES,
    THEME_FIRMWARE_BYTES: THEME_FIRMWARE_BYTES,
    IMAGE_MAGIC: IMAGE_MAGIC, IMAGE_VERSION: IMAGE_VERSION,
    IMAGE_NAME_MAX: IMAGE_NAME_MAX, IMAGE_MAX_COUNT: IMAGE_MAX_COUNT,
    IMAGE_ENTRY_SIZE: IMAGE_ENTRY_SIZE, IMAGE_HEADER_SIZE: IMAGE_HEADER_SIZE,
    hex: hex, wrap: wrap, utf8Decode: utf8Decode,
    parsePartitionCsv: parsePartitionCsv,
    findPartition: findPartition,
    computeLayout: computeLayout,
    describeLayout: describeLayout,
    themeContentLength: themeContentLength,
    findNonZero: findNonZero,
    assertZeroPadding: assertZeroPadding,
    checkThemeContent: checkThemeContent,
    parseImageBlob: parseImageBlob,
    imageBlobLength: imageBlobLength,
    packBytes: packBytes,
    unpackBytes: unpackBytes,
    flashCommand: flashCommand
  };
});
