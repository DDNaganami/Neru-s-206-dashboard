/* ============================================================
 * make-golden-hashes.js —— 产出「打包产物指纹」这一份**证据文件**
 *
 *   node tools/theme-editor/make-golden-hashes.js            # 打印 + 写入
 *   node tools/theme-editor/make-golden-hashes.js --print     # 只打印
 *
 * ------------------------------------------------------------
 * 为什么要有它（2026-10-15「车主模式」那一单）
 * ------------------------------------------------------------
 * 那一单**不许改任何输出格式**：`theme.json` / `image.bin` / `assets.bin`
 * 的字节布局一个字都不能动，`theme-json.js`、`image-blob-build.js`、
 * `asset-package-core.js` 的**产物**必须与改动前**逐字节相同**。
 *
 * "我看着没改"不是证据。可执行的证据是：把**输入固定死**，跑一遍这三段打包，
 * 把结果的 SHA-256 记下来。谁以后改了任何会进字节的代码，这个哈希就会变，
 * `check-owner-mode.js` 当场红。
 *
 * ★ 输入是**算出来的**（不读 theme.json / local\*.bin 这些被 gitignore 的
 *   本机文件）—— 所以换一台机器、换一个仓库副本，哈希都一样。
 *   两处"确定性"特别说明：
 *     · 主题字节 = index.html 的 defaultTheme() **同一句**序列化
 *       （JSON.stringify({theme:T}, null, 2)）—— 字段顺序由对象字面量决定；
 *     · 图像素 = 一个纯函数式的图案（不依赖时间/随机数）。
 *
 * ★ 它**不写仓库里除 golden 那一份以外的任何文件**（默认写到本目录的
 *   .golden-hashes.json；--print 时一个文件都不写）。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

const dir = __dirname;
const TJ = require(path.join(dir, "theme-json.js"));
const IB = require(path.join(dir, "image-blob-build.js"));
const APC = require(path.join(dir, "asset-package-core.js"));

const GOLDEN_FILE = path.join(dir, ".golden-hashes.json");

function sha256(buf) {
  return crypto.createHash("sha256").update(Buffer.from(buf)).digest("hex");
}

// ------------------------------------------------------------
// ① 主题字节 —— 与 index.html 的 exportThemeJson()/saveOwnerPackage() **同一句**
//    ★ 这里**从 index.html 里把 defaultTheme() 抠出来求值**，不手抄一份：
//      抄一份就会在"改了页面忘了改这里"时静默分叉（本仓库的既有风格：
//      测试一律解析真源码，见 test-theme-json.js）。
//    ★ ★ 2026-10-15（配色 + 图片合并成一个页面之后）：
//      "图片页那份默认主题"（ARC_FALLBACK）也跟着并进了 index.html ——
//      image-editor.html 现在只是一个跳到 index.html#images 的**薄壳**，
//      所以两份都从 index.html 里抠。**指纹值一个都没变**（文字没动），
//      这一点由 check-owner-mode.js 的第 ⑬ 节逐字段对账。
// ------------------------------------------------------------
function themeDefaultFromPage(pageFile) {
  const src = fs.readFileSync(path.join(dir, pageFile), "utf8");
  const m = /function defaultTheme\(\)\s*\{([\s\S]*?)\n\}/.exec(src);
  if (!m) throw new Error(pageFile + " 里找不到 defaultTheme()");
  // 函数体里用到外部常量吗？目前不用；直接求值即可（去掉首行缩进即可编译）。
  return new Function(m[1].replace(/^\s*return\s*/, "return "))();
}

// ARC_FALLBACK 是**对象字面量**（键没引号、颜色写成 0x…）⇒ 去掉行注释后用
// new Function 求值（与 test-theme-json.js 那一节同一套办法）。
// ★ 合并后它住在 index.html（原来在图片页）。
function arcFallbackFromMergedPage(pageFile) {
  const src = fs.readFileSync(path.join(dir, pageFile), "utf8");
  const m = /const ARC_FALLBACK\s*=\s*\{([\s\S]*?)\n\};/.exec(src);
  if (!m) throw new Error(pageFile + " 里找不到 ARC_FALLBACK");
  return new Function("return {" + m[1].replace(/\/\/[^\n]*/g, "") + "};")();
}

function themeBytesOf(t) {
  // ★ 与页面 exportTheme() 里那一句**逐字一致**（这就是"产物"的定义）
  return new TextEncoder().encode(JSON.stringify({ theme: t }, null, 2));
}

// ------------------------------------------------------------
// ② 图像素 —— 固定图案（纯函数），覆盖两条编码路径：
//    · 表情（RGB565A8，带 alpha，3 字节/像素）
//    · 背景（RGB565，2 字节/像素）
// ------------------------------------------------------------
function patternRGBA(w, h, withAlpha) {
  const px = new Uint8Array(w * h * 4);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      const i = (y * w + x) * 4;
      px[i] = (x * 7 + y * 3) & 0xFF;
      px[i + 1] = (x * 5 + y * 11) & 0xFF;
      px[i + 2] = (x * 13 + y * 2) & 0xFF;
      px[i + 3] = withAlpha ? ((x + y) % 256) : 255;
    }
  }
  return px;
}

function imageBytes() {
  const items = [
    { name: "bg", w: 64, h: 64, cf: IB.CF.RGB565, role: IB.ROLE.Background, order: 0,
      pixels: IB.rgbaToRgb565(patternRGBA(64, 64, false), 64, 64) },
    { name: "face", w: 48, h: 48, cf: IB.CF.RGB565A8, role: IB.ROLE.FaceIdle, order: 0,
      pixels: IB.rgbaToRgb565A8(patternRGBA(48, 48, true), 48, 48) }
  ];
  return IB.build(items).blob;
}

// 与「② 图片」页 coreForTarget() **同值**的布局（偏移一样取自分区表）
function layout() {
  const themePartOffset = 0x210000;
  const themePartSize = APC.THEME_SECTION_BYTES;
  const spiffsBytes = 0x40000;
  const imagePartOffset = 0x254000;
  const imageBytesN = IB.partitionBytesFor(IB.DEFAULT_TARGET);
  return {
    base: themePartOffset,
    themeSectionBytes: themePartSize,
    spiffsBytes: spiffsBytes,
    betweenLabels: ["spiffs"],
    imageOffset: imagePartOffset - themePartOffset,
    maxImageBytes: imageBytesN,
    containerMin: imagePartOffset - themePartOffset,
    containerMax: imagePartOffset + imageBytesN - themePartOffset,
    themePart: { offset: themePartOffset, size: themePartSize, sizeText: "0x4000" },
    imagePart: { offset: imagePartOffset, size: imageBytesN,
                 sizeText: "0x" + imageBytesN.toString(16) }
  };
}

function computeAll() {
  const themeIdx = themeBytesOf(themeDefaultFromPage("index.html"));
  const themeImg = themeBytesOf(arcFallbackFromMergedPage("index.html"));
  const imgBytes = imageBytes();
  const packed = APC.packBytes({
    themeBytes: themeIdx,
    imageBytes: imgBytes,
    layout: layout(),
    themeLabel: "golden", imageLabel: "golden"
  });
  // 另跑一遍"用图片页那份默认主题打包"—— 两页产出的容器都要在指纹里
  const packedImg = APC.packBytes({
    themeBytes: themeImg,
    imageBytes: imgBytes,
    layout: layout(),
    themeLabel: "golden", imageLabel: "golden"
  });
  return {
    version: 1,
    note: "打包产物指纹：任何会进字节的改动都会让它变 —— 见 check-owner-mode.js",
    themeJsonBytes: themeIdx.length,
    themeJsonSha256: sha256(themeIdx),
    // 合并前"图片页那份默认主题"（ARC_FALLBACK）序列化后的字节
    //   ★ 字段名保留（指纹文件是**证据**，改名会让"前后两个值在说同一件事"看不出来）；
    //     只是它现在从合并后的 index.html 里抠。
    imageEditorThemeJsonBytes: themeImg.length,
    imageEditorThemeJsonSha256: sha256(themeImg),
    imageBinBytes: imgBytes.length,
    imageBinSha256: sha256(imgBytes),
    assetsBinBytes: packed.totalLen,
    assetsBinSha256: sha256(packed.bytes),
    assetsBinImagePageThemeBytes: packedImg.totalLen,
    assetsBinImagePageThemeSha256: sha256(packedImg.bytes),
    flashCommand: APC.flashCommand(packed.layout, "assets.bin", { chip: "esp32s3", port: "COM6" })
  };
}

const cur = computeAll();
if (process.argv.indexOf("--print") < 0) {
  fs.writeFileSync(GOLDEN_FILE, JSON.stringify(cur, null, 2) + "\n", "utf8");
  console.log("已写入 " + path.relative(path.join(dir, "..", ".."), GOLDEN_FILE));
}
console.log(JSON.stringify(cur, null, 2));
