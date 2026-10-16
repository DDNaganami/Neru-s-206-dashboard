/* ============================================================
 * smoke-pages.js —— 用**极简 DOM 桩**把编辑器页面真的跑一遍（Node 运行）
 *
 *   node tools/theme-editor/smoke-pages.js
 *   退出码：0 = 跑通；1 = 启动/交互时抛错
 *
 * ------------------------------------------------------------
 * 为什么值得有它（而 syntax-check-pages.js 不够）
 * ------------------------------------------------------------
 * syntax-check-pages.js 只做**语法**检查（new Function 编译一遍）。
 * 但这两个页面的故障大多是**运行时**的，而且表现都一样：白屏 / 点了没反应 ——
 *   · 顶层就调用还没声明的 const（TDZ）或重复声明（同一个 let 写两遍）；
 *   · 初始化时调的 DOM 不存在（getElementById 返回 null ⇒ 后面 .value 抛错）；
 *   · render() 在 build* 之前跑，读到 undefined 就炸。
 * 这些在浏览器里是**一条控制台报错 + 整页不动**，而本仓库的测试是 Node 侧的，
 * 没有浏览器。所以这里塞一个最小 DOM 桩：够页面把它那段内联脚本从头跑到尾，
 * 再按车主模式的口径点几下（改色 / 切分区 / 切模式 / 套预设 / 点保存），
 * 看有没有抛错。
 *
 * ★★ 2026-10-15（车主口径变更）：配色页与图片页**合并成一个页面**
 *   ⇒ 这里改成"跑**一个**页面、查**两个**分区"，另外单独断言
 *     image-editor.html 那个**重定向薄壳**（几行、没有脚本、带 meta refresh）。
 *
 * ★ 它**不判像素**（canvas 是空壳，画不出东西）—— 像素判据在 check-*.js
 *   那一批里（它们读真实落帧）。这里只判"页面能不能活着跑起来"。
 * ★ 桩里凡是页面用到的方法都要有；缺一个就会抛 TypeError，
 *   而那正是"真机上白屏"的同一种错 —— 所以这个桩**故意不宽容**。
 * ============================================================ */
"use strict";

const fs = require("fs");
const path = require("path");
const vm = require("vm");
// ★ 真实内核（与页面 <script src> 里那几份**同一批文件**）：
//   导入那一条用例要**自己造一个极小的 assets.bin 夹具**（不往仓库里塞二进制）。
const APC = require(path.join(__dirname, "asset-package-core.js"));
const IB = require(path.join(__dirname, "image-blob-build.js"));

const dir = __dirname;
// canvas 桩上"画了几笔"的计数：用来证明"导入之后预览真的重画了"
//   （桩不判像素 —— 像素判据在 check-*.js 那一批里，见文件头）。
let ctxDraws = 0;
let pass = 0, fail = 0;
const failures = [];
function ok(cond, what) {
  if (cond) pass++;
  else { fail++; failures.push(what); console.log("  ✗ " + what); }
}
function eq(a, b, what) {
  ok(a === b, what + "  (期望 " + JSON.stringify(b) + "，得到 " + JSON.stringify(a) + ")");
}
function section(t) { console.log("\n== " + t); }

// ------------------------------------------------------------
// 极简 DOM 桩
// ------------------------------------------------------------
function makeEl(tag, doc) {
  const el = {
    tagName: String(tag || "div").toUpperCase(),
    children: [],
    childNodes: [],
    attributes: {},
    style: { setProperty: function () {}, cssText: "" },
    dataset: {},
    classList: {
      _s: new Set(),
      add: function (c) { this._s.add(c); },
      remove: function (c) { this._s.delete(c); },
      contains: function (c) { return this._s.has(c); },
      toggle: function (c, force) {
        const want = (force === undefined) ? !this._s.has(c) : !!force;
        if (want) this._s.add(c); else this._s.delete(c);
        return want;
      }
    },
    _text: "",
    _html: "",
    value: "",
    checked: false,
    disabled: false,
    selected: false,
    title: "",
    type: "",
    id: "",
    className: "",
    placeholder: "",
    readOnly: false,
    accept: "",
    min: 0, max: 0, step: 1,
    width: 300, height: 150,
    naturalWidth: 0, naturalHeight: 0,
    clientWidth: 460, clientHeight: 460,
    parentElement: null,
    _events: {},
    appendChild: function (c) { c.parentElement = el; el.children.push(c); return c; },
    append: function () { for (let i = 0; i < arguments.length; i++) el.appendChild(arguments[i]); },
    removeChild: function (c) { const i = el.children.indexOf(c); if (i >= 0) el.children.splice(i, 1); return c; },
    insertBefore: function (c) { return el.appendChild(c); },
    setAttribute: function (k, v) { el.attributes[k] = String(v); if (k === "id") el.id = String(v); },
    getAttribute: function (k) { return Object.prototype.hasOwnProperty.call(el.attributes, k) ? el.attributes[k] : null; },
    removeAttribute: function (k) { delete el.attributes[k]; },
    addEventListener: function (ev, fn) { (el._events[ev] = el._events[ev] || []).push(fn); },
    removeEventListener: function () {},
    dispatch: function (ev, arg) { (el._events[ev] || []).forEach(fn => fn(arg || { preventDefault: function () {} })); },
    click: function () { el.dispatch("click", { preventDefault: function () {} }); },
    focus: function () {}, blur: function () {}, select: function () {},
    setPointerCapture: function () {}, releasePointerCapture: function () {},
    getBoundingClientRect: function () { return { left: 0, top: 0, width: el.clientWidth, height: el.clientHeight }; },
    getContext: function () { return makeCtx(); },
    toDataURL: function () { return "data:image/png;base64,"; },
    // 够用的 querySelector：只支持 "#id" / "tag" / ".class"（页面用到的就这几种）
    querySelector: function (sel) {
      return findIn(el, sel);
    },
    querySelectorAll: function (sel) {
      const out = [];
      walk(el, function (n) { if (matches(n, sel)) out.push(n); });
      return out;
    },
    closest: function () { return null; },
    get firstChild() { return el.children[0] || null; },
    get lastChild() { return el.children[el.children.length - 1] || null; }
  };
  Object.defineProperty(el, "textContent", {
    get: function () { return el._text; },
    set: function (v) { el._text = String(v); el.children.length = 0; }
  });
  Object.defineProperty(el, "innerHTML", {
    get: function () { return el._html; },
    set: function (v) { el._html = String(v); el.children.length = 0; }
  });
  return el;
}

// 极简选择器：够页面用（"#id" / "tag" / ".class"）
function matches(el, sel) {
  sel = String(sel).trim();
  if (sel.charAt(0) === "#") return el.id === sel.slice(1);
  if (sel.charAt(0) === ".") return (" " + el.className + " ").indexOf(" " + sel.slice(1) + " ") >= 0;
  return el.tagName === sel.toUpperCase();
}
function walk(el, fn) {
  (el.children || []).forEach(c => { fn(c); walk(c, fn); });
}
function findIn(el, sel) {
  let out = null;
  walk(el, function (n) { if (!out && matches(n, sel)) out = n; });
  return out;
}

function makeCtx() {  const noop = function () {};
  return {
    canvas: { width: 300, height: 150 },
    setTransform: noop, clearRect: function () { ctxDraws++; }, fillRect: function () { ctxDraws++; },
    strokeRect: noop,
    beginPath: noop, closePath: noop, arc: noop, moveTo: noop, lineTo: noop,
    stroke: noop, fill: function () { ctxDraws++; }, save: noop, restore: noop, translate: noop,
    scale: noop, rotate: noop, roundRect: noop,
    fillText: function () { ctxDraws++; }, strokeText: function () { ctxDraws++; },
    drawImage: function () { ctxDraws++; }, putImageData: function () { ctxDraws++; },
    measureText: function () { return { width: 10 }; },
    createImageData: function (w, h) { return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }; },
    getImageData: function (x, y, w, h) { return { width: w, height: h, data: new Uint8ClampedArray(w * h * 4) }; },
    globalAlpha: 1, fillStyle: "#000", strokeStyle: "#000", lineWidth: 1,
    font: "", textAlign: "", textBaseline: "", imageSmoothingEnabled: true,
    imageSmoothingQuality: "high"
  };
}

// 记录"页面上出现过哪些 id / 文本"，供判据使用
function makeDoc(html) {
  const doc = makeEl("document");
  doc._byId = {};
  doc._byCap = [];                 // 没有 id、但有 data-ocap 的静态元素（见 seedStaticIds）
  doc.ids = [];
  doc.createElement = function (tag) { return makeEl(tag, doc); };
  doc.createElementNS = function (ns, tag) { return makeEl(tag, doc); };
  doc.createTextNode = function (t) { const e = makeEl("#text"); e.textContent = t; return e; };
  doc.getElementById = function (id) { return doc._byId[id] || null; };
  // ★ 桩里的 document 没有 HTML 解析器 ⇒ 静态元素只是"按 id 建出来的空壳"，
  //   挂在 body 下面（`walk` 走不到它们）。所以 document.querySelector 要**也查
  //   那张 id 表**，否则 `doc.querySelector('[data-ocap="legend"]')` 永远拿不到 ——
  //   那会让断言变成"查不到 ⇒ 页面错了"的假红（实测踩过）。
  doc.querySelector = function (sel) {
    const s = String(sel).trim();
    const attr = /^\[([\w-]+)(?:=["']?([^"'\]]+)["']?)?\]$/.exec(s);
    if (attr) {
      const k = attr[1], v = attr[2];
      for (const el of doc._byCap) {
        const got = el.getAttribute(k);
        if (got !== null && (v === undefined || got === v)) return el;
      }
      for (const id of Object.keys(doc._byId)) {
        const el = doc._byId[id];
        const got = el.getAttribute(k);
        if (got !== null && (v === undefined || got === v)) return el;
      }
      return null;
    }
    const byId = s.charAt(0) === "#" ? doc._byId[s.slice(1)] : null;
    if (byId) return byId;
    return findIn(doc.body, s);
  };
  doc.querySelectorAll = function () { return []; };
  doc.body = makeEl("body", doc);
  doc.documentElement = makeEl("html", doc);
  doc.head = makeEl("head", doc);
  doc.addEventListener = function () {};
  return doc;
}

// 把页面里所有带 id 的静态元素先"建"出来（getElementById 要用）
function seedStaticIds(doc, html) {
  const re = /<([a-zA-Z][\w-]*)([^>]*?)\bid="([^"]+)"([^>]*?)>/g;
  let m;
  while ((m = re.exec(html)) !== null) {
    const tag = m[1], id = m[3];
    if (doc._byId[id]) continue;
    const el = makeEl(tag, doc);
    el.id = id;
    // 静态属性照抄（type/value/min/max/step/checked/data-ocap 这些页面要用）
    const attrs = (m[2] + " " + m[4]);
    const ar = /([a-zA-Z-]+)="([^"]*)"/g;
    let a;
    while ((a = ar.exec(attrs)) !== null) {
      el.attributes[a[1]] = a[2];
      if (a[1] === "type") el.type = a[2];
      if (a[1] === "value") el.value = a[2];
      if (a[1] === "min") el.min = +a[2];
      if (a[1] === "max") el.max = +a[2];
      if (a[1] === "step") el.step = +a[2];
    }
    doc._byId[id] = el;
  }
  // ★ 页面对**子元素**也有依赖，而且是最典型的白屏来源：
  //     `bar.querySelector("i").style.width = …`（占用条里那根 i）。
  //   真浏览器里那个 <i> 是 HTML 解析出来的；桩里没有解析器 ⇒ 按页面用到的
  //   关系显式补上（就这么一处，别扩散成半个解析器）。
  const bar = doc._byId["budget-bar"];
  if (bar) bar.appendChild(makeEl("i", doc));

  // ★ **没有 id、但有 data-ocap 的静态元素**（例如配色区那行说明）也建一份：
  //   它们没进 _byId，而桩里的 body 又没有子节点 ⇒ 按属性查会永远查不到。
  //   只收 `data-ocap=...` 这一种属性（就是测试要按属性点名的那些），
  //   不当成通用 HTML 解析器。
  const reCap = /<([a-zA-Z][\w-]*)\b[^>]*\bdata-ocap="([a-z]+)"[^>]*>/g;
  while ((m = reCap.exec(html)) !== null) {
    const el = makeEl(m[1], doc);
    el.attributes["data-ocap"] = m[2];
    doc._byCap.push(el);
  }
}

// ------------------------------------------------------------
// ★★ 2026-10-15（车主："光有导出，没有导入 bin 啊？" / "没有看见图片载入"）：
//    造一个**极小**的 assets.bin 夹具来喂"打开安装包…"那条路 ——
//    **内联在这里，不往仓库里塞二进制**。
//    内容（两条项，两条编码路都覆盖）：
//      · 背景 8×8 / RGB565（不透明路）
//      · 一方表情 4×4 / RGB565A8（带 alpha 路），名字故意用**老包那种截断字节**
//        （不是合法 UTF-8 —— 真实文件里的中文名就是这样，见 image-blob-build.js
//         的 nameToBytes）⇒ 往返要逐字节相同就必须把原始名字字节写回去。
//    ★ 布局**问页面自己**（`coreForTarget()`）：目标板是页面选的，
//      在测试里另抄一份常量会变成"页面换了默认板 ⇒ 夹具被当成不是容器"。
// ------------------------------------------------------------
function buildTinyContainer(sandbox) {
  const layout = sandbox.coreForTarget();
  const themeBytes = new TextEncoder().encode(JSON.stringify(
    { theme: { bg_color: 0x123456, readout: { rpm_color: 0xAB12CD } } }, null, 2));

  const px = new Uint8Array(8 * 8 * 4);
  for (let i = 0; i < px.length; i += 4) {
    px[i] = 10; px[i + 1] = 20; px[i + 2] = 30; px[i + 3] = 255;   // 纯色 RGBA
  }
  const face = new Uint8Array(4 * 4 * 4);
  for (let i = 0; i < face.length; i += 4) {
    face[i] = 200; face[i + 1] = 100; face[i + 2] = 50; face[i + 3] = (i / 4) % 2 ? 255 : 0;
  }
  const items = [
    { name: "bg", w: 8, h: 8, cf: IB.CF.RGB565, role: IB.ROLE.Background, order: 0,
      pixels: IB.rgbaToRgb565(px, 8, 8) },
    { name: "l\u001fh-1-\uFFFD", w: 4, h: 4, cf: IB.CF.RGB565A8, role: IB.ROLE.FaceIdle,
      order: 0, pixels: IB.rgbaToRgb565A8(face, 4, 4),
      // ★ 老包里的名字字节：不是合法 UTF-8（0x6C 0x1F 0x68 …）——
      //   nameRaw 走"原样写回"，往返才可能逐字节相同
      nameRaw: [0x6C, 0x1F, 0x68, 0x2D, 0x31, 0x2D, 0xE6, 0x4F, 0x5F, 0x5F, 0x5F] }
  ];
  const img = IB.build(items).blob;
  const packed = APC.packBytes({ themeBytes: themeBytes, imageBytes: img, layout: layout,
                                 themeLabel: "fixture", imageLabel: "fixture" });
  // ★ packBytes 返回的 `imageBytes` 是**长度**（不是字节）—— 夹具自己留着那份 blob
  return { bytes: new Uint8Array(packed.bytes), layout: layout,
           imageBytes: new Uint8Array(img),
           info: APC.parseImageBlob(new Uint8Array(img), { label: "fixture" }) };
}

// ------------------------------------------------------------
// 点「保存安装包」并把导出的字节接回来（桩里 Blob 留住 parts、URL 换成钩子）。
//   ★ 用**真按钮**：这一条要覆盖的就是"车主会走的那条路"。
// ------------------------------------------------------------
function exportViaButton(r, doc) {
  let captured = null;
  const real = r.sandbox.URL.createObjectURL;
  r.sandbox.URL.createObjectURL = function (b) { captured = b; return "blob:x"; };
  try {
    doc.getElementById("btn-owner-save").click();
  } finally {
    r.sandbox.URL.createObjectURL = real;
  }
  if (!captured || !captured._parts || !captured._parts.length) return null;
  return Buffer.concat(captured._parts.map(p => Buffer.from(p)));
}

// ------------------------------------------------------------
// 跑一个页面
// ------------------------------------------------------------
function runPage(pageFile, opts) {
  const html = fs.readFileSync(path.join(dir, pageFile), "utf8");
  const doc = makeDoc(html);
  seedStaticIds(doc, html);

  const store = {};
  const sandbox = {
    console: console,
    document: doc,
    window: { addEventListener: function () {}, location: { href: "" } },
    localStorage: {
      getItem: function (k) { return Object.prototype.hasOwnProperty.call(store, k) ? store[k] : null; },
      setItem: function (k, v) { store[k] = String(v); },
      removeItem: function (k) { delete store[k]; }
    },
    location: { href: "" },
    navigator: { clipboard: { writeText: function () { return Promise.resolve(); } } },
    requestAnimationFrame: function () { return 0; },
    setTimeout: function () { return 0; },
    clearTimeout: function () {},
    Image: function () {
      const self = this;
      self.naturalWidth = 0; self.naturalHeight = 0; self.width = 0; self.height = 0;
      Object.defineProperty(self, "src", { set: function () { /* 不加载 */ }, get: function () { return ""; } });
    },
    // ★★ 2026-10-15：Blob 桩留住 parts —— "保存安装包"导出的字节要能拿回来做
    //    **逐字节比对**（这一单的核心判据就是"导出 → 导入 → 再导出相同"）。
    Blob: function (parts) {
      this._parts = parts || [];
      this.size = (parts && parts[0] && parts[0].length) || 0;
    },
    URL: { createObjectURL: function () { return "blob:x"; }, revokeObjectURL: function () {} },
    TextEncoder: TextEncoder, TextDecoder: TextDecoder,
    Uint8Array: Uint8Array, Uint8ClampedArray: Uint8ClampedArray,
    DataView: DataView, ArrayBuffer: ArrayBuffer, JSON: JSON, Math: Math, Date: Date,
    ImageData: function (data, w, h) { this.data = data; this.width = w; this.height = h; },
    // ★★ 2026-10-15（车主："光有导出，没有导入 bin 啊？"）：FileReader 从"什么都不做"
    //   升级成**最小可用实现** —— 它同步把夹具文件自带的 `_buffer` 回填给 onload。
    //   为什么要真的能跑：这一单的验收点之一是"点车主模式的「打开安装包…」⇒
    //   配色与图片清单都被更新、预览跟着刷新"，只查源码是查不出这条的
    //   （桩里原来那句注释也正是这么写的：DOM 桩没有可用的 FileReader）。
    //   ★ 桩里没有真 File：夹具自己带 `_buffer`（见 readFixture 那一段）。
    FileReader: function () {
      const self = this;
      self.readAsArrayBuffer = function (f) {
        self.result = (f && f._buffer) ? f._buffer : new ArrayBuffer(0);
        if (self.onload) self.onload();
      };
      self.readAsText = function (f) {
        self.result = (f && f._text) ? f._text : "";
        if (self.onload) self.onload();
      };
    }
  };
  sandbox.globalThis = sandbox;
  sandbox.self = sandbox;
  vm.createContext(sandbox);

  // 先跑页面引用的那几个共享脚本（页面假设它们是全局的）
  const srcRe = /<script[^>]*\bsrc="([^"]+)"/gi;
  let sm;
  while ((sm = srcRe.exec(html)) !== null) {
    const p = path.join(dir, sm[1]);
    if (!fs.existsSync(p)) throw new Error(pageFile + " 引用了不存在的 " + sm[1]);
    vm.runInContext(fs.readFileSync(p, "utf8"), sandbox, { filename: sm[1] });
  }

  // 再跑内联脚本（页面自己的那段）
  const re = /<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi;
  let m, ran = 0;
  while ((m = re.exec(html)) !== null) {
    vm.runInContext(m[1], sandbox, { filename: pageFile });
    ran++;
  }
  return { sandbox: sandbox, doc: doc, store: store, ran: ran };
}

// ------------------------------------------------------------
section("合并后的编辑器（index.html）在最小 DOM 桩里从头跑到尾（启动 + 两个分区都点一遍）");
const results = {};
{
  const page = "index.html";
  const SOURCE = fs.readFileSync(path.join(dir, page), "utf8");
  let r = null;
  try {
    r = runPage(page);
    ok(true, page + " 启动没抛错（内联脚本 " + r.ran + " 段）");
  } catch (e) {
    ok(false, page + " 启动抛错：" + e.message + "\n      " + String(e.stack).split("\n")[1]);
  }
  if (r) {
    results[page] = r;
    const doc = r.doc;

    // ---- 分区 ① 配色：那几件控件真的被接上了（不是停留在 HTML 里没人管）----
    ok(!!doc.getElementById("oc-bg"), "有背景色控件 #oc-bg");
    ok(doc.getElementById("oc-bg-txt").value !== "", "背景色那一行被灌了当前值：" +
       doc.getElementById("oc-bg-txt").value);
    for (let i = 0; i < 4; i++) {
      ok(doc.getElementById("oc-arc" + i + "-txt").value !== "",
         "弧 " + i + " 那一行被灌了当前值");
      ok(doc.getElementById("oc-arc" + i + "-lb").textContent.length > 0,
         "弧 " + i + " 的标签是中文（" + doc.getElementById("oc-arc" + i + "-lb").textContent + "）");
    }
    ok(String(doc.getElementById("oc-redline").value) !== "", "红区滑块被灌了当前值");
    ok(String(doc.getElementById("oc-overspeed").value) !== "", "超速滑块被灌了当前值");
    // ---- ★ 2026-10-15：水温 / 进气两块色板（接的是**已有的键**）----
    for (const [id, label] of [["oc-coolant", "水温颜色"], ["oc-intake", "进气颜色"]]) {
      const c = doc.getElementById(id), t = doc.getElementById(id + "-txt");
      ok(!!c && !!t, "有「" + label + "」色板 #" + id);
      ok(!!c && String(c.value) !== "", label + "色板的色块被灌了当前值：" + (c && c.value));
      ok(!!t && String(t.value) !== "", label + "色板的色号被灌了当前值：" + (t && t.value));
    }
    // ---- ★★ 2026-10-15（R2）：转速 / 速度**各有一块**色板 ----
    //   （原来那一句"转速/速度共用数字颜色"已经随本单作废 —— 主题里现在有
    //    readout.rpm_color / speed_color 两个键，固件按屏的角色取。）
    for (const [id, label] of [["oc-rpm", "转速数字颜色"], ["oc-speed", "速度数字颜色"]]) {
      const c = doc.getElementById(id), t = doc.getElementById(id + "-txt");
      ok(!!c && !!t, "有「" + label + "」色板 #" + id);
      ok(!!c && String(c.value) !== "", label + "色板的色块被灌了当前值：" + (c && c.value));
      ok(!!t && String(t.value) !== "", label + "色板的色号被灌了当前值：" + (t && t.value));
    }
    // ---- ★★ 2026-10-15 晚（R3）：数字底板深浅（滑杆 + 百分比显示）----
    {
      const rng = doc.getElementById("oc-plate"), pct = doc.getElementById("oc-plate-pct");
      ok(!!rng && !!pct, "有「数字底板深浅」滑杆 #oc-plate 与百分比显示 #oc-plate-pct");
      ok(!!rng && String(rng.value) !== "", "底板深浅滑杆被灌了当前值：" + (rng && rng.value));
      if (rng) {
        eq(String(rng.min), "0", "底板滑杆的下限是 0（= 关掉底板）");
        eq(String(rng.max), "255", "底板滑杆的上限是 255（与主题键 plate_opa 同一个域）");
        ok(Number(rng.value) === 38,
           "底板滑杆的初值 = 主题里的 38/255（得到 " + rng.value + "）");
      }
      ok(!!pct && /^15%$/.test(pct.textContent),
         "百分比显示是 15%（38/255 —— 显示成人话，存进文件的仍是 0..255）：" +
         (pct && pct.textContent));
    }
    // ★ 那句说明现在说的是"已各自独立"（原来那句"共用…要改固件"已经作废）
    //   ★ DOM 桩没有 HTML 解析器 ⇒ 静态元素只"建"出一个空壳，textContent 是空的。
    //     所以这里按**静态源码**判它的内容（"它写着什么"），控件层那条（"它不是
    //     控件"）由 check-owner-mode.js 的 ①c 判 —— 两边各管一头，不重复也不留空。
    {
      const legend = doc.querySelector('[data-ocap="legend"]');
      ok(!!legend, "配色区里有那行说明（元素在）");
      const m = /<div class="note"[^>]*data-ocap="legend"[^>]*>([\s\S]*?)<\/div>/.exec(SOURCE);
      ok(!!m, "说明那一行的静态 HTML 找得到");
      if (m) {
        ok(/转速与速度的数字颜色已各自独立/.test(m[1]),
           "说明文字点明「已经拆开」（不再是『共用』那套说法）");
        ok(!/目前共用/.test(m[1]), "说明文字里不再有「目前共用」这句过时的话");
        ok(!/需要一次固件改动/.test(m[1]), "说明文字里不再有「要拆开需要一次固件改动」");
        ok(!/<input|<select|<button/.test(m[1]), "那一行里没有控件（是纯文字说明）");
      }
    }
    ok(doc.getElementById("owner-presets").children.length === 1,
       "3 个预设那一行被挂上界面");

    // ---- 分区 ② 图片：槽位清单（背景 1 + 表情 10），每格三件事 ----
    const grid = doc.getElementById("owner-slots");
    ok(!!grid, "有槽位清单容器 #owner-slots");
    if (grid) {
      eq(grid.children.length, 11, "槽位清单正好 11 格（背景 1 + 表情 10）");
      const cells = grid.children;
      eq(cells.filter(c => c.getAttribute("data-ocap") === "bgimage").length, 1,
         "11 格里恰好 1 格是背景（data-ocap=bgimage）");
      const first = cells[0];
      ok(first.children.length === 3, "每格三件事：名称 / 缩略图 / 按钮列");
      ok(/^背景/.test(first.children[0].textContent), "第一格是背景（名称：" +
         first.children[0].textContent.split("\n")[0] + "）");
      const bgs = first.children[2];
      eq(bgs.children.length, 2, "每格两个按钮（换图/清除）");
      eq(bgs.children[0].textContent, "选文件", "空的格子第一个按钮写「选文件」");
      ok(bgs.children[1].disabled === true, "空格的「清除」是禁用的");
      const faceCells = cells.filter(c => c.getAttribute("data-ocap") !== "bgimage");
      eq(faceCells.length, 10, "表情格 10 个");
      const names = faceCells.map(c => c.children[0].textContent.split("\n")[0]);
      ok(names.filter(n => n.indexOf("转速表") >= 0).length === 5, "5 格点明「转速表」");
      ok(names.filter(n => n.indexOf("速度表") >= 0).length === 5, "5 格点明「速度表」");
      ok(names.join("|").indexOf("怠速") >= 0 && names.join("|").indexOf("红区") >= 0 &&
         names.join("|").indexOf("静止") >= 0 && names.join("|").indexOf("超速") >= 0,
         "格名是中文阶段名（" + names.slice(0, 5).join(" / ") + " …）");
      const fc = faceCells[0].children[2];
      eq(fc.children.length, 2, "表情格也有换图/清除两个按钮");
    }

    // ---- 两个分区各自的「保存安装包」+ 一条刷写命令（端口可改、偏移写死 0x210000）----
    for (const [cmdId, portId, saveId, tag] of [
      ["owner-cmd", "owner-port", "btn-owner-save", "① 配色"],
      ["owner-cmd-img", "owner-port-img", "btn-owner-save-img", "② 图片"]
    ]) {
      const cmd = doc.getElementById(cmdId);
      ok(!!cmd && cmd.value.indexOf("write_flash 0x210000 assets.bin") > 0,
         tag + " 分区那条刷写命令带写死的偏移 0x210000：" + (cmd ? cmd.value : "(没有这个元素)"));
      ok(!!cmd && cmd.value.indexOf("--port COM6") > 0, tag + " 分区端口默认 COM6（可在输入框里改）");
      ok(doc.getElementById(portId).value === "COM6", tag + " 分区的端口框被灌了当前值");
      ok(doc.getElementById(saveId).getAttribute("data-ocap") === "save",
         tag + " 分区的「保存安装包」标记正确");
    }

    // ---- ★ 2026-10-15：背景格也走框选器（取景语义），mode 必须是 "bg" ----
    //   ★ 这一条**故意只在源码层查**：DOM 桩里没有 FileReader / 真 Image 解码，
    //     硬塞假 File 只会测到桩自己的行为。真正"喂一张 WebP 进去看走哪条路"
    //     的端到端判据在 check-webp.js（它起无头浏览器，用页面自己的
    //     sendFilesToSlot 跑三种情形）。
    ok(typeof r.sandbox.sendFilesToSlot === "function",
       "页面里有 sendFilesToSlot()（两个槽位共用的入口）");
    ok(/const mode = spec\.face \? "face" : "bg"/.test(SOURCE),
       "入口按 face 分流：表情=face、背景=bg（背景也有框选，只是语义不同）");
    // 两个端口框同步同一个值（改一个，另一个跟着变）
    const p1 = doc.getElementById("owner-port");
    p1.value = "COM9";
    p1.dispatch("input");
    ok(r.store["206dash.flashPort"] === "COM9", "端口记在本机（自己的键，不混进配色）");
    ok(doc.getElementById("owner-port-img").value === "COM9", "另一个分区的端口框同步到 COM9");
    ok(doc.getElementById("owner-cmd-img").value.indexOf("--port COM9") > 0,
       "另一个分区的命令行也跟着换端口");

    // ---- 页内分区切换：只切显隐，不动状态 ----
    doc.getElementById("part-images").click();
    ok(doc.getElementById("owner-theme").style.display === "none" &&
       doc.getElementById("owner-images").style.display === "",
       "点「② 图片」= 图片那一半可见、配色那一半隐藏");
    ok(r.store["206dash.themeEditorPart"] === "images", "分区选择记在自己的键上（不动主题格式）");
    doc.getElementById("part-theme").click();
    ok(doc.getElementById("owner-images").style.display === "none",
       "点「① 配色」切回来");

    // ---- 交互 1：改背景色 ⇒ T.bg_color 跟着变，且写进了本机那一份 ----
    const bgIn = doc.getElementById("oc-bg");
    bgIn.value = "#123456";
    bgIn.dispatch("input");
    const saved = JSON.parse(r.store["206dash.theme"]);
    ok(saved.theme.bg_color === 0x123456,
       "改背景色写进了现有主题状态（bg_color = 0x" + saved.theme.bg_color.toString(16) + "）");
    ok(saved.v === 2 && !!saved.base, "本机记录仍是 {v:2, base, theme} 那套格式（没改存储格式）");
    // 高级那一栏（同一个 T）也跟着变了 —— 一份状态，两处显隐
    ok(String(doc.getElementById("bg-color").value).toUpperCase() === "#123456",
       "高级里的「预览底色」跟着变（同一个 T）：" + doc.getElementById("bg-color").value);
    const jsonVal = JSON.parse(doc.getElementById("json").value);
    ok(jsonVal.theme.bg_color === 0x123456, "主题 JSON 文本框里的 bg_color 也是新的");

    // ---- ★ 交互 1b：改「水温颜色」⇒ 写的**就是 readout.coolant_color** 那个键 ----
    //   （车主那一单：两块新色板只暴露既有键，不新增格式）
    {
      const c = doc.getElementById("oc-coolant");
      c.value = "#bf4153";                  // 车主实拍里那两个板的颜色
      c.dispatch("input");
      const s2 = JSON.parse(r.store["206dash.theme"]);
      ok(s2.theme.readout.coolant_color === 0xBF4153,
         "改「水温颜色」写进 readout.coolant_color = 0x" +
         s2.theme.readout.coolant_color.toString(16));
      ok(Object.keys(s2.theme.readout).sort().join(",") ===
         Object.keys(JSON.parse(doc.getElementById("json").value).theme.readout).sort().join(","),
         "readout 段的键集合没变（不多不少 —— 没有新增格式）");
      // 进水温**不该**动到进气/数字
      const i2 = doc.getElementById("oc-intake");
      i2.value = "#7cff6b";
      i2.dispatch("input");
      const s3 = JSON.parse(r.store["206dash.theme"]);
      ok(s3.theme.readout.intake_color === 0x7CFF6B,
         "改「进气颜色」写进 readout.intake_color = 0x" +
         s3.theme.readout.intake_color.toString(16));
      ok(s3.theme.readout.coolant_color === 0xBF4153,
         "两次改动各归各的键（水温那次没被覆盖）");
      // 高级里那两行也跟着变（一份状态两处显隐）
      ok(JSON.parse(doc.getElementById("json").value).theme.readout.intake_color === 0x7CFF6B,
         "主题 JSON 文本框里的 intake_color 也是新的");
    }

    // ---- ★★ 交互 1c（2026-10-15 R2）：转速/速度两块色板 ⇒ 各写各的键 ----
    //   这是"拆开了"在**交互层**的机器证明：两个色板改出来的是两个不同的键。
    {
      const rp = doc.getElementById("oc-rpm");
      rp.value = "#aa0001"; rp.dispatch("input");
      const s4 = JSON.parse(r.store["206dash.theme"]);
      ok(s4.theme.readout.rpm_color === 0xAA0001,
         "改「转速数字颜色」写进 readout.rpm_color = 0x" +
         s4.theme.readout.rpm_color.toString(16));
      const sp = doc.getElementById("oc-speed");
      sp.value = "#bb0002"; sp.dispatch("input");
      const s5 = JSON.parse(r.store["206dash.theme"]);
      ok(s5.theme.readout.speed_color === 0xBB0002,
         "改「速度数字颜色」写进 readout.speed_color = 0x" +
         s5.theme.readout.speed_color.toString(16));
      ok(s5.theme.readout.rpm_color === 0xAA0001,
         "两块色板各归各的键（改速度没把转速覆盖掉）");
      ok(s5.theme.readout.digit_color === 0xFFFFFF,
         "两个新键与 digit_color 是**三个**字段（回落值本身没被当成写入口）");
      ok(JSON.parse(doc.getElementById("json").value).theme.readout.speed_color === 0xBB0002,
         "主题 JSON 文本框里的 speed_color 也是新的");
    }

    // ---- ★★ 交互 1d（2026-10-15 R3）：底板深浅滑杆 ⇒ readout.plate_opa ----
    {
      const rng = doc.getElementById("oc-plate");
      eq(String(rng.value), "38", "滑杆初值 = 主题里的 38/255（15%）");
      rng.value = "128"; rng.dispatch("input");
      const s6 = JSON.parse(r.store["206dash.theme"]);
      ok(s6.theme.readout.plate_opa === 128,
         "拖底板滑杆写进 readout.plate_opa = " + s6.theme.readout.plate_opa + "（0..255 域）");
      ok(/^50%$/.test(doc.getElementById("oc-plate-pct").textContent),
         "百分比显示跟着变：128/255 ≈ 50%（得到 " +
         doc.getElementById("oc-plate-pct").textContent + "）");
      rng.value = "0"; rng.dispatch("input");
      ok(JSON.parse(r.store["206dash.theme"]).theme.readout.plate_opa === 0,
         "拖到底 = 0（关掉底板）也是一个能存进主题的合法值");
      rng.value = "38"; rng.dispatch("input");   // 还原，别影响后面的交互
    }

    // ---- 交互 2：套一个预设 ⇒ 写进同一份状态 ----
    const presetRow = doc.getElementById("owner-presets").children[0];
    const btns = presetRow.children[1].children;
    ok(btns.length === 3, "预设按钮 3 个（当前默认 / 夜间 / 浅底）");
    const before = JSON.parse(r.store["206dash.theme"]).theme.bg_color;
    btns[1].click();                       // 夜间
    const after = JSON.parse(r.store["206dash.theme"]).theme.bg_color;
    ok(after === 0x05070C, "点「夜间」把底色写成 0x05070C（原来是 0x" +
       before.toString(16) + "）");

    // ---- 交互 3：切模式 ⇒ 只是显隐（两个分区那排标签在高级里藏起来）----
    doc.getElementById("btn-mode-adv").click();
    ok(doc.getElementById("advanced").style.display === "" &&
       doc.getElementById("owner").style.display === "none",
       "点「高级」= 高级可见、车主模式隐藏");
    ok(doc.getElementById("partbar").style.display === "none",
       "高级里那排分区标签藏起来（高级本来就把两边放在一起）");
    doc.getElementById("btn-mode-owner").click();
    ok(doc.getElementById("advanced").style.display === "none", "点「车主模式」切回来");
    ok(doc.getElementById("partbar").style.display !== "none", "分区标签也回来了");
    ok(r.store["206dash.themeEditorMode"] === "owner", "模式选择记在**另一个**键上（不动主题格式）");

    // ---- 交互 4：没有图片时点「保存安装包」⇒ 明说原因，不抛错 ----
    doc.getElementById("btn-owner-save").click();
    ok(/还没有图片/.test(doc.getElementById("status").textContent),
       "没有图片时点保存会说清原因：" + doc.getElementById("status").textContent.split("\n")[0]);

    // ---- 交互 5：改主题的几条路（恢复默认配色 / 整份默认 / 从 JSON 文本框应用）----
    //   ★ 这三条都会走 applyThemeToUi() —— 它会**重建**高级那一整面墙
    //     （控件的闭包抓着 T 里的具体对象，主题换了对象就必须重建）。
    //     桩里跑一遍就是"重建这条路不许抛错"的判据。
    doc.getElementById("oc-bg").value = "#0A0B0C";
    doc.getElementById("oc-bg").dispatch("input");
    doc.getElementById("btn-reset-theme").click();
    let rec2 = JSON.parse(r.store["206dash.theme"]);
    ok(rec2.theme.bg_color === 0x141414,
       "「用固件默认」把底色拉回固件默认 0x141414（实际 0x" + rec2.theme.bg_color.toString(16) + "）");
    ok(doc.getElementById("oc-bg-txt").value.toUpperCase() === "#141414",
       "车主模式那一行也跟着回到默认色：" + doc.getElementById("oc-bg-txt").value);

    doc.getElementById("btn-default").click();
    rec2 = JSON.parse(r.store["206dash.theme"]);
    ok(rec2.theme.alerts && rec2.theme.alerts.redline_rpm === 5500,
       "「恢复默认」恢复的是整份主题（alerts.redline_rpm = 5500）");

    doc.getElementById("json").value = JSON.stringify({ theme: { bg_color: 0xABCDEF } });
    doc.getElementById("btn-apply").click();
    rec2 = JSON.parse(r.store["206dash.theme"]);
    ok(rec2.theme.bg_color === 0xABCDEF,
       "从 JSON 文本框「应用」也能改（bg_color = 0x" + rec2.theme.bg_color.toString(16) + "）");
    ok(rec2.theme.screens.length === 2 && rec2.theme.screens[0].arcs.length === 2,
       "只写了一个字段的主题也会补齐（缺字段继承默认值：2 屏 × 2 弧）");
    ok(doc.getElementById("oc-bg-txt").value.toUpperCase() === "#ABCDEF",
       "应用之后车主模式那一行是新值：" + doc.getElementById("oc-bg-txt").value);

    // ---- ★★ 交互 5b（2026-10-15 R2）：**读入一份老主题** ⇒ 两个新色板显示 digit_color ----
    //   这是兼容条款的**端到端**证明（固件那边由 test_theme_store.cpp 的
    //   test_readout_rpm_speed_colors 钉着同一条语义）：
    //   老主题文件里只有 digit_color（车主把它设成 0xBF4153）⇒ 转速/速度两块色板
    //   必须显示 **0xBF4153**，而不是"默认白"。
    //   ★ 这一条抓过一个真 bug：合并默认值那一步会**先把新键占掉**（填成默认白），
    //     于是回落永远不触发 —— 车主设过的数字颜色看起来被换掉了。修法见
    //     index.html 的 mergeImportedTheme / loadThemeFromLocal（先解回落，再合并）。
    {
      doc.getElementById("json").value = JSON.stringify(
        { theme: { readout: { digit_color: 0xBF4153 } } });
      doc.getElementById("btn-apply").click();
      const rv = doc.getElementById("oc-rpm-txt").value.toUpperCase();
      const sv = doc.getElementById("oc-speed-txt").value.toUpperCase();
      ok(rv === "#BF4153", "老主题（只有 digit_color）⇒ 转速色板显示它的值（得到 " + rv + "）");
      ok(sv === "#BF4153", "同上：速度色板也是它的值（得到 " + sv + "）");
      const rec3 = JSON.parse(r.store["206dash.theme"]);
      ok(rec3.theme.readout.rpm_color === 0xBF4153 &&
         rec3.theme.readout.speed_color === 0xBF4153,
         "导出/存下来的主题里两个新键 = 老主题那份 digit_color（不是默认白）");
      ok(JSON.parse(doc.getElementById("json").value).theme.readout.rpm_color === 0xBF4153,
         "主题 JSON 文本框里也是回落后的值（与固件解析出来的一致）");
      // 文件里**写了**新键时当然听文件的（回落不能盖掉它）
      doc.getElementById("json").value = JSON.stringify(
        { theme: { readout: { digit_color: 0xBF4153, rpm_color: 0xAA0001 } } });
      doc.getElementById("btn-apply").click();
      ok(doc.getElementById("oc-rpm-txt").value.toUpperCase() === "#AA0001",
         "写了 rpm_color 就听它的（得到 " + doc.getElementById("oc-rpm-txt").value + "）");
      ok(doc.getElementById("oc-speed-txt").value.toUpperCase() === "#BF4153",
         "只写了一个新键 ⇒ 另一个回落到 digit_color");
    }

    // ---- ★★ 交互 5c（车主："光有导出，没有导入 bin 啊？"）：车主模式里导入 assets.bin ----
    //   ☆ 端到端：点车主模式那个按钮 → 灌一个**极小**的内联夹具（见 buildTinyContainer）
    //     → 配色（背景色 + 转速色）与图片清单都被恢复，而且预览真的重画了。
    //   ☆ 顺手钉住"两个入口、一条实现"：两个按钮都只是替点**同一个** #file-theme。
    {
      const fi = doc.getElementById("file-theme");
      let clicks = 0;
      const realClick = fi.click;                    // 桩：只记账，不弹真的文件框
      fi.click = function () { clicks++; };
      doc.getElementById("btn-owner-import").click();
      doc.getElementById("btn-owner-import-img").click();
      eq(clicks, 2, "两个「打开安装包…」按钮都只是替点 #file-theme（同一个输入框）");
      // ★ 顺手确认**高级那两个入口没被弄丢**（这一单只是把同一个入口搬到车主模式）
      doc.getElementById("btn-import").click();
      doc.getElementById("btn-load-theme").click();
      eq(clicks, 4,
         "高级的「导入主题文件」「读入 theme.json / assets.bin」也照旧替点**同一个** #file-theme");
      ok(doc.getElementById("btn-owner-import").getAttribute("data-ocap") === "import" &&
         doc.getElementById("btn-owner-import-img").getAttribute("data-ocap") === "import",
         "两个按钮都带 data-ocap=\"import\"（能力表数得到两个）");
      fi.click = realClick;

      const fx = buildTinyContainer(r.sandbox);
      const drawsBefore = ctxDraws;
      const bgBefore = doc.getElementById("oc-bg-txt").value.toUpperCase();
      fi.files = [{ name: "fixture-assets.bin", _buffer: fx.bytes.buffer }];
      fi.dispatch("change", { target: fi });

      const bg = doc.getElementById("oc-bg-txt").value.toUpperCase();
      ok(bg === "#123456",
         "读入后背景色板 = 夹具里的 bg_color（得到 " + bg + "，之前 " + bgBefore + "）");
      eq(doc.getElementById("oc-rpm-txt").value.toUpperCase(), "#AB12CD",
         "读入后转速色板 = 夹具里的 rpm_color（配色真的恢复了）");
      // 图片清单：**两处**显示同一个字符串（高级 + 车主模式）
      const ownInfo = doc.getElementById("owner-assets-info");
      const advInfo = doc.getElementById("assets-info");
      ok(/共 2 张/.test(ownInfo.textContent),
         "车主模式里也显示了导入的图片清单（" + ownInfo.textContent.split("\n")[0] + "）");
      ok(ownInfo.style.display === "", "车主模式那份清单从隐藏变成可见");
      eq(ownInfo.textContent, advInfo.textContent,
         "两处清单是**同一个字符串**（一处生成、两处显示）");
      ok(ctxDraws > drawsBefore,
         "预览跟着重画了（画布笔数 " + drawsBefore + " → " + ctxDraws + "）");
      ok(doc.getElementById("owner-slots").children.length === 11,
         "槽位清单刷新后仍是 11 格（背景 1 + 表情 10）");
      // ★★ 槽位**真的被填上**了（车主报的正是"没有图片载入"）：
      //   夹具那两条项一条是背景（第 1 格）、一条是左屏·怠速（第 2 格），
      //   两格的「清除」都从"禁用"变成"可用" —— 这是"格子里真有图"的界面证据
      //   （桩里没有真 canvas，缩略图像素判不了；像素判据在 check-*.js 那一批）。
      const cells = doc.getElementById("owner-slots").children;
      ok(cells[0].children[2].children[1].disabled === false,
         "背景格被填上了（「清除」可用）");
      ok(cells[1].children[2].children[1].disabled === false,
         "左屏·怠速那一格也被填上了（「清除」可用）");
      ok(cells[2].children[2].children[1].disabled === true,
         "没在包里的格子照旧是空的（第 3 格「清除」仍禁用）");
      ok(/已读入/.test(doc.getElementById("status").textContent),
         "状态栏说明了读进来了什么：" +
         doc.getElementById("status").textContent.split("\n")[0]);
      ok(/背景 1 \+ 表情 1/.test(doc.getElementById("status").textContent),
         "状态栏给了可读摘要（背景 1 + 表情 1）");

      // ★★★ 往返判据（本仓库的核心口径）：**导出 → 导入 → 再导出逐字节相同**。
      //   走页面自己的打包入口 buildPayload()（不是在这里另拼一份 payload），
      //   再喂给同一份 ImageBlob.build() —— 字节要**完全一样**。
      //   这一条同时钉住两件事：像素透传（item.packed）与**名字原始字节回写**
      //   （夹具里那条第 2 项的名字故意不是合法 UTF-8，重新编码就回不去）。
      const rebuilt = IB.build(r.sandbox.buildPayload()).blob;
      ok(Buffer.from(rebuilt).equals(Buffer.from(fx.imageBytes)),
         "★ 往返：导入后再打包出的 image 段与夹具**逐字节相同**（" +
         rebuilt.length + " vs " + fx.imageBytes.length + " 字节）");
      // ★★ 更进一步：点**真的那个按钮**（「保存安装包」）拿回导出的字节 ——
      //    桩里把 URL.createObjectURL 换成一个钩子，就能把 Blob 的 parts 接出来。
      //    这一条同时覆盖"按钮那条路"与**整包**（theme 段走 themeSrcBytes 原始字节 +
      //    image 段走 packed 透传）。
      const saved = exportViaButton(r, doc);
      ok(saved && saved.equals(Buffer.from(fx.bytes)),
         "★★ 整包往返：点「保存安装包」导出的 assets.bin 与夹具**逐字节相同**" +
         (saved ? "（" + saved.length + " vs " + fx.bytes.length + " 字节）" : "（没拿到字节）"));

      // ★★ 透传的**收口判据**：改过就不能再透传（否则"改了没反应"）。
      //   这里直接把页面那个纯函数拿出来喂几种"改法" —— 比在四个控件里各试一遍结实。
      {
        const meta = { w: 4, h: 4, cf: 0x14, role: 3, stridePad: 0, fit: "contain", trim: false };
        const mk = (over) => Object.assign({
          w: 4, h: 4, cf: 0x14, role: 3, stridePad: 0, fit: "contain", trim: false,
          packedMeta: meta, packed: new Uint8Array(48)
        }, over);
        ok(r.sandbox.packedUnchanged(mk({})) === true,
           "一个字没改 ⇒ 仍然按原字节透传（往返的前提）");
        ok(r.sandbox.packedUnchanged(mk({ w: 5 })) === false,
           "改了宽度 ⇒ 回渲染路径（不会拿老像素糊过去）");
        ok(r.sandbox.packedUnchanged(mk({ fit: "cover" })) === false, "改了缩放方式 ⇒ 同上");
        ok(r.sandbox.packedUnchanged(mk({ trim: true })) === false, "勾了「裁透明边」⇒ 同上");
        ok(r.sandbox.packedUnchanged(mk({ role: 1 })) === false, "改了用途 ⇒ 同上");
        ok(r.sandbox.packedUnchanged(mk({ order: 7 })) === true,
           "只改顺序 ⇒ 仍然透传（顺序照实写进文件，不影响像素）");
        ok(Object.assign(mk({ w: 5 }), {}).packed !== undefined, "（透传字节本身还在项上）");
      }
      const rePack = APC.packBytes({ themeBytes: new TextEncoder().encode(
                                       JSON.stringify({ theme: r.sandbox.T }, null, 2)),
                                     imageBytes: rebuilt, layout: fx.layout,
                                     themeLabel: "again", imageLabel: "again" });
      const tail = Buffer.from(rePack.bytes).subarray(fx.layout.imageOffset,
                                                      fx.layout.imageOffset + fx.imageBytes.length);
      ok(tail.equals(Buffer.from(fx.imageBytes)),
         "★ 整包里那一段 image 也逐字节相同（走 packBytes 那条真路径）");

      // ★★★ 可选：拿**车主那份真文件**再跑一遍（`REAL_PKG=<路径>` 时才跑）。
      //   为什么做成可选：那个 bin 在仓库外（2.4MB），不能进仓库；但"导入 → 再导出
      //   逐字节相同"这条判据值得在**真数据**上复现一次 —— 车主导入自己的包
      //   就是这个场景（11 项：背景 1 + 表情 10，名字还是老的截断字节）。
      const realPkg = process.env.REAL_PKG;
      if (realPkg && fs.existsSync(realPkg)) {
        const rb = new Uint8Array(fs.readFileSync(realPkg));
        const rLayout = r.sandbox.coreForTarget();
        const rInfo = APC.parseImageBlob(
          rb.subarray(rLayout.imageOffset), { label: "真文件" });
        const rImage = rb.subarray(rLayout.imageOffset,
                                   rLayout.imageOffset + rInfo.totalBytes);
        fi.files = [{ name: "real-assets.bin", _buffer: rb.buffer.slice(
                        rb.byteOffset, rb.byteOffset + rb.byteLength) }];
        fi.dispatch("change", { target: fi });
        // ★ items 是页面里的 `let`（词法绑定，拿不到）⇒ 用**打包入口**数项数：
        //   buildPayload() 一项一条，它就是"当前图片列表"的快照。
        eq(r.sandbox.buildPayload().length, rInfo.count,
           "★ 真文件（" + realPkg.split(/[\\/]/).pop() + "）：导入后图片列表项数 = 包里的项数 " +
           rInfo.count);
        const cellsR = doc.getElementById("owner-slots").children;
        eq(cellsR.filter(c => c.children[2].children[1].disabled === false).length, 11,
           "★ 真文件导入后**11 格全部被填上**（车主看到的『没有图片载入』就是这里原本是空的）");
        const rebuiltR = IB.build(r.sandbox.buildPayload()).blob;
        ok(Buffer.from(rebuiltR).equals(Buffer.from(rImage)),
           "★★ 真文件往返：导入后再打包出的 image 段与原件**逐字节相同**（" +
           rebuiltR.length + " vs " + rImage.length + " 字节）");
        const savedR = exportViaButton(r, doc);
        ok(savedR && savedR.equals(Buffer.from(rb)),
           "★★ 真文件整包往返：点「保存安装包」导出的 assets.bin 与车主那份文件**逐字节相同**" +
           (savedR ? "（" + savedR.length + " vs " + rb.length + " 字节）" : "（没拿到字节）"));
        console.log("  · 真文件往返已跑：" + rInfo.count + " 张，" +
                    rebuiltR.length + " 字节，逐字节相同 ✓" +
                    (savedR && savedR.equals(Buffer.from(rb)) ? "（整包也相同 ✓）" : "（整包不同 ✗）"));
      } else {
        console.log("  · （未设 REAL_PKG ⇒ 真文件那一条跳过；夹具那条已经跑了）");
      }

      // ★ 复原成"没有图片"：后面几条交互（点导出 ⇒ 应当说"还没有图片"）的前提就是
      //   空的图片列表 —— 这里把**每一格**的「清除」都点一遍（走车主真会走的那条路：
      //   桩里每格的结构是「名称 / 缩略图 / 按钮列」，按钮列第 2 个 = 清除）。
      const cells2 = doc.getElementById("owner-slots").children;
      eq(cells2[0].children.length, 3, "槽位格三件事（名称 / 缩略图 / 按钮列）");
      for (const c of cells2) {
        const btn = c.children[2].children[1];
        if (btn.disabled === false) btn.click();
      }
      ok(cells2.every(c => c.children[2].children[1].disabled === true),
         "每一格都清掉之后回到『空』（后续交互的前提恢复了）");
    }

    // ---- 交互 7：**每一个静态按钮都被接上了**（不是停留在 HTML 里没人管）----
    //   ★ 合并两页时最容易犯的错：按钮复制过来了，但接线落在被删掉的那一段里
    //     （实测抓到过：btn-load-theme / btn-reset-theme 各有一处）。
    //     桩里 addEventListener 会记账，所以这里按 id 逐个问"有没有 click 处理"。
    {
      const html = fs.readFileSync(path.join(dir, "index.html"), "utf8");
      const ids = [];
      const re = /<button[^>]*\bid="([^"]+)"/g;
      let m;
      while ((m = re.exec(html)) !== null) ids.push(m[1]);
      const dead = ids.filter(id => {
        const el = doc.getElementById(id);
        return !el || !(el._events && el._events.click && el._events.click.length);
      });
      eq(dead.length, 0, "每个静态按钮都有 click 处理（没接的：" + dead.join(", ") + "）");
      ok(ids.length >= 18, "静态按钮至少 18 个（实际 " + ids.length + "）");
    }

    // ---- 交互 8：导出那两条路（theme.json 能导出；没图时 image.bin/容器会明说原因）----
    doc.getElementById("btn-theme-export").click();
    ok(/已导出 theme\.json/.test(doc.getElementById("status").textContent),
       "点「导出 theme.json」走通了：" + doc.getElementById("status").textContent.split("\n")[0]);
    doc.getElementById("btn-export").click();
    ok(/还没有图片/.test(doc.getElementById("status").textContent),
       "没图时点「导出 image.bin」会说清原因");
    doc.getElementById("btn-assets-export").click();
    ok(/还没有图片/.test(doc.getElementById("status").textContent),
       "没图时点「导出 assets.bin」会说清原因");

    // ---- 交互 6：目标板 / 阶段 / 阶段模拟那排小图都在 ----
    ok(doc.getElementById("target-board").children.length >= 3,
       "目标板下拉有选项（" + doc.getElementById("target-board").children.length + " 个）");
    // ★ 档数**问 face-stages.js**（不写死）：转速 5 + 车速 5 + 水温 3 + 进气 3 = 16
    const FSrd = require(path.join(dir, "face-stages.js"));
    eq(doc.getElementById("stage").children.length, FSrd.STAGES.length,
       "阶段下拉的档数 = face-stages.js 的 STAGES 档数");
    eq(doc.getElementById("stage-grid").children.length, FSrd.STAGES.length,
       "阶段模拟那排小图 = 一档一格");
    ok(doc.getElementById("face-left").children.length === FSrd.statesOf(0).length + 1,
       "左屏表情下拉 = 跟随阶段 + 该屏的状态数");
    ok(doc.getElementById("face-right").children.length === FSrd.statesOf(1).length + 1,
       "右屏表情下拉 = 跟随阶段 + 该屏的状态数");
  }
}

// ------------------------------------------------------------
section("image-editor.html 只是**重定向薄壳**（不许有第二份实现）");
{
  const shell = fs.readFileSync(path.join(dir, "image-editor.html"), "utf8");
  const code = shell.replace(/<!--[\s\S]*?-->/g, "");
  ok(/<meta\s+http-equiv="refresh"\s+content="0;\s*url=index\.html#images"/i.test(shell),
     "薄壳带 meta refresh 跳 index.html#images");
  ok(/href="index\.html#images"/.test(shell), "薄壳里还有一个能点的回跳链接（meta 被忽略时兜底）");
  ok((code.match(/<script\b/gi) || []).length === 0, "薄壳里没有 <script>（没有第二份实现）");
  ok(code.indexOf("data-ocap") < 0 && code.indexOf("THEME_KEY") < 0,
     "薄壳里没有车主模式控件、也没有主题状态");
  ok(shell.split(/\r?\n/).length <= 40 && Buffer.byteLength(shell, "utf8") <= 3000,
     "薄壳够薄（" + shell.split(/\r?\n/).length + " 行 / " + Buffer.byteLength(shell, "utf8") + " 字节）");
}

// ------------------------------------------------------------
section("一个页面、一份状态（合并后不再是\"两页共用同一个键\"）");
{
  const idx = results["index.html"];
  ok(!!idx, "合并后的页面跑起来了");
  if (idx) {
    ok(idx.store["206dash.theme"] !== undefined, "配色写进 206dash.theme（唯一那份）");
    ok(idx.store["206dash.flashPort"] !== undefined, "端口存在自己的键里（不混进配色）");
    ok(idx.store["206dash.themeEditorPart"] !== undefined, "分区选择也存在自己的键里");
    // 主题记录里**不许**混进本机偏好（模式/分区/端口/预览尺寸）
    const rec = JSON.parse(idx.store["206dash.theme"]);
    const keys = Object.keys(rec.theme);
    ok(keys.indexOf("mode") < 0 && keys.indexOf("part") < 0 &&
       keys.indexOf("port") < 0 && keys.indexOf("zoom") < 0,
       "导出的主题里没有本机偏好字段（" + keys.length + " 个顶层键）");
  }
}

console.log("\n" + "=".repeat(56));
if (fail === 0) console.log("全部通过:" + pass + " 项断言");
else {
  console.log("失败 " + fail + " 项 / 共 " + (pass + fail) + " 项:");
  failures.forEach(f => console.log("  - " + f));
}
console.log("=".repeat(56));
process.exit(fail === 0 ? 0 : 1);
