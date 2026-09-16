/* ============================================================
 * build-image-bin.js —— 命令行打包器(生成 image.bin)
 *
 * 两种用法:
 *
 * 1) 用一份简单的"配方"JSON 打包(给自动化测试用,也是本文件的主用途):
 *      node build-image-bin.js --spec spec.json --out image.bin
 *    配方里可以写:
 *      { "images": [ { "pattern": "ramp", "w":4, "h":2, "role":"background",
 *                      "order":0, "name":"grad" } ] }
 *    pattern 取值见下方的 makePattern()。
 *
 * 2) 直接打包已经转好的原始 RGB565 数据(给其它工具链用):
 *      node build-image-bin.js --raw grad.raw:480x480:rgb565:background:0:grad \
 *                              --out image.bin
 *
 * 图形界面的版本在 index.html —— 拖图片进去、点导出,不用命令行。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");
const IB = require("./image-blob-build.js");

// ---------------- 造图案 ----------------
// 每种图案都刻意带上"位置信息"(颜色随 x/y 变化),
// 这样一旦行序、列序或 stride 弄错,逐字节对比就会立刻发现,
// 而不是"看起来是张图"就蒙过去。
function makePattern(kind, w, h) {
  const rgba = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      let r, g, b, a = 255;
      switch (kind) {
        case "ramp":      // 横向渐变
          r = Math.round(255 * x / Math.max(1, w - 1));
          g = Math.round(255 * y / Math.max(1, h - 1));
          b = 0x40;
          break;
        case "checker":   // 棋盘格
          r = ((x + y) & 1) ? 255 : 0;
          g = ((x >> 1) ^ (y >> 1)) & 1 ? 200 : 20;
          b = (x * 8) & 0xFF;
          break;
        case "solid":
          r = 0x12; g = 0x34; b = 0x56;
          break;
        case "transparent":  // 带 alpha,测混合
          r = 255; g = 0; b = 0;
          a = (x * 255) / Math.max(1, w - 1) | 0;
          break;
        default:
          throw new Error("不认识的图案: " + kind);
      }
      rgba[i] = r; rgba[i + 1] = g; rgba[i + 2] = b; rgba[i + 3] = a;
    }
  }
  return rgba;
}

// 角色名 → 编号(与 image_blob.h 的 ImageRole 一致)
// ★ 这里只列**当前存在**的角色。当年那些已删除的角色(开机帧 boot、
//   左屏惊喜 face_surprise、右屏红区 face_redline_r)对应编号是**保留号**,
//   一律不复用 —— 所以它们既不在这里、也不该出现在任何 spec 里:
//   传进来会直接报"不认识的角色",而不是悄悄生成一个编号为 undefined 的项
//   (那会生成一个能通过字节校验、但角色是垃圾的 blob)。
const ROLE_ID = {
  background: IB.ROLE.Background,
  // 左屏(转速表)
  face_idle: IB.ROLE.FaceIdle,
  face_cruise: IB.ROLE.FaceCruise,
  face_sport: IB.ROLE.FaceSport,
  face_redline: IB.ROLE.FaceRedline,
  // 右屏(速度表)
  face_idle_r: IB.ROLE.FaceIdleR,
  face_cruise_r: IB.ROLE.FaceCruiseR,
  face_sport_r: IB.ROLE.FaceSportR,
  // 超速(第 4 档,当年叫"惊喜")。旧名保留成别名:编号没变(都是 8),
  // 老 spec.json 里的 face_surprise_r 仍然能打包,只是语义变成"超速"。
  face_overspeed_r: IB.ROLE.FaceOverspeedR,
  face_surprise_r: IB.ROLE.FaceOverspeedR
};

function roleId(v) {
  if (typeof v === "number") return v;
  const k = String(v || "background").toLowerCase();
  if (!(k in ROLE_ID)) throw new Error("不认识的角色: " + v);
  return ROLE_ID[k];
}

// ---------------- manifest ----------------
// 与 test_image_roundtrip.cpp 约定的纯文本格式。
// 注意:这里的字段是**数据模型**层面的(offset/size 由打包器算出来),
// 不是照着二进制布局反推的 —— 否则测试就成了"自己抄自己"。
function manifestText(res, binPath) {
  const L = [];
  L.push("# image.bin manifest —— 由 build-image-bin.js 生成");
  L.push("# 用途:test_image_roundtrip.cpp 用它和固件解析器的结果逐字节对账");
  L.push("# 不要手改;改了要和 lib/themetool/image_blob.h 一起改");
  L.push("");
  L.push("magic=0x" + IB.MAGIC.toString(16));
  L.push("version=" + IB.VERSION);
  L.push("count=" + res.entries.length);
  L.push("header_bytes=" + res.headerBytes);
  L.push("data_bytes=" + res.dataBytes);
  L.push("total_bytes=" + res.totalBytes);
  L.push("");
  res.entries.forEach((e, i) => {
    L.push("[img" + i + "]");
    L.push("img" + i + ".name=" + e.name);
    L.push("img" + i + ".w=" + e.w);
    L.push("img" + i + ".h=" + e.h);
    L.push("img" + i + ".cf=0x" + e.cf.toString(16));
    L.push("img" + i + ".stride_pad=" + e.stridePad);
    L.push("img" + i + ".size=" + e.size);
    L.push("img" + i + ".offset=" + e.offset);
    L.push("img" + i + ".role=" + e.role);
    L.push("img" + i + ".order=" + e.order);
    // 角色的**名字**也要写出来:编号是三方共用契约(界面下拉框/打包器/固件),
    // C 侧有一张同样的编号→名字表,两边对不上就说明有人只改了一边。
    L.push("img" + i + ".role_name=" + (IB.ROLE_NAMES[e.role] || "?"));
    // 每行的字节,逗号分隔的两位十六进制
    const stride = IB.bytesPerPixel(e.cf) * e.w + e.stridePad;
    for (let r = 0; r < e.h; r++) {
      const parts = [];
      for (let c = 0; c < stride; c++) {
        parts.push(e.pixels[r * stride + c].toString(16).padStart(2, "0"));
      }
      L.push("row" + r + "=" + parts.join(","));
    }
    L.push("");
  });
  L.push("# 生成的是二进制镜像,目标是设备 flash 的 image 分区:");
  L.push("#   " + IB.esptoolCommand("COM3", path.basename(binPath || "image.bin")));
  L.push("");
  return L.join("\n");
}

// ---------------- 参数解析 ----------------
function parseArgs(argv) {
  const o = { spec: null, out: "image.bin", raw: [], manifest: null };
  for (let i = 2; i < argv.length; i++) {
    const a = argv[i];
    if (a === "--spec") o.spec = argv[++i];
    else if (a === "--out") o.out = argv[++i];
    else if (a === "--manifest") o.manifest = argv[++i];
    else if (a === "--raw") o.raw.push(argv[++i]);
    else if (a === "--help" || a === "-h") o.help = true;
    else throw new Error("不认识的参数: " + a);
  }
  return o;
}

// --raw name.raw:480x480:rgb565:background:0:name
function parseRawSpec(s) {
  const p = s.split(":");
  if (p.length < 3) throw new Error("--raw 至少要有 文件:宽x高:颜色格式");
  const m = /^(\d+)x(\d+)$/.exec(p[1]);
  if (!m) throw new Error("尺寸要写成 宽x高,例如 480x480");
  const cfName = p[2].toLowerCase();
  const cfMap = { rgb565: IB.CF.RGB565, rgb888: IB.CF.RGB888, a8: IB.CF.A8, l8: IB.CF.L8 };
  if (!(cfName in cfMap)) throw new Error("不认识的颜色格式: " + cfName);
  return {
    file: p[0], w: +m[1], h: +m[2], cf: cfMap[cfName],
    role: roleId(p[3] || "background"),
    order: p[4] ? +p[4] : 0,
    name: p[5] || path.basename(p[0]).replace(/\.[^.]+$/, "")
  };
}

function main() {
  const args = parseArgs(process.argv);
  if (args.help) {
    console.log("用法:");
    console.log("  node build-image-bin.js --spec spec.json --out image.bin");
    console.log("  node build-image-bin.js --raw a.raw:480x480:rgb565:background:0:bg --out image.bin");
    console.log("");
    console.log("图案类型(配方里的 pattern): ramp | checker | solid | transparent");
    return 0;
  }

  const items = [];

  if (args.spec) {
    const spec = JSON.parse(fs.readFileSync(args.spec, "utf8"));
    for (const im of spec.images || []) {
      const w = im.w | 0, h = im.h | 0;
      const rgba = makePattern(im.pattern || "ramp", w, h);
      const pad = im.stride_pad | 0;
      const pixels = IB.rgbaToRgb565(rgba, w, h, {
        stridePad: pad,
        alphaBg: im.alpha_bg || null
      });
      items.push({
        name: im.name || ("img" + items.length),
        w, h, cf: IB.CF.RGB565, stridePad: pad,
        role: roleId(im.role), order: im.order | 0, pixels
      });
    }
  }

  for (const r of args.raw) {
    const s = parseRawSpec(r);
    const buf = fs.readFileSync(s.file);
    const bpp = IB.bytesPerPixel(s.cf);
    const need = bpp * s.w * s.h;
    if (buf.length !== need) {
      throw new Error(`${s.file}: 大小 ${buf.length} 字节,${s.w}x${s.h} ${bpp}B/px 需要 ${need} 字节`);
    }
    items.push({
      name: s.name, w: s.w, h: s.h, cf: s.cf, stridePad: 0,
      role: s.role, order: s.order, pixels: new Uint8Array(buf)
    });
  }

  if (!items.length) {
    console.error("没有要打包的图片:请给 --spec 或 --raw");
    return 2;
  }

  const res = IB.build(items);
  fs.writeFileSync(args.out, Buffer.from(res.blob));

  const manPath = args.manifest || (args.out + ".manifest");
  fs.writeFileSync(manPath, manifestText(res, args.out), "utf8");

  console.log("已生成 " + args.out + "  (" + res.totalBytes + " 字节)");
  console.log("       " + manPath + "  (对账清单)");
  console.log("  头 " + res.headerBytes + " 字节 + 像素 " + res.dataBytes + " 字节,共 " +
              res.entries.length + " 张图");
  const pct = (100 * res.totalBytes / IB.PARTITION_BYTES).toFixed(1);
  console.log("  占 image 分区 " + pct + "%  (" + res.totalBytes + "/" +
              IB.PARTITION_BYTES + " 字节)");
  if (res.totalBytes > IB.PARTITION_BYTES) {
    console.error("✗ 超出分区大小,刷进去会被截断!");
    return 3;
  }
  console.log("  刷写: " + IB.esptoolCommand("COM3", path.basename(args.out)));
  return 0;
}

if (require.main === module) {
  try {
    process.exit(main());
  } catch (e) {
    console.error("失败: " + e.message);
    process.exit(1);
  }
}

module.exports = { makePattern, manifestText, parseRawSpec, roleId };
