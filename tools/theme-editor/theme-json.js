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

  // ------------------------------------------------------------
  // 三方合并 —— 让"本机记住的配色"不会挡住新的默认值
  //
  // 为什么需要它(实测踩过):
  //   编辑器会把配色记在本机(localStorage),这样刷新不丢。但那份记录是
  //   **整份主题**,固件默认值一改(比如水温弧从 145→330 改成 0→180),
  //   记录里的旧值就把它盖住了 —— 用户刷新后看到"什么都没变",
  //   还以为改的是固件。这不是固件的问题,是"存了整份"的问题。
  //
  // 解法:存记录时**连基线一起存**(base = 存的那一刻的固件默认值)。
  //   读回来时逐字段三方比对:
  //     用户改过的字段(base != ours)  → 保留用户的值
  //     用户没碰过的字段(base == ours) → 取**当前**的默认值(theirs)
  //   于是"没碰过的默认值"永远跟着固件走,用户的改动也不丢。
  //   没有基线(老记录)时保守处理:一律保留用户的值(ours)。
  // ------------------------------------------------------------
  function isPlainObject(v) {
    return v !== null && typeof v === "object" && !Array.isArray(v);
  }

  function deepEqual(a, b) {
    if (a === b) return true;
    if (typeof a !== typeof b) return false;
    if (Array.isArray(a) && Array.isArray(b)) {
      return a.length === b.length && a.every(function (x, i) { return deepEqual(x, b[i]); });
    }
    if (isPlainObject(a) && isPlainObject(b)) {
      const ka = Object.keys(a), kb = Object.keys(b);
      if (ka.length !== kb.length) return false;
      return ka.every(function (k) { return deepEqual(a[k], b[k]); });
    }
    return false;
  }

  // 逐字段三方合并。base 缺失时:**保留 ours**(没有基线就假设用户改过)。
  //
  // 数组特殊处理:主题里的 screens/arcs 是数组,而**差异形式**是"数字键的普通对象"
  // ({0:{...}})。所以三边都按"下标取值"来读,结果一律返回数组。
  function getIdx(container, i) {
    if (Array.isArray(container)) return container[i];
    if (isPlainObject(container)) return container[i];
    return undefined;
  }
  function idxCount(container) {
    if (Array.isArray(container)) return container.length;
    if (isPlainObject(container)) {
      let max = -1;
      Object.keys(container).forEach(function (k) {
        if (/^\d+$/.test(k)) max = Math.max(max, Number(k));
      });
      return max + 1;
    }
    return 0;
  }
  function looksLikeIndexMap(o) {
    if (!isPlainObject(o)) return false;
    const ks = Object.keys(o);
    return ks.length > 0 && ks.every(function (k) { return /^\d+$/.test(k); });
  }

  function mergeDefaults(base, ours, theirs) {
    const arrayish = Array.isArray(base) || Array.isArray(ours) || Array.isArray(theirs) ||
                     looksLikeIndexMap(ours) || looksLikeIndexMap(theirs);
    if (arrayish) {
      const n = Math.max(idxCount(base), idxCount(ours), idxCount(theirs));
      const out = [];
      for (let i = 0; i < n; i++) {
        const v = mergeDefaults(getIdx(base, i), getIdx(ours, i), getIdx(theirs, i));
        if (v !== undefined) out[i] = v;
      }
      return out;
    }

    if (isPlainObject(ours) && isPlainObject(theirs)) {
      const out = {};
      const keys = {};
      Object.keys(theirs).forEach(function (k) { keys[k] = 1; });
      Object.keys(ours).forEach(function (k) { keys[k] = 1; });
      Object.keys(keys).forEach(function (k) {
        out[k] = mergeDefaults(isPlainObject(base) ? base[k] : undefined, ours[k], theirs[k]);
      });
      return out;
    }

    if (ours === undefined) return theirs;      // 用户没有这个字段 → 用默认值
    if (theirs === undefined) return ours;      // 新默认值里没有了 → 保留用户的
    return deepEqual(ours, base) ? theirs : ours;
  }

  // 求 obj 相对 base 的**差异**(只留改过的叶子)。
  // 用途:把"用户改了什么"记下来,而不是把整份主题抄一遍 ——
  // 抄整份会让以后改默认值失效(见上面)。
  // 数组的差异用**数字键的普通对象**表示({0:{...},2:{...}}),因为 JSON 存不了
  // 稀疏数组(空位会变成 null,读回来会把默认值清掉)。
  function diffDeep(base, obj) {
    if (Array.isArray(obj) || isPlainObject(obj)) {
      const keys = Array.isArray(obj) ? obj.map(function (_v, i) { return String(i); })
                                      : Object.keys(obj);
      const out = {};
      let any = false;
      keys.forEach(function (k) {
        const bk = Array.isArray(base) ? base[Number(k)]
                 : (isPlainObject(base) ? base[k] : undefined);
        const d = diffDeep(bk, obj[k]);
        if (d !== undefined) { out[k] = d; any = true; }
      });
      return any ? out : undefined;
    }
    return deepEqual(base, obj) ? undefined : obj;
  }

  // 把差异合并回默认值,得到完整主题(不修改入参)
  function applyPatch(base, patch) {
    return mergeDefaults(base, base, patch);
  }

  return {
    stripHex: stripHex,
    parseThemeJson: parseThemeJson,
    themeObject: themeObject,
    isPlainObject: isPlainObject,
    deepEqual: deepEqual,
    mergeDefaults: mergeDefaults,
    diffDeep: diffDeep,
    applyPatch: applyPatch
  };
});
