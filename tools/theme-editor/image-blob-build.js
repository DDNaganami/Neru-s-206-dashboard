/* ============================================================
 * image-blob-build.js —— 把图片打成设备认的 image.bin
 *
 * 为什么单独一个文件、而不是写在 index.html 里:
 *   这个文件的输出是**二进制格式**,格式错了设备端要么不显示、要么读越界。
 *   放进 HTML 就只能靠肉眼在浏览器里点,没法自动验证。
 *   单独一个 .js 之后,Node 可以 require 它、把产物喂给固件的解析器
 *   (见 test_image_roundtrip.ps1),做到"同一份格式定义、两边都测"。
 *
 * 格式定义在 lib/themetool/image_blob.h,改字段必须两边一起改。
 * 两边不一致会被 test_image_roundtrip.ps1 直接抓出来。
 *
 * 这个文件同时给浏览器(<script src>)和 Node(module.exports)用,
 * 所以不写 import/export,只在末尾挂到 globalThis / module.exports。
 * ============================================================ */
(function (root, factory) {
  "use strict";
  var api = factory();
  if (typeof module === "object" && module && module.exports) {
    module.exports = api;             // Node
  }
  root.ImageBlob = api;               // 浏览器
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  // ---- 与 image_blob.h 逐字对应的常量 ----
  // ★ 任何一处改了,这里的数字也要改,否则设备端解析失败(而且会静默失败)
  var MAGIC      = 0x44363032;   // '2','0','6','D'
  var VERSION    = 1;
  var NAME_MAX   = 24;
  var MAX_COUNT  = 32;
  // ★ 项大小 44(曾经因为看错对齐规则写成 48,见下面 build() 里的注释)。
  //   44 = 4+4+2+2+1+1+2+2+24,每一项都紧跟前一项,**没有对齐空洞**。
  var ENTRY_SIZE = 44;
  var HEADER_SIZE = 12 + MAX_COUNT * ENTRY_SIZE;   // = 1420

  // 分区大小。★ 有两块板,**image 分区不一样大**(4MB 板 1MB / S3 16MB 板 8MB),
  //   所以这里只留"经典板"这个名字给老代码用;要按目标板取大小请用
  //   TARGETS + partitionBytesFor(id)。见下面那段说明。
  var PARTITION_BYTES = 1024 * 1024;   // = TARGETS.classic.partitionBytes

  // ============================================================
  // 目标板(2026-09-18 加)
  //
  // 为什么要有这个东西:固件那边 [env:esp32s3] 已经把 IMAGE_PARTITION_BYTES
  // 放宽到 8MB(partitions-s3.csv 的 image 分区),而编辑器这边还写死 1MB ——
  // 于是**多出来的 7MB 从网页导出进不去**:图做大了就在浏览器里被拦下,
  // 提示还写着"超过 image 分区(1024 KB)",让人以为设备装不下。
  //
  // ★ 两份分区表的 theme/image **偏移刻意相同**(0x210000 / 0x254000),
  //   所以 esptool 命令一个字都不用改 —— 只有"能放多大"这一条不同。
  //   这也正是当初把偏移对齐的目的(见 partitions-s3.csv 的文件头注释)。
  // ★ 这些数字必须与分区表一致:test-image-blob-build.js 会**直接读两份 csv**
  //   对账,改了一边忘了另一边会被测试抓住。
  // ============================================================
  var TARGETS = {
    classic: {
      id: "classic",
      label: "经典 ESP32(4MB flash)",
      partitionBytes: 1024 * 1024,
      partitionsCsv: "partitions.csv",
      // 面板大于这个就别指望装了(给 UI 提示用,不作为硬闸门)
      hint: "image 分区 1MB:192×192 能放下一整套表情,240×240 放不下全部"
    },
    s3: {
      id: "s3",
      label: "ESP32-S3 N16R8(16MB flash)",
      partitionBytes: 8 * 1024 * 1024,
      partitionsCsv: "partitions-s3.csv",
      hint: "image 分区 8MB:240×240 一整套表情 + 480×480 背景都放得下"
    }
  };
  var DEFAULT_TARGET = "classic";

  function targetInfo(id) {
    var t = TARGETS[id || DEFAULT_TARGET];
    if (!t) throw new Error("未知的目标板:" + id);
    return t;
  }
  function partitionBytesFor(id) { return targetInfo(id).partitionBytes; }

  // LVGL 颜色格式(与 lv_color.h 的枚举值一致)
  var CF = {
    L8: 0x06, I8: 0x0A, A8: 0x0E,
    RGB888: 0x0F, ARGB8888: 0x10, XRGB8888: 0x11,
    RGB565: 0x12, RGB565A8: 0x14
  };

  // 图片角色(与 image_blob.h 的 ImageRole 一致)
  // 左右屏各一套表情:车速表和转速表的表情差分不同,所以按「屏 × 状态」命名。
  // ★ 左右屏的含义按**法系车**(标致 206 实车)来:**左屏 = 转速表,右屏 = 速度表**。
  //   别按"左车速右转速"的日德习惯理解(装反了不会报错,只会画错表)。
  // ★ 两屏的状态**不一样**,各 4 张,一共 8 张(顺带解决了 1MB 分区装不下
  //   一整套表情的问题):
  //     左屏(转速表,只看转速):常态 / 红区 / 巡航 / 运动
  //     右屏(速度表,只看车速):常态 / 惊喜 / 巡航 / 运动
  //   水温不参与表情(只驱动水温弧与水温数字)。
  // ★ 保留编号 2 / 5 / 7 / 9 / 10 / 11 / 14 / 15 / 16 / 19 / 20
  //   **不在这里出现、也不复用**(分别是当年的:开机帧、左屏惊喜、右屏红区、
  //   左右屏开机图、左右屏眨眼图、左右屏冷车/过热图)。
  //   复用会让别人已导出的 image.bin 里那几张图突然变成别的表情,而且不报错。
  //   **新角色从 21 开始接。**
  // ★ 8 号当年是"惊喜"(急加速瞬态),现在语义是**超速**(>130 km/h)——
  //   只改名不改号,已导出的 image.bin 不受影响。见 image_blob.h。
  var ROLE = {
    Background: 1,
    // 左屏(转速表)
    FaceIdle: 3, FaceRedline: 4, FaceCruise: 12, FaceSport: 13,
    // 右屏(速度表)
    FaceIdleR: 6, FaceOverspeedR: 8, FaceCruiseR: 17, FaceSportR: 18
  };
  var ROLE_NAMES = {
    1: "表盘背景",
    3: "左屏表情·常态", 4: "左屏表情·红区",
    12: "左屏表情·巡航", 13: "左屏表情·运动",
    6: "右屏表情·常态", 8: "右屏表情·超速",
    17: "右屏表情·巡航", 18: "右屏表情·运动"
  };

  function bytesPerPixel(cf) {
    switch (cf) {
      case CF.L8: case CF.I8: case CF.A8: return 1;
      case CF.RGB565: case CF.RGB565A8: return 2;
      case CF.RGB888: return 3;
      case CF.ARGB8888: case CF.XRGB8888: return 4;
      default: return 0;
    }
  }

  // ------------------------------------------------------------
  // RGB565 编码:LVGL 用小端存放 —— 低字节在前。
  // 写反了画面会变成"红蓝互换 + 条纹",而且很难一眼看出是字节序问题。
  // ------------------------------------------------------------
  function pack565(r, g, b) {
    // 各通道取高位,低位丢弃(565 只有 5/6/5 位)
    var v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    return v & 0xFFFF;
  }

  // ------------------------------------------------------------
  // 把一张「宽 w、高 h、RGBA 逐字节」的图转成 RGB565 小端像素
  //
  // opts:
  //   stridePad  每行末尾补几个字节(默认 0)
  //   alphaBg    alpha 混合用的底色 [r,g,b](默认不混合)
  // ------------------------------------------------------------
  function rgbaToRgb565(rgba, w, h, opts) {
    opts = opts || {};
    var pad = opts.stridePad | 0;
    var stride = w * 2 + pad;
    var out = new Uint8Array(stride * h);
    var bg = opts.alphaBg || null;

    for (var y = 0; y < h; y++) {
      var srcRow = y * w * 4;
      var dst = y * stride;
      for (var x = 0; x < w; x++) {
        // ★ 必须带 x 偏移。少了 `+ x * 4` 的后果是"每行都重复第一个像素",
        //   小图看着还挺像样(纯色横幅),大图就整片横向糊掉 —— 很难查。
        var src = srcRow + x * 4;
        var r = rgba[src], g = rgba[src + 1], b = rgba[src + 2], a = rgba[src + 3];
        if (bg && a < 255) {
          // 半透明贴到背景色上(PNG 常带透明通道,不处理会出现黑边)
          var k = a / 255;
          r = Math.round(r * k + bg[0] * (1 - k));
          g = Math.round(g * k + bg[1] * (1 - k));
          b = Math.round(b * k + bg[2] * (1 - k));
        }
        var v = pack565(r, g, b);
        out[dst + x * 2]     = v & 0xFF;         // 低字节在前
        out[dst + x * 2 + 1] = (v >> 8) & 0xFF;
      }
      // 填充字节写 0(不是 0xFF):LVGL 不会读它们,但保持可重复
    }
    return out;
  }

  // ------------------------------------------------------------
  // RGBA → RGB565A8(带透明通道,给"要叠在别的图层上"的图用)
  //
  // 为什么要专门一个格式:表情图如果是不透明方块,会把底下的弧线挡掉一块。
  // RGB565 里没有 alpha 位,LVGL 为此提供 RGB565A8:
  //   **上半部** = 整张 RGB565 平面(stride*h 字节)
  //   **下半部** = 独立的 A8 平面(stride/2*h 字节,每像素 1 字节)
  // 已在 LVGL 源码确认(lv_draw_buf.c 的 _calculate_draw_buf_size):
  //     if(cf == LV_COLOR_FORMAT_RGB565A8) size += (stride / 2) * h;  // A8 mask
  //   所以每像素 3 字节,A8 的行宽是 RGB565 行宽的一半。
  //
  // ★ 注意:颜色**不做背景合成** —— 透明就是透明,由 LVGL 在绘制时混合。
  //   这与"打包时合成掉 alpha"是两条不同的路线,不要混。
  // ------------------------------------------------------------
  function rgbaToRgb565A8(rgba, w, h) {
    var stride = w * 2;                     // RGB565 平面每行字节数
    var colorBytes = stride * h;
    var out = new Uint8Array(colorBytes + (stride >> 1) * h);

    for (var y = 0; y < h; y++) {
      var src = y * w * 4;
      var dstC = y * stride;
      var dstA = colorBytes + y * (stride >> 1);
      for (var x = 0; x < w; x++) {
        var i = src + x * 4;
        var v = pack565(rgba[i], rgba[i + 1], rgba[i + 2]);
        out[dstC + x * 2]     = v & 0xFF;   // 小端
        out[dstC + x * 2 + 1] = (v >> 8) & 0xFF;
        out[dstA + x]         = rgba[i + 3];
      }
    }
    return out;
  }

  // 每种颜色格式每像素占多少字节(用于校验像素数组长度)。
  // RGB565A8 是 3 字节/像素(2 字节色 + 1 字节 alpha),不是 2。
  function packedBytesPerPixel(cf) {
    if (cf === CF.RGB565A8) return 3;
    return bytesPerPixel(cf);
  }

  //
  // items: [{ name, w, h, cf, role, order, pixels(Uint8Array), stridePad }]
  // 返回 { blob: Uint8Array, entries: [...], dataBytes, totalBytes }
  //
  // 校验从严:宁可在这里报错,也不要产出一个设备端读不了的 bin。
  // ------------------------------------------------------------
  function build(items) {
    if (!items || !items.length) throw new Error("没有图片可打包");
    if (items.length > MAX_COUNT) {
      throw new Error("最多 " + MAX_COUNT + " 张图,现在有 " + items.length + " 张");
    }

    var entries = [];
    var off = 0;

    for (var i = 0; i < items.length; i++) {
      var it = items[i];
      var cf = it.cf === undefined ? CF.RGB565 : it.cf;
      var bpp = bytesPerPixel(cf);
      if (!bpp) throw new Error("第 " + (i + 1) + " 张:不支持的颜色格式 0x" + cf.toString(16));
      // 紧凑字节数用 packedBytesPerPixel:RGB565A8 是 3 字节/像素
      // (2 字节色 + 1 字节 alpha),用 bytesPerPixel 校验会算错。
      var pbpp = packedBytesPerPixel(cf);

      var w = it.w | 0, h = it.h | 0;
      if (w <= 0 || h <= 0 || w > 65535 || h > 65535) {
        throw new Error("第 " + (i + 1) + " 张:尺寸不合法 " + w + "x" + h);
      }

      var px = it.pixels;
      var pxLen = px ? px.length : 0;
      // 紧凑长度 = 每像素**打包后**字节数 × 宽 × 高
      // (RGB565A8 的 3 字节/像素已经含了 alpha 平面,不再另加)
      var tight = pbpp * w * h;

      // stride_pad 没显式给的时候,**从像素数组长度反推**。
      // 为什么:像素数据里其实已经含了填充(rgbaToRgb565 会按 stridePad 补零),
      // 却要求调用方再传一次同样的数字 —— 忘了传就报"字节数不对",
      // 而且报的数字(8 vs 12)完全不提 padding,极难查。这里直接算出来。
      var pad;
      if (it.stridePad === undefined || it.stridePad === null) {
        if (pxLen === tight) {
          pad = 0;
        } else if (pxLen > tight && pxLen % h === 0) {
          pad = pxLen / h - pbpp * w;     // 由实际行宽反推
          if (pad < 0 || pad > 255) {
            throw new Error("第 " + (i + 1) + " 张:像素字节数 " + pxLen +
                            " 与 " + w + "x" + h + " 对不上,反推出的 stride_pad=" + pad + " 超出 0..255");
          }
        } else {
          throw new Error("第 " + (i + 1) + " 张:像素字节数不对(应为 " + tight +
                          " 或 行宽×" + h + ",实际 " + pxLen + ")");
        }
      } else {
        pad = it.stridePad | 0;
        if (pad < 0 || pad > 255) throw new Error("第 " + (i + 1) + " 张:stride_pad 超出 0..255");
      }

      var size = (pbpp * w + pad) * h;
      if (size > 0xFFFFFFFF) throw new Error("第 " + (i + 1) + " 张:太大");

      if (pxLen !== size) {
        throw new Error("第 " + (i + 1) + " 张:像素字节数不对(按 " + w + "x" + h +
                        " + stride_pad=" + pad + " 应为 " + size + ",实际 " + pxLen + ")");
      }
      // offset 现在是 u32(曾经是 u16,把整个镜像卡在 64KB 以内)。
      // 这里只挡住真的越界,「放不放得进分区」由 build 之后的检查负责。
      if (off + size > 0xFFFFFFFF) {
        throw new Error("第 " + (i + 1) + " 张:数据偏移超过 4GB,不可能装得下");
      }

      var name = String(it.name || ("img" + i));
      if (name.length >= NAME_MAX) {
        throw new Error("第 " + (i + 1) + " 张:名字最多 " + (NAME_MAX - 1) + " 个字符");
      }

      entries.push({
        offset: off, size: size, w: w, h: h, cf: cf, stridePad: pad,
        role: it.role || ROLE.Background, order: it.order | 0,
        name: name, pixels: px
      });
      off += size;
    }

    var dataBytes = off;
    var totalBytes = HEADER_SIZE + dataBytes;

    var blob = new Uint8Array(totalBytes);
    var dv = new DataView(blob.buffer);

    // ---- 头 12 字节 ----
    dv.setUint32(0, MAGIC, true);
    dv.setUint16(4, VERSION, true);
    dv.setUint16(6, entries.length, true);
    dv.setUint32(8, dataBytes, true);

    // ---- 每个索引项 44 字节 ----
    // 布局必须与 C 的 ImageEntry 完全一致:
    //   [0..3]   offset      u32
    //   [4..7]   size        u32    ← 两个 u32 挨着放,中间没有洞
    //   [8..9]   w           u16
    //   [10..11] h           u16
    //   [12]     cf          u8
    //   [13]     stride_pad  u8
    //   [14..15] role        u16
    //   [16..17] order       u16
    //   [18..41] name[24]
    //   合计 44 字节(44 是 4 的倍数,所以尾部也不用补)。
    // ★ 这里踩过坑:一度以为要 48 字节、还硬加了 2 字节尾部填充 ——
    //   实际 2 个 u16 只占 [16..17],name 从 18 紧接着开始,44 就够了。
    //   别手算对齐:C 侧有一条 offsetof/sizeof 断言(test_entry_field_offsets),
    //   JS 侧有"头部字段"那一节,两边把同一张表各钉一遍。
    for (var k = 0; k < entries.length; k++) {
      var e = entries[k];
      var base = 12 + k * ENTRY_SIZE;
      dv.setUint32(base + 0, e.offset, true);
      dv.setUint32(base + 4, e.size, true);
      dv.setUint16(base + 8, e.w, true);
      dv.setUint16(base + 10, e.h, true);
      blob[base + 12] = e.cf;
      blob[base + 13] = e.stridePad;
      dv.setUint16(base + 14, e.role, true);
      dv.setUint16(base + 16, e.order, true);
      // ★ 必须用 e.name。这里曾经写成上一轮循环遗留的 `name` 变量(var 不按块作用域),
      //   结果**每一项都被写成最后一张图的名字** —— 编译不报错、
      //   设备端只是"按名字找图找不到",极难查。测试里专门有一条防它。
      for (var c = 0; c < e.name.length && c < NAME_MAX - 1; c++) {
        blob[base + 18 + c] = e.name.charCodeAt(c) & 0xFF;
      }
      // 名字之后必须留 '\0':设备端靠它判断字符串合法
      blob[base + 18 + Math.min(e.name.length, NAME_MAX - 1)] = 0;
    }

    // ---- 像素数据 ----
    var pxOff = HEADER_SIZE;
    for (var m = 0; m < entries.length; m++) {
      blob.set(entries[m].pixels, pxOff + entries[m].offset);
    }

    return {
      blob: blob, entries: entries,
      dataBytes: dataBytes, totalBytes: totalBytes,
      headerBytes: HEADER_SIZE
    };
  }

  // ------------------------------------------------------------
  // esptool 刷写命令行(直接复制到终端就能用)
  // 波特率给 921600:1MB 用 115200 要一分半,太慢
  //
  // ★ --chip 必须跟着**目标板**走(2026-09-18):两块板的芯片不一样,
  //   拿 "--chip esp32" 去刷 S3 会被 esptool 当场拒掉
  //   ("Chip is ESP32-S3 ... but --chip esp32 was specified")。
  //   分区偏移两块板相同(0x254000),所以只有这一个是变量。
  // ------------------------------------------------------------
  function esptoolCommand(port, binPath, targetId) {
    var chip = (targetId === "s3") ? "esp32s3" : "esp32";
    return "python -m esptool --chip " + chip + " --port " + (port || "COM3") +
           " --baud 921600 write_flash 0x254000 " + (binPath || "image.bin");
  }

  // ------------------------------------------------------------
  // 生成 C 头文件(想把图直接编进固件时用)
  // ------------------------------------------------------------
  function toCHeader(blob, varName) {
    var name = varName || "g_image_blob";
    var lines = [];
    lines.push("// 由 tools/theme-editor 生成,请勿手改");
    lines.push("// 用法见 lib/themetool/image_blob.h");
    lines.push("#include <stdint.h>");
    lines.push("");
    lines.push("const uint8_t " + name + "[" + blob.length + "] = {");
    for (var i = 0; i < blob.length; i += 16) {
      var row = [];
      for (var j = 0; j < 16 && i + j < blob.length; j++) {
        row.push("0x" + blob[i + j].toString(16).padStart(2, "0"));
      }
      lines.push("  " + row.join(", ") + ",");
    }
    lines.push("};");
    lines.push("const uint32_t " + name + "_len = " + blob.length + ";");
    lines.push("");
    return lines.join("\n");
  }

  return {
    MAGIC: MAGIC, VERSION: VERSION, NAME_MAX: NAME_MAX, MAX_COUNT: MAX_COUNT,
    ENTRY_SIZE: ENTRY_SIZE, HEADER_SIZE: HEADER_SIZE,
    PARTITION_BYTES: PARTITION_BYTES,
    TARGETS: TARGETS, DEFAULT_TARGET: DEFAULT_TARGET,
    targetInfo: targetInfo, partitionBytesFor: partitionBytesFor,
    CF: CF, ROLE: ROLE, ROLE_NAMES: ROLE_NAMES,
    bytesPerPixel: bytesPerPixel,
    packedBytesPerPixel: packedBytesPerPixel,
    pack565: pack565,
    rgbaToRgb565: rgbaToRgb565,
    rgbaToRgb565A8: rgbaToRgb565A8,
    build: build,
    esptoolCommand: esptoolCommand,
    toCHeader: toCHeader
  };
});
