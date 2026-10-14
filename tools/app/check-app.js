#!/usr/bin/env node
/*
 * check-app.js -- 表盘助手外壳的静态闸门（`node tools/app/check-app.js`）
 *
 * 为什么要有它：这个应用是"一堆 id + 一堆选择器 + 几个 iframe"拼起来的，
 *   而**在浏览器里点一遍**不是每次都能做（这台机器上常常没板子、也没法点）。
 *   而这类错（选择器写错一个字母、屏 id 与 section 对不上、iframe 指到不存在的
 *   文件）在运行时**只表现为"点了没反应"**，最难查。所以在这里一次钉死：
 *     ① app.js 里 `$('#x')` 用到的每个 id，index.html 里必须有；
 *     ② 顶栏就是三个屏（刷固件/表盘/工具），每个屏有对应的 `<section id="tab-<id>">`；
 *     ③ iframe 的 src（本地的）必须真实存在；
 *     ④ 屏内跳转用的 data-go 必须指向存在的屏；
 *     ⑤ 外壳用到的每个 `/api/*` 在 server.py 里真有路由；
 *     ⑥ **能力表**：每一项老能力（构建/烧写/列串口/固件列表/板子状态/确认升级成功/
 *        预览/两个编辑器/退出）在 index.html 里都还找得到控件 —— 简化界面可以藏，
 *        但不能把能力弄丢；
 *     ⑦ 三步刷固件 + 去术语：两块大卡片、一个「开始升级」按钮，
 *        并且**折叠区（高级）以外**不许出现那批术语（OTA/热点/槽/校验/env/bin/
 *        slave/master/heap/psram/running/target）—— 车主看不懂的词只能待在「高级」里。
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

// ② 屏 ↔ section（三个屏，不是七个页签）
const tabs = [...js.matchAll(/\{\s*id:\s*'([a-z0-9-]+)'\s*,\s*title:/g)].map((m) => m[1]);
console.log('② 屏幕：' + tabs.join(', '));
say(tabs.length === 3, `顶栏应当是 3 个屏（刷固件/表盘/工具），现在是 ${tabs.length} 个`);
for (const t of tabs) say(ids.has('tab-' + t), `屏 ${t} 没有对应的 <section id="tab-${t}">`);

// ③ iframe src 存在
const srcs = [...html.matchAll(/<iframe[^>]*\bsrc="([^"]+)"/g)].map((m) => m[1]);
console.log('③ iframe：' + srcs.join(', '));
for (const s of srcs) {
  if (s.startsWith('http')) continue;
  const p = path.join(dir, '..', '..', s.replace(/^\//, ''));
  say(fs.existsSync(p), `iframe src="${s}" 指向的文件不存在：${p}`);
}

// ④ data-go 指向存在的屏
const gos = [...html.matchAll(/data-go="([^"]+)"/g)].map((m) => m[1]);
console.log('④ 屏内跳转 data-go：' + [...new Set(gos)].join(', '));
for (const g of new Set(gos)) say(tabs.includes(g), `data-go="${g}" 不是任何屏`);

// ⑤ 服务端兑现的接口，外壳里用到的都应当在 server.py 里有对应路由
const srv = fs.readFileSync(path.join(dir, 'server.py'), 'utf8');
const eps = new Set([...js.matchAll(/['"`](\/api\/[a-z-]+)/g)].map((m) => m[1]));
console.log('⑤ 外壳用到的接口：' + [...eps].join(', '));
for (const e of eps) say(srv.includes('"' + e + '"'), `server.py 里没有 ${e} 这条路由`);

// ⑥ 能力表：界面可以简化/折叠，但这些控件一个都不能少
const KEEP = {
  '构建固件': 'b-build', '构建档位': 'b-env', 'USB 烧写': 'b-flash', '列串口': 'b-ports',
  '选串口': 'f-port', '读固件文件': 'f-bin', '固件列表': 'bins-kv', '刷新固件列表': 'b-bins',
  '板子原始状态': 'b-out', '刷新板子状态': 'b-refresh', '自动刷新开关': 'b-auto',
  '板子地址（状态）': 'b-host', '板子地址（升级）': 'ota-host', '板子串口': 'ota-port',
  '固件来源': 'ota-src', '从本机选固件': 'ota-file', '固件下拉': 'ota-bin',
  '开始升级按钮': 'b-ota', '只检查不升级': 'b-ota-check', '检查板子': 'b-board',
  '预览运行': 'b-preview', '预览刷新': 'b-pv-reload', '预览宿主': 'pv-host',
  '配色编辑器': 'fr-theme', '图片编辑器': 'fr-image', '配色新窗口': 'b-theme-open',
  '图片新窗口': 'b-image-open', '退出应用': 'b-quit', '两块板状态条(左)': 'chip-left',
  '两块板状态条(右)': 'chip-right', '升级进度条': 'ota-bar', '升级原始日志': 'ota-log',
};
console.log('⑥ 能力控件：' + Object.keys(KEEP).length + ' 项');
for (const [what, id] of Object.entries(KEEP)) say(ids.has(id), `「${what}」的控件 #${id} 没了 —— 能力被删掉了`);

// ⑦ 三步刷固件 + 去术语
say(/data-board="left"/.test(html), '少了「转速表（左）」那张大卡片（data-board="left"）');
say(/data-board="right"/.test(html), '少了「速度表（右）」那张大卡片（data-board="right"）');
say(/id="b-ota"[^>]*>开始升级/.test(html), '第 3 步那个唯一的「开始升级」按钮不见了');
const steps = [...html.matchAll(/class="stepn">(\d)</g)].map((m) => m[1]);
console.log('⑦ 刷固件步骤：' + steps.join(' → ') + '；折叠区（高级）：' +
  (html.match(/<details/g) || []).length + ' 个');
say(steps.join(',') === '1,2,3', '刷固件应当是显式的三步（1/2/3），现在是 ' + (steps.join(',') || '无'));
say((html.match(/<summary>[^<]*高级/g) || []).length >= 2, '技术细节没有收进「高级」折叠区');
// 折叠区以外（用户默认看得见的地方）不许出现这批词。
//   只看**文本**（先剥掉标签，否则 target="_blank" 这种属性也算词）；
//   真实文件名（image.bin/assets.bin）与真实 WiFi 名（206dash-SLAVE）照原样显示，不算术语。
const outside = html.replace(/<details[\s\S]*?<\/details>/g, '')
  .replace(/<[^>]*>/g, ' ')
  .replace(/206dash-(SLAVE|MASTER)/g, '板子的 WiFi');
const jargon = ['OTA', '热点', '槽位', '目标槽', '验槽', '校验', 'psram', 'heap', 'running', 'target']
  .filter((w) => outside.includes(w))
  .concat([
    /\benv\b/i.test(outside) ? 'env' : null,
    /(?<![\w.])bin(?![\w])/i.test(outside) ? 'bin' : null,
    /\bslave\b|\bmaster\b/i.test(outside) ? 'slave/master' : null,
  ].filter(Boolean));
say(jargon.length === 0, '「高级」以外还看得到这些术语：' + jargon.join('、'));

console.log(bad === 0 ? '\n全部通过。' : `\n${bad} 项没通过。`);
process.exit(bad === 0 ? 0 : 1);
