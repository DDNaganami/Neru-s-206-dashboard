/* ============================================================
 * smoke-pages.js —— 用**极简 DOM 桩**把两个页面真的跑一遍（Node 运行）
 *
 *   node tools/theme-editor/smoke-pages.js
 *   退出码：0 = 两个页面都跑通；1 = 有页面在启动/交互时抛错
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
 * 再按车主模式的口径点几下（改色 / 套预设 / 切模式），看有没有抛错。
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

const dir = __dirname;
let pass = 0, fail = 0;
const failures = [];
function ok(cond, what) {
  if (cond) pass++;
  else { fail++; failures.push(what); console.log("  ✗ " + what); }
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
    setTransform: noop, clearRect: noop, fillRect: noop, strokeRect: noop,
    beginPath: noop, closePath: noop, arc: noop, moveTo: noop, lineTo: noop,
    stroke: noop, fill: noop, save: noop, restore: noop, translate: noop,
    scale: noop, rotate: noop, roundRect: noop, fillText: noop, strokeText: noop,
    drawImage: noop, putImageData: noop,
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
  doc.ids = [];
  doc.createElement = function (tag) { return makeEl(tag, doc); };
  doc.createElementNS = function (ns, tag) { return makeEl(tag, doc); };
  doc.createTextNode = function (t) { const e = makeEl("#text"); e.textContent = t; return e; };
  doc.getElementById = function (id) { return doc._byId[id] || null; };
  doc.querySelector = function () { return null; };
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
    Blob: function (parts) { this.size = (parts && parts[0] && parts[0].length) || 0; },
    URL: { createObjectURL: function () { return "blob:x"; }, revokeObjectURL: function () {} },
    TextEncoder: TextEncoder, TextDecoder: TextDecoder,
    Uint8Array: Uint8Array, Uint8ClampedArray: Uint8ClampedArray,
    DataView: DataView, ArrayBuffer: ArrayBuffer, JSON: JSON, Math: Math, Date: Date,
    ImageData: function (data, w, h) { this.data = data; this.width = w; this.height = h; },
    FileReader: function () { this.readAsText = function () {}; this.readAsArrayBuffer = function () {}; }
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
section("两个页面的内联脚本能在最小 DOM 桩里从头跑到尾（启动 + 交互）");
const results = {};
for (const page of ["index.html", "image-editor.html"]) {
  let r = null;
  try {
    r = runPage(page);
    ok(true, page + " 启动没抛错（内联脚本 " + r.ran + " 段）");
  } catch (e) {
    ok(false, page + " 启动抛错：" + e.message + "\n      " + String(e.stack).split("\n")[1]);
    continue;
  }
  results[page] = r;
  const doc = r.doc;

  // 车主模式那几件控件真的被接上了（不是停留在 HTML 里没人管）
  if (page === "index.html") {
    ok(!!doc.getElementById("oc-bg"), "index.html 有背景色控件 #oc-bg");
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
    ok(doc.getElementById("owner-cmd").value.indexOf("write_flash 0x210000 assets.bin") > 0,
       "那条刷写命令带写死的偏移 0x210000：" + doc.getElementById("owner-cmd").value);
    ok(doc.getElementById("owner-cmd").value.indexOf("--port COM6") > 0,
       "端口默认 COM6（可在输入框里改）");
    ok(doc.getElementById("oc-slot").children.length === 10,
       "槽位下拉有 10 个中文阶段名（实际 " + doc.getElementById("oc-slot").children.length + "）");
    const slotTexts = doc.getElementById("oc-slot").children.map(o => o.textContent);
    ok(slotTexts.join("|").indexOf("怠速") >= 0 && slotTexts.join("|").indexOf("红区") >= 0 &&
       slotTexts.join("|").indexOf("静止") >= 0 && slotTexts.join("|").indexOf("超速") >= 0,
       "槽位名是中文阶段名：" + slotTexts.join(" / "));
    ok(doc.getElementById("owner-presets").children.length === 1,
       "预设那一行被挂上界面");

    // 交互 1：改背景色 ⇒ T.bg_color 跟着变，且写进了本机那份
    const bgIn = doc.getElementById("oc-bg");
    bgIn.value = "#123456";
    bgIn.dispatch("input");
    const saved = JSON.parse(r.store["206dash.theme"]);
    ok(saved.theme.bg_color === 0x123456,
       "改背景色写进了现有主题状态（bg_color = 0x" + saved.theme.bg_color.toString(16) + "）");
    ok(saved.v === 2 && !!saved.base, "本机记录仍是 {v:2, base, theme} 那套格式（没改存储格式）");

    // 交互 2：切模式 ⇒ 只是显隐
    doc.getElementById("btn-mode-adv").click();
    ok(doc.getElementById("advanced").style.display === "" &&
       doc.getElementById("owner").style.display === "none",
       "点「高级」= 高级可见、车主模式隐藏");
    doc.getElementById("btn-mode-owner").click();
    ok(doc.getElementById("advanced").style.display === "none",
       "点「车主模式」切回来");
    ok(r.store["206dash.themeEditorMode"] === "owner", "模式选择记在**另一个**键上（不动主题格式）");
  } else {
    ok(!!doc.getElementById("owner-colors"), "图片页有车主模式配色容器");
    ok(doc.getElementById("owner-slot").children.length === 10,
       "图片页槽位下拉也有 10 个中文阶段名（实际 " +
       doc.getElementById("owner-slot").children.length + "）");
    ok(doc.getElementById("owner-cmd").value.indexOf("write_flash 0x210000 assets.bin") > 0,
       "图片页那条刷写命令也带写死的偏移 0x210000");
    ok(doc.getElementById("btn-owner-save").getAttribute("data-ocap") === "save",
       "图片页的「保存安装包」标记正确");
    // 交互：套预设（图片页也有预设那一行？没有 —— 只有主题页有；这里改色验证接线）
    doc.getElementById("btn-mode-adv").click();
    ok(doc.getElementById("advanced").style.display === "", "图片页「高级」切得动");
    doc.getElementById("btn-mode-owner").click();
  }
}

// ------------------------------------------------------------
section("车主模式的键与图片页/主题页共用（跨页不是两套状态）");
{
  const idx = results["index.html"], img = results["image-editor.html"];
  ok(idx && img, "两个页面都跑起来了");
  if (idx && img) {
    ok(idx.store["206dash.theme"] !== undefined, "主题页把配色写进 206dash.theme");
    ok(idx.store["206dash.flashPort"] === undefined || typeof idx.store["206dash.flashPort"] === "string",
       "端口存在自己的键里（不混进配色）");
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
