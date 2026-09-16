/* ============================================================
 * theme-json.js —— 把"主题文件方言"读成对象
 *
 * 为什么不能直接 JSON.parse:
 *   主题文件是**给人手改**的,颜色按 `0xRRGGBB` 写最直观
 *   (参考文件 theme-default.json 全是这么写的,固件解析器也两种都吃:
 *    见 lib/themetool/theme_store.cpp 的 uintVal,它带 0x 分支)。
 *   但 `0x` 不是合法 JSON —— 直接 JSON.parse 会报
 *   "Expected ',' or '}' after property value",而报错信息**完全看不出**
 *   是颜色写法的问题。于是"固件能读、编辑器读不进来"这种最坑的错就出现了:
 *   同一个文件两边理解不一致。
 *
 * 做法:先把值位置上的 `0x...` 换成十进制,再交给 JSON.parse。
 *   只替换**紧跟在冒号后面**的 0x(也就是"值"的位置),
 *   所以字符串里的 0x 不会被误伤。
 *
 * 这个文件同时给浏览器(<script src>)和 Node(require)用:
 * 不写 import/export,只在末尾挂到 globalThis / module.exports。
 * 有单测:tools/theme-editor/test-theme-json.js
 * ============================================================ */
(function (root, factory) {
  "use strict";
  var api = factory();
  if (typeof module === "object" && module && module.exports) {
    module.exports = api;                 // Node
  }
  root.ThemeJson = api;                   // 浏览器
})(typeof globalThis !== "undefined" ? globalThis : this, function () {
  "use strict";

  // 把值位置上的 0x 十六进制换成十进制。
  // 正则要求 0x 前面是 `:` + 空白,于是 "name": "0x1234" 这种字符串值不受影响
  // (而主题文件里本来也没有字符串值 —— 这条是给将来加字段留的余地)。
  function stripHex(text) {
    return String(text).replace(
      /(:\s*)0[xX]([0-9a-fA-F]+)/g,
      function (_m, prefix, hex) {
        return prefix + String(parseInt(hex, 16));
      });
  }

  // 解析主题文件文本 → 对象。失败时抛出带原文的错(上层要显示给用户看)
  function parseThemeJson(text) {
    return JSON.parse(stripHex(text));
  }

  // 取 "theme" 里的对象;裸根({...} 直接写字段)也接受 —— 与固件同一套规则
  function themeObject(obj) {
    if (!obj || typeof obj !== "object") return obj;
    return (obj.theme && typeof obj.theme === "object") ? obj.theme : obj;
  }

  return {
    stripHex: stripHex,
    parseThemeJson: parseThemeJson,
    themeObject: themeObject
  };
});
