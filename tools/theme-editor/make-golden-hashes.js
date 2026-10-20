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
 *
 * ------------------------------------------------------------
 * ★★ 2026-10-15 晚：这一份指纹**改了**（主题格式修订 R2 + R3）
 * ------------------------------------------------------------
 * 触发它的需求（车主）：
 *   · R2「转速/速度现在就拆开」⇒ `readout` 多两个键 rpm_color / speed_color；
 *   · R3「底板深浅要能在编辑器里调」⇒ 再多一个键 plate_opa。
 * 三个键都进了 `index.html` 的 `defaultTheme()`（以及 ARC_FALLBACK）——
 * 而**这一份指纹算的就是把那份默认主题序列化出来的字节**（见上面 ①），
 * 所以它**必须**变：这不是"格式被改坏了"，而是"格式被有意扩了"（缺键回落，
 * 老主题照旧；旧的固件遇到新键只是跳过 —— 见 ui_theme.h 的 R2/R3 注记）。
 *
 *   指纹            改动前（2026-10-15 白天）                 改动后（本单）
 *   themeJson       2cf2c42432c211fb62aa05252a47ad7576278f2   1dcc7d40e7a77a090132c494f796d4a8
 *                   98e3e1f1bc5db67e3da439445b（2892 B）      64557d4ce02d612310459948226efa2e（2975 B）
 *   imageEditorTheme 5fdfd1b262ec37038f74e8cb2d79b13c19dff1   2c69e55a9b0bf3d8deee7600194161256d
 *   Json            1b17d8a8fee1c8ed4b8b5f2bdc（1389 B）       9358831d85eb7ed61d1b575fd8f719（1472 B）
 *   imageBin        24fc3c873d5c5641720f687f2edfd719d525d     **一个字没变**（同左）
 *   assetsBin       63fe3276be21c5cebd1031dcfcdbab8ff1806d     96e02893fdbf3aa4e49711bb1b3519cf
 *                   19f2fb1869655863663293271（295052 B）      4409183a2f7d43aeaed608f1c087747f（295052 B）
 *
 * ★★ `assetsBin` 为什么会变（**它必须变**，不是意外）：这个容器是
 *    「主题段(0x4000, 定长) + spiffs 空洞 + image 段」拼起来的，**主题段就在里面**
 *    ⇒ 主题 JSON 一变，整包的 sha 一定跟着变。**图片段逐字节没动**，这一点有取证
 *    （2026-10-15 晚实测，用本文件同一套打包调用跑旧/新两页）：
 *      · `imageBin` 的 sha 前后相同（同一份输入，见上面那一列）；
 *      · 容器里 [0x4000, end)（image 段 + 全 0 填充）前后**逐字节相同**；
 *      · 全部差异 1874 字节，范围 [665, 2974]，**完全落在主题段 [0, 0x4000) 内**。
 *    ⇒ 以后看到 assetsBin 变了，先看**变的是哪一段**（主题段变 = 主题改了；
 *      image 段变 = 图片编码改了），别把两者混成一句"指纹变了"。
 *
 * ★★ R4（2026-10-15 晚，车主："rpm / km/h 的字体颜色是不是漏掉了"）刻意**一个数都没改**：
 *    主读数单位改为跟随该屏的数字颜色（渲染侧 `build_readout()` 用 `digit_base_color(pk)`），
 *    `unit_color` 键保留、默认值仍是 `0x9AA0A6`，`defaultTheme()` 与 `ARC_FALLBACK` 的
 *    字段与取值一个字都没动 ⇒ **三份指纹（themeJson / imageBin / assetsBin）一个都没变**，
 *    上面那组数照旧。谁要是"顺手"把这一单的数改了，说明动到了默认值 —— 那不是本单该做的事。
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
  // ★ 与页面 themeFileText() 里那一句**逐字一致**（这就是"产物"的定义）。
  // ★★ 2026-10-17（主题格式修订 R5）：形状从 `{theme}` 变成
  //   `{"theme": …, "night": …}` —— 页面上"没动过夜间"时 night 是 `{}`
  //   （= 全继承白天，docs/NIGHT-MODE.md §8.1）。这里就按那个**默认状态**算：
  //   默认页面导出什么，指纹就记录什么。
  return new TextEncoder().encode(JSON.stringify({ theme: t, night: {} }, null, 2));
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
    // ★★ 2026-10-17（R5）：夜间底图（role 23）也要进指纹 —— 它是**新角色**，
    //    而 §8.2 明说"指纹必变：imageBinSha256 / assetsBinSha256"。
    //    不进夹具的话 imageBin 的字节与改动前**一模一样**，那条要求就没被任何东西钉住。
    { name: "bg_night", w: 64, h: 64, cf: IB.CF.RGB565, role: IB.ROLE.BackgroundNight, order: 0,
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
