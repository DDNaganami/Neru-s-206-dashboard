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

const dir = __dirname;
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
section("合并后的编辑器（index.html）在最小 DOM 桩里从头跑到尾（启动 + 两个分区都点一遍）");
const results = {};
{
  const page = "index.html";
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
