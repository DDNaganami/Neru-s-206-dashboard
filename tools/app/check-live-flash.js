#!/usr/bin/env node
/*
 * check-live-flash.js -- 「写进板子**不产生文件**」的可执行判据（Node 运行，不碰硬件）
 *
 *   node tools/app/check-live-flash.js
 *
 * ------------------------------------------------------------
 * 为什么要有它（车主 2026-10-15）
 * ------------------------------------------------------------
 * 车主原话："为什么每次刷写都要保存一次 bin 呢，这样我的下载文件夹会有很多个相同的 bin。"
 * ⇒ 外壳（`tools/app/app.js`）刷之前**不再要求先保存**：它现场问「表盘」屏那个编辑器框要
 *   **当前状态**的容器（复用编辑器**唯一**那条打包路 `exportAssetsBin()`），办法是把它的
 *   `download()` **临时截下来**（拿到字节就还原）⇒ `<a download>` 那一下根本不会发生，
 *   下载文件夹里不会多东西；下载只在车主**主动**点编辑器里的「保存安装包」时才发生。
 *
 * ★ 这份脚本把 app.js 里那三个函数**抠出来**（与 check-owner-mode.js 抠 defaultTheme()
 *   同一套办法：解析真源码，不手抄），喂一个**假的编辑器 window**，逐条验：
 *     ① 成功那一次：截到了字节、**真 download 一次都没被调到**、之后 download 被还原；
 *     ② 编辑器没给出文件（比如还没有图片）⇒ `构建失败：<编辑器状态栏那句>`（不静默）；
 *     ③ 编辑器没准备好（window 拿不到）⇒ 明说怎么办；
 *     ④ 外壳自己**一行产生下载的代码都没有**（createObjectURL / a.download 那套）；
 *     ⑤ 与编辑器页面的**约定**：index.html 里仍然有 `exportAssetsBin()` 与 `download()`，
 *        而且 `exportAssetsBin()` 真的会调 `download()` —— 那边改名/搬走，这里当场红。
 * ★ 它**不发任何请求、不碰串口、不跑 esptool**（连 fetch 都不需要）。
 */
'use strict';

const fs = require('fs');
const path = require('path');

const dir = __dirname;
const APP_JS = fs.readFileSync(path.join(dir, 'app.js'), 'utf8');
const EDITOR_HTML = fs.readFileSync(path.join(dir, '..', 'theme-editor', 'index.html'), 'utf8');

let pass = 0, fail = 0;
const failures = [];
function ok(cond, what) {
  if (cond) { pass++; console.log('  ok  ' + what); }
  else { fail++; failures.push(what); console.log('  ✗   ' + what); }
  return !!cond;
}
function section(t) { console.log('\n== ' + t); }

// ------------------------------------------------------------
// 从 app.js 里抠出那三个函数（不手抄 —— 抄一份就会在"改了页面忘了改这里"时分叉）
// ------------------------------------------------------------
function pick(name) {
  const re = new RegExp('function ' + name + '\\(\\)\\s*\\{[\\s\\S]*?\\n\\}');
  const m = re.exec(APP_JS);
  if (!m) throw new Error('app.js 里找不到 function ' + name + '()');
  return m[0];
}
const SRC = [pick('editorWindow'), pick('editorStatusText'), pick('buildAssetsInMemory')].join('\n');

// 假编辑器 window：download() 是"真的那个"，被调到就说明会产生下载文件
function makeFakeEditor(behavior) {
  const calls = { realDownload: 0, exportCalls: 0 };
  const win = {
    document: {
      getElementById: function (id) {
        if (id !== 'status') return null;
        return { textContent: behavior === 'empty' ? '还没有图片，导不出合并文件。' : '' };
      }
    },
    exportAssetsBin: function () {
      calls.exportCalls++;
      if (behavior === 'ok') win.download({ size: 123, name: 'assets.bin' });  // 编辑器内部就是 download(blob,name)
      // behavior 是 'empty' 时什么都不给：就像"还没有图片"那条路
    },
    _realDownload: function () {
      calls.realDownload++;
      throw new Error('真的 download() 被调到了 ⇒ <a download> 会发生 ⇒ 下载文件夹里会多一个文件 ✗');
    }
  };
  win.download = win._realDownload;                 // 编辑器自己的 download()
  return { win: win, calls: calls };
}

function load(win) {
  const $ = function () { return win ? { contentWindow: win } : null; };
  return new Function('$', SRC + '\nreturn { buildAssetsInMemory: buildAssetsInMemory };')($);
}

// ------------------------------------------------------------
section('① 成功那一次：截到字节、**真 download 没被调到**、之后还原');
{
  const f = makeFakeEditor('ok');
  const api = load(f.win);
  const built = api.buildAssetsInMemory();
  ok(built.ok === true, '返回 ok（拿到了编辑器当前状态打的容器）');
  ok(built.blob && built.blob.size === 123, '截到的就是编辑器要写出去的那一份字节（blob.size=123）');
  ok(f.calls.exportCalls === 1, '用的就是编辑器那条唯一的打包路 exportAssetsBin()（调了 1 次）');
  ok(f.calls.realDownload === 0,
     '★ 真的 download() **一次都没被调到** ⇒ 没有 <a download> ⇒ 下载文件夹里不会多文件');
  ok(f.win.download === f.win._realDownload, '用完把 download() 还原回去了（导出那条路不受影响）');
}

// ------------------------------------------------------------
section('② 编辑器没给出文件（比如还没有图片）⇒ 明说原因，不静默、也不下载');
{
  const f = makeFakeEditor('empty');
  const api = load(f.win);
  const built = api.buildAssetsInMemory();
  ok(built.ok === false, '返回失败（没有硬编一份空容器去刷）');
  ok(/^构建失败：/.test(built.why), '说法以「构建失败：」开头（车主看得懂）');
  ok(built.why.indexOf('还没有图片') >= 0, '把编辑器状态栏那句**原样**带出来：' + built.why);
  ok(f.calls.realDownload === 0, '失败那一次同样没有产生下载');
  ok(f.win.download === f.win._realDownload, '失败那一次 download() 也还原了');
}

// ------------------------------------------------------------
section('③ 编辑器没准备好 ⇒ 明说怎么办（不抛、不静默）');
{
  const api = load(null);
  const built = api.buildAssetsInMemory();
  ok(built.ok === false, '返回失败');
  ok(built.why.indexOf('编辑器还没准备好') >= 0 && built.why.indexOf('表盘') >= 0,
     '说明里点了去哪、怎么办：' + built.why);
}

// ------------------------------------------------------------
section('④ 外壳自己一行产生下载的代码都没有');
{
  ok(!/createObjectURL/.test(APP_JS), 'app.js 里没有 createObjectURL（不会造 blob URL 去点下载）');
  ok(!/\.download\s*=/.test(APP_JS.replace(/w\.download\s*=\s*(function \(blob\)|orig)/g, '')),
     'app.js 里除了"截字节/还原"那两处，没有别的地方在写 <a download>');
  ok(!/saveOwnerPackage/.test(APP_JS),
     '外壳**不调**编辑器那个"保存安装包"（刷之前不要求先保存 —— 车主那一条）');
  // 查"外壳有没有自己打容器"时先剥注释：解释性注释里会提到内核的名字（本仓库的风格就是这样）
  const APP_CODE = APP_JS.replace(/\/\*[\s\S]*?\*\//g, '').replace(/(^|[^:])\/\/[^\n]*/g, '$1');
  ok(!/AssetPackageCore\.packBytes\s*\(|ImageBlob\.build\s*\(/.test(APP_CODE),
     '外壳**不自己打容器**（没有第二套打包：字节一律来自编辑器的 exportAssetsBin）');
}

// ------------------------------------------------------------
section('⑤ 与编辑器页面的约定（那边改名/搬走，这里当场红）');
{
  ok(/function exportAssetsBin\(\)/.test(EDITOR_HTML),
     '编辑器页面里仍然有唯一的导出入口 exportAssetsBin()');
  ok(/function download\(/.test(EDITOR_HTML), '编辑器页面里仍然有 download()');
  const body = /function exportAssetsBin\(\)\s*\{([\s\S]*?)\n\}/.exec(EDITOR_HTML);
  ok(!!body && /\bdownload\(/.test(body[1]),
     'exportAssetsBin() 里真的会调 download() ⇒ 外壳"把它截下来"这套办法有效');
}

// ------------------------------------------------------------
console.log('\n' + '='.repeat(56));
if (fail === 0) console.log('全部通过:' + pass + ' 项断言（没发请求、没碰串口、没跑 esptool）');
else {
  console.log('失败 ' + fail + ' 项 / 共 ' + (pass + fail) + ' 项:');
  failures.forEach((f) => console.log('  - ' + f));
}
console.log('='.repeat(56));
process.exit(fail === 0 ? 0 : 1);
