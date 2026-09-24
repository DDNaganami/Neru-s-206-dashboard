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
// 三个页面:两个编辑器在 tools/theme-editor/,预览页在 preview/
// ★ 2026-09-24:预览页变成**两张**(preview.html 纯帧播放;
//   preview-28c.html 是「2.8C(最终板)」档,页面上多一圈真机圆边虚线)。
//   两张都要查 —— 它们是"双击打开的静态页",语法错的表现同样是"一片空白"。
const pages = [
  path.join(dir, "index.html"),
  path.join(dir, "image-editor.html"),
  path.join(dir, "..", "..", "preview", "preview.html"),
  path.join(dir, "..", "..", "preview", "preview-28c.html")
];

let pass = 0, fail = 0;

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
    console.log("  ✗ " + page + " 里一个内联 <script> 都没找到(解析器坏了?)");
    fail++;
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

console.log("\n" + "=".repeat(56));
if (fail === 0) console.log("全部通过:" + pass + " 项");
else console.log("失败 " + fail + " 项");
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
