/* ============================================================
 * syntax-check-pages.js —— 两个编辑器的内联脚本语法检查(Node 运行)
 *
 *   node tools/theme-editor/syntax-check-pages.js
 *
 * 为什么需要:编辑器是**双击打开的静态 HTML**,没有构建、没有 lint,
 * 语法错误的表现是"页面一片空白"或"点了没反应" —— 而不是一条报错。
 * 改 HTML 里的脚本时很容易漏个括号,这个检查把那种错挡在提交之前。
 *
 * 只做语法检查(不执行):把每个内联 <script> 的正文交给 new Function 编译。
 * 页面里的 DOM 调用不会被执行,所以这里不需要浏览器。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");

const dir = __dirname;
// 五个页面:两个编辑器在 tools/theme-editor/,两个预览页在 preview/,
// 统一入口(导航页)在 tools/web/。
// ★ 旧的三个页面 = 两个编辑器 + preview.html。
// ★ 2026-09-24:预览页变成**两张**(preview.html 纯帧播放;
//   preview-28c.html 是「2.8C(最终板)」档,页面上多一圈真机圆边虚线)。
//   两张都要查 —— 它们是"双击打开的静态页",语法错的表现同样是"一片空白"。
// ★ 2026-09-24 新增 tools/web/index.html(统一入口/导航页):
//   它是纯 HTML + 内联 CSS,**一段脚本都没有** —— 这是它的优点(没有"脚本挂了
//   就白页"这回事)。所以下面那条"至少要有一段内联脚本"的老检查对它不适用:
//   对导航页改为**反向断言**(必须真的没有内联脚本),这样以后谁往那一页塞脚本
//   都会被这条抓住,而不是被"没找到脚本 ⇒ 失败"误伤。
const pages = [
  path.join(dir, "index.html"),
  path.join(dir, "image-editor.html"),
  path.join(dir, "..", "..", "preview", "preview.html"),
  path.join(dir, "..", "..", "preview", "preview-28c.html"),
  // ★ 导航页(统一入口)。放在最后只为了让上面四页的顺序保持原样。
  path.join(dir, "..", "web", "index.html")
];
// 判断"是不是那一页导航页" —— 按**完整路径**认,不能只看 basename:
// 本目录下也有一个 index.html(主题编辑器),只看文件名会把两者混起来。
function isNavPage(file) {
  return /[\\/]web[\\/]index\.html$/i.test(file);
}

let pass = 0, fail = 0;

// 取出 <div class="navstrip"> 那一段(到与它配对的 </div> 为止)。
//
// ★ 为什么不能用正则懒匹配:① <style> 里也有 `.navstrip{...}` 这样的文本,
//   正则会把**样式规则**当成导航条(实测踩过);
//   ② 导航条末尾可能还有别的元素(image-editor.html 里那句「要传图片素材」的 span),
//   用"第一个 </div></div> 就算完"会**少读几个链接** ⇒ 检查变空转。
//   所以先找 <body> 之后再找 navstrip,并按 div 的开合**数深度**找配对。
function navStripOf(html) {
  const bodyAt = html.search(/<body\b/i);
  const from = bodyAt >= 0 ? bodyAt : 0;
  const at = html.indexOf('<div class="navstrip">', from);
  if (at < 0) return null;
  const re = /<\/?div\b[^>]*>/gi;
  re.lastIndex = at;
  let depth = 0, m;
  while ((m = re.exec(html)) !== null) {
    depth += (m[0][1] === "/") ? -1 : 1;
    if (depth === 0) return html.slice(at, m.index + m[0].length);
  }
  return null;
}

// 取出所有没有 src 的内联 <script> 正文
function inlineScripts(html) {
  const out = [];
  const re = /<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi;
  let m;
  while ((m = re.exec(html)) !== null) out.push(m[1]);
  return out;
}

for (const file of pages) {
  const page = path.basename(file);
  if (!fs.existsSync(file)) {
    console.log("  - " + page + " 不存在,跳过");
    continue;
  }
  const html = fs.readFileSync(file, "utf8");
  const pageDir = path.dirname(file);

  // ★ 开合必须配对。这条是踩过坑才加的:曾经在页面中间多出一个 </script>,
  //   于是后面几百行 JS 全跑到标签外面去了 —— 表现是"页面空白",
  //   而按"每个 <script> 都能编译"的老检查**完全查不出来**(它们确实都能编译)。
  const opens = (html.match(/<script\b/gi) || []).length;
  const closes = (html.match(/<\/script>/gi) || []).length;
  if (opens !== closes) {
    fail++;
    console.log("  ✗ " + page + " 的 <script> 开合不配对:" + opens + " 个开、" + closes + " 个闭");
  } else {
    pass++;
  }

  const scripts = inlineScripts(html);
  if (scripts.length === 0) {
    if (isNavPage(file)) {
      // 导航页就该没有脚本(纯 HTML/CSS)—— 这是**通过**,不是失败。
      pass++;
      console.log("  " + page + ":0 段内联脚本(导航页,预期如此)");
    } else {
      console.log("  ✗ " + page + " 里一个内联 <script> 都没找到(解析器坏了?)");
      fail++;
    }
    continue;
  }
  if (isNavPage(file)) {
    // 反向断言:有人往导航页里加脚本时,这一条会说话(而不是静静地少测一页)
    fail++;
    console.log("  ✗ " + page + "(导航页)本不该有脚本,却找到 " + scripts.length +
                " 段内联脚本 —— 它要能离线双击打开,别在这里加 JS");
    continue;
  }
  scripts.forEach((src, i) => {
    try {
      // 包一层:允许顶层 return/await 之外的正常脚本正文(与浏览器同一套语法)
      new Function(src);
      pass++;
    } catch (e) {
      fail++;
      console.log("  ✗ " + page + " 第 " + (i + 1) + " 段内联脚本语法错误:" + e.message);
    }
  });
  console.log("  " + page + ":" + scripts.length + " 段内联脚本");
}

// 页面引用的本地 .js 必须真的存在 —— 漏拷一个文件的表现同样是"页面空白"
console.log("\n== 页面引用的本地脚本都在");
for (const file of pages) {
  const page = path.basename(file);
  if (!fs.existsSync(file)) continue;
  const pageDir = path.dirname(file);
  const html = fs.readFileSync(file, "utf8");
  const re = /<script[^>]*\bsrc="([^"]+)"/gi;
  let m;
  while ((m = re.exec(html)) !== null) {
    if (/^https?:/i.test(m[1])) continue;
    const p = path.join(pageDir, m[1]);   // 相对页面自身目录,与浏览器一致
    if (fs.existsSync(p)) pass++;
    else { fail++; console.log("  ✗ " + page + " 引用了不存在的 " + m[1]); }
  }
}

// ============================================================
// 统一导航条里的链接必须**真的指向存在的文件**
//
// 为什么值得单测一条:四页分布在两个目录里(tools/theme-editor/ 与 preview/),
// 深度不同 ⇒ 相对路径不一样(`../web/index.html` 对 `../../tools/web/index.html`)。
// 写错的症状是"点了没反应/404",而这在静态页上**不报任何错**;
// 而且四页要能**离线双击打开**,所以链接一律相对路径,不许 http(s)。
// ============================================================
console.log("\n== 导航条链接指向的文件都在");
for (const file of pages) {
  const page = path.basename(file);
  if (!fs.existsSync(file)) continue;
  // ★ 导航页**自己就是首页**:它不需要"返回导航页"，也不需要那条页内导航条
  //   （它靠四张卡片进四页）。所以这一段对它是"不适用"，不是失败 ——
  //   少了这一句，检查会在一个**正确的**页面上报假红。
  if (isNavPage(file)) {
    console.log("  " + page + "（导航页/首页本身：不需要导航条与返回键）");
    continue;
  }
  const pageDir = path.dirname(file);
  const html = fs.readFileSync(file, "utf8");
  // 只取导航条那一段,免得把页面正文里的别的东西也当成导航链接。
  // （提取逻辑见上面的 navStripOf()：按 div 深度找配对，不用正则懒匹配。）
  const strip = navStripOf(html);
  if (!strip) {
    fail++;
    console.log("  ✗ " + page + " 里找不到统一导航条(<div class=\"navstrip\">)");
    continue;
  }
  const re = /<a[^>]*\bhref="([^"]+)"/gi;
  let m, n = 0;
  while ((m = re.exec(strip)) !== null) {
    const href = m[1];
    if (/^https?:/i.test(href)) {
      fail++;
      console.log("  ✗ " + page + " 的导航条里有外部链接 " + href + "(这些页面要能离线打开)");
      continue;
    }
    if (/^#/.test(href)) continue;          // 页内锚点不用查
    const p = path.join(pageDir, href);     // 相对页面自身目录,与浏览器一致
    if (fs.existsSync(p)) { pass++; n++; }
    else { fail++; console.log("  ✗ " + page + " 的导航条指向了不存在的 " + href); }
  }
  // 非导航页必须有"返回导航页"那一条(车主口径:要能频繁来回跳)
  if (/class="navback"[^>]*href="[^"]*web\/index\.html"|href="[^"]*web\/index\.html"[^>]*class="navback"/i
      .test(strip)) {
    pass++;
  } else {
    fail++;
    console.log("  ✗ " + page + " 的导航条里没有「返回导航页」那条链接");
  }
  console.log("  " + page + ":导航条 " + n + " 个链接 + 1 个「← 返回导航页」");
}

console.log("\n" + "=".repeat(56));
if (fail === 0) console.log("全部通过:" + pass + " 项");
else console.log("失败 " + fail + " 项");
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
