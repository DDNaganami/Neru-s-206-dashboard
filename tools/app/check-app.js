#!/usr/bin/env node
/*
 * check-app.js -- 开发台外壳的静态闸门（`node tools/app/check-app.js`）
 *
 * 为什么要有它：这个应用是"一堆 id + 一堆选择器 + 几个 iframe"拼起来的，
 *   而**在浏览器里点一遍**不是每次都能做（这台机器上常常没板子、也没法点）。
 *   而这类错（选择器写错一个字母、页签 id 与 section 对不上、iframe 指到不存在的
 *   文件）在运行时**只表现为"点了没反应"**，最难查。所以在这里一次钉死：
 *     ① app.js 里 `$('#x')` 用到的每个 id，index.html 里必须有；
 *     ② TABS 里的每个页签，必须有对应的 `<section id="tab-<id>">`；
 *     ③ iframe 的 src（本地的）必须真实存在；
 *     ④ 页签导航用的 data-go 必须指向存在的页签。
 *
 * 退出码 0 = 全过；1 = 有问题（逐条打印）。
 */
'use strict';
const fs = require('fs');
const path = require('path');

const dir = __dirname;
const htmlPath = path.join(dir, 'index.html');
const jsPath = path.join(dir, 'app.js');
const html = fs.readFileSync(htmlPath, 'utf8');
const js = fs.readFileSync(jsPath, 'utf8');

let bad = 0;
const say = (ok, msg) => { if (!ok) { bad++; console.log('  ✗ ' + msg); } return ok; };

// ① id 是否有定义
const ids = new Set([...html.matchAll(/\bid="([^"]+)"/g)].map((m) => m[1]));
const used = new Set([...js.matchAll(/\$\('#([A-Za-z0-9_-]+)'\)/g)].map((m) => m[1]));
console.log('① app.js 用到的 id：' + used.size + ' 个');
for (const id of used) say(ids.has(id), `app.js 用了 #${id}，但 index.html 里没有这个 id`);

// ② 页签 ↔ section
const tabs = [...js.matchAll(/\{\s*id:\s*'([a-z0-9-]+)'\s*,\s*title:/g)].map((m) => m[1]);
console.log('② 页签：' + tabs.join(', '));
say(tabs.length >= 6, '页签少于 6 个（用户要的 6 项能力应当都在）');
for (const t of tabs) say(ids.has('tab-' + t), `页签 ${t} 没有对应的 <section id="tab-${t}">`);

// ③ iframe src 存在
const srcs = [...html.matchAll(/<iframe[^>]*\bsrc="([^"]+)"/g)].map((m) => m[1]);
console.log('③ iframe：' + srcs.join(', '));
for (const s of srcs) {
  if (s.startsWith('http')) continue;
  const p = path.join(dir, '..', '..', s.replace(/^\//, ''));
  say(fs.existsSync(p), `iframe src="${s}" 指向的文件不存在：${p}`);
}

// ④ data-go 指向存在的页签
const gos = [...html.matchAll(/data-go="([^"]+)"/g)].map((m) => m[1]);
console.log('④ 快捷按钮 data-go：' + [...new Set(gos)].join(', '));
for (const g of new Set(gos)) say(tabs.includes(g), `data-go="${g}" 不是任何页签`);

// ⑤ 服务端兑现的接口，外壳里用到的都应当在 server.py 里有对应路由
const srv = fs.readFileSync(path.join(dir, 'server.py'), 'utf8');
const eps = new Set([...js.matchAll(/['"`](\/api\/[a-z-]+)/g)].map((m) => m[1]));
console.log('⑤ 外壳用到的接口：' + [...eps].join(', '));
for (const e of eps) say(srv.includes('"' + e + '"'), `server.py 里没有 ${e} 这条路由`);

console.log(bad === 0 ? '\n全部通过。' : `\n${bad} 项没通过。`);
process.exit(bad === 0 ? 0 : 1);
