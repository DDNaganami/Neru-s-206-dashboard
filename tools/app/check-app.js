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
 *        预览/编辑器/退出）在 index.html 里都还找得到控件 —— 简化界面可以藏，
 *        但不能把能力弄丢；
 *     ⑦ 三步刷固件 + 去术语：两块大卡片、一个「开始升级」按钮，
 *        并且**折叠区（高级）以外**不许出现那批术语（OTA/热点/槽/校验/env/bin/
 *        slave/master/heap/psram/running/target）—— 车主看不懂的词只能待在「高级」里。
 *     ⑧ **表盘屏只有一个编辑器**（2026-10-16「打包 + 客户端集成」那一单）：
 *        配色与图片是**同一个页面**（index.html）里的两个页内分区 ⇒ 外壳里出现的
 *        编辑器地址只许有一个，图片入口落到那个页面的 `#images`；而 image-editor.html
 *        仍然只是"老链接还能用"的**无脚本跳转薄壳**，外壳不再引用它。
 *        （原来那条"两个编辑器页各一个 iframe"的断言必然红 —— 所以是**换成这一条**，
 *          不是把门槛调低。）
 *     ⑨ 编辑器在**客户端里**才走本机能力：保存安装包 → 选串口 → 客户端现成的
 *        /api/ports + /api/upload + /api/flash（kind 固定 assets，**请求体里没有偏移**）；
 *        没有那个服务（file:// 双击打开）时面板不出现 ⇒ 页面只剩原来那条 esptool 命令。
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

// ③ iframe src 存在（地址里可能带 `#锚点` —— 查文件时先把它去掉）
const srcs = [...html.matchAll(/<iframe[^>]*\bsrc="([^"]+)"/g)].map((m) => m[1]);
console.log('③ iframe：' + srcs.join(', '));
for (const s of srcs) {
  if (s.startsWith('http')) continue;
  const p = path.join(dir, '..', '..', s.replace(/[#?].*$/, '').replace(/^\//, ''));
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
  '编辑器框(唯一)': 'fr-theme', '编辑器面板': 'sub-editor',
  '编辑器新窗口(配色)': 'b-theme-open', '编辑器新窗口(图片)': 'b-image-open',
  '写两块板按钮': 'b-flash-both', '读角色按钮': 'b-roles', '角色那一行': 'roles-line',
  '刷存下来的文件': 'b-flash-file', '写到哪块': 'f-kind',
  '退出应用': 'b-quit', '两块板状态条(左)': 'chip-left',
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

// ⑧ 表盘屏**只有一个编辑器**：配色与图片是同一个页面里的两个页内分区
//   ★ 这一条替代了原来"两个编辑器页各一个 iframe"的断言（两页合并之后那条必然红）。
//   判据是**地址**：外壳里出现的编辑器地址只许有一个（图片那颗多一个 #images 锚点），
//   而且外壳不再指向 image-editor.html —— 它只剩"老链接还能用"这一个职责。
const EDITOR_ADDR = '/tools/theme-editor/index.html';
const noHash = (u) => u.replace(/[#?].*$/, '');
const shellAddrs = srcs
  .concat([...js.matchAll(/window\.open\(\s*'([^']+)'/g)].map((m) => m[1]))
  .concat([...js.matchAll(/EDITOR_URL\s*=\s*'([^']+)'/g)].map((m) => m[1]))
  .filter((u) => u.indexOf('/tools/theme-editor/') >= 0)
  .map(noHash);
const uniqAddrs = [...new Set(shellAddrs)];
console.log('⑧ 外壳里的编辑器地址：' + (uniqAddrs.join(', ') || '（一个都没有）'));
say(srcs.length === 1, `表盘屏只许有 1 个 iframe（编辑器那一个），现在是 ${srcs.length} 个：${srcs.join(', ')}`);
say(uniqAddrs.length === 1 && uniqAddrs[0] === EDITOR_ADDR,
    `编辑器地址只许有这一个：${EDITOR_ADDR}；现在是：${uniqAddrs.join(', ') || '（无）'}`);
say(noHash(srcs[0] || '') === EDITOR_ADDR,
    `那个 iframe 加载的是编辑器页面（${EDITOR_ADDR}），现在是 ${srcs[0] || '（无）'}`);
// 图片入口 ⇒ 落到那个页面的 #images（老「图片页」的地址就是这个锚点）
say(/data-sub="image"/.test(html), '少了「图片素材」那颗页签（data-sub="image"）');
say(/image:\s*'images'/.test(js), "「图片素材」映射到编辑器页内的 images 分区（#images）");
say(/window\.open\(EDITOR_URL \+ '#images', '_blank'\)/.test(js),
    '「在新窗口打开（图片）」打开的是同一个 index.html + #images');
say(/theme:\s*'theme'/.test(js) && /window\.open\(EDITOR_URL, '_blank'\)/.test(js),
    '「主题配色」与它那颗「在新窗口打开」用的是同一个地址（无锚点）');
say(!/theme-editor\/image-editor\.html/.test(html + js),
    '外壳不再引用 image-editor.html（老链接仍由那个文件自己兜）');
// image-editor.html 仍是**无脚本的跳转薄壳**（先剥掉 HTML 注释再查代码）
const shellHtml = fs.readFileSync(path.join(dir, '..', 'theme-editor', 'image-editor.html'), 'utf8');
const shellCode = shellHtml.replace(/<!--[\s\S]*?-->/g, '');
say((shellCode.match(/<script\b/gi) || []).length === 0,
    'image-editor.html 里没有 <script>（不许有第二份实现）');
say(/<meta\s+http-equiv="refresh"\s+content="0;\s*url=index\.html#images"/i.test(shellHtml),
    'image-editor.html 仍是一条跳 index.html#images 的 meta refresh（老链接还能用）');

// ⑨ 编辑器在**客户端里**才走本机能力：保存安装包 → 选串口 → 现成的 /api/flash
//   ★ 判据都是静态可查的：用哪三条接口、kind 是什么、**请求体里有没有偏移**。
//     真正的偏移正确性由 tools/app/check-flash-offset.py 拿 server.py 的白名单跑一遍
//     （它把 run() 换成假的，所以**不碰串口、不跑 esptool**）。
const page = fs.readFileSync(path.join(dir, '..', 'theme-editor', 'index.html'), 'utf8');
say(/FLASH_API\s*=\s*\{\s*health:\s*"\/api\/health",\s*ports:\s*"\/api\/ports",\s*upload:\s*"\/api\/upload",\s*flash:\s*"\/api\/flash"\s*\}/.test(page),
    '编辑器用的是现成的 /api/health + /api/ports + /api/upload + /api/flash（没有第二套刷写）');
say(/id="owner-flash" style="display:none"/.test(page),
    '「写进板子」那一块默认是隐藏的（没有那个服务时页面只剩原来那条命令）');
say(/ownerClientProbe|FLASH_API\.health/.test(page) && /proto !== "http:"/.test(page),
    '面板的显隐由"那个服务答不答得出来"决定（file:// 下不试）');
const flashBody = /JSON\.stringify\(\{\s*port:\s*port,\s*kind:\s*"assets",\s*path:\s*up\.path\s*\}\)/.exec(page);
say(!!flashBody, '刷写请求体恰好是 {port, kind:"assets", path}（与「工具」屏那个按钮同一条路）');
say(!!flashBody && !/offset|偏移\s*:/.test(flashBody[0]),
    '刷写请求体里没有"偏移"这个字段 —— 偏移由客户端按分区表白名单取，页面上没有入口');

// ⑩ 端口下拉**标出角色** + 一次写两块板（2026-10-15 车主："标出角色吧…你写进板子这个
//   只有一个按钮，也没区分主/从板啊"）
//   ★ 角色是**认出来**的，不是猜的：判据在 server.py（不变的 MAC ↔ USB 的 SER= /
//     板子自己起的 AP 名 / 它自报的 role=）。这里钉的是"外壳有没有按那套显示、
//     有没有偷偷按 COM 号推、两块那条路有没有退化成第二套刷写"。
const srvSrc = fs.readFileSync(path.join(dir, 'server.py'), 'utf8');
say(/id="b-flash-both"[^>]*>写进两块板/.test(html), '少了「写进两块板」那颗按钮（车主的核心诉求）');
say(/id="b-roles"/.test(html) && /id="roles-line"/.test(html),
    '少了「读一下角色」按钮 / 角色那一行');
say(/state\.boards = state\.ports\.filter\(\(p\) => p\.is_project_board\)/.test(js),
    '「本项目的板」由服务端的 is_project_board 说了算（外壳不自己认）');
say(/p\.label \|\| p\.port/.test(js),
    '下拉用的是服务端给的**带角色**标签（p.label），不是光一个 COM 号');
say(/state\.ports\.some\(\(p\) => p\.port === keep\)/.test(js),
    '刷新串口不会把用户已经选好的那一个清掉');
// ★ 不猜：外壳里不许有"按 COM 号推角色"的写法，也不许自己造一个默认角色
say(!/COM\d+[^\n]{0,40}(MASTER|SLAVE)|(MASTER|SLAVE)[^\n]{0,40}COM\d+/.test(js),
    '外壳里没有"哪个 COM 号 = 哪个角色"的硬编码（COM 号会变，不能靠它）');
say(!/role\s*[:=]\s*['"](MASTER|SLAVE)['"]/.test(js),
    '外壳没有自己造角色（角色只从服务端来；认不出就是「角色未知」）');
// 两块那条路：先亮出目标 → 只写认出来的板 → 只调现成的 /api/flash（kind=assets）
say(/将要写这两块：/.test(js), '动手前先把"将要写这两块"逐行亮出来（确认框）');
say(/if \(!state\.boards\.length\)/.test(js), '一块都没认出来时不硬写（给一句能照做的说明）');
const bothCall = /api\('\/api\/flash', \{ port: b\.port, kind: 'assets', path: up\.path \}\)/.exec(js);
say(!!bothCall, '两块那条路也是调现成的 /api/flash（kind=assets）——没有第二套刷写实现');
say(!!bothCall && !/offset/.test(bothCall[0]), '那个请求体里同样没有偏移字段');
say(/for \(let i = 0; i < state\.boards\.length; i\+\+\)/.test(js),
    '两块是**依次**写（逐块 await），不是并发冲两个口');
// 逐块报结果，失败不许被说成成功
say(/'✓ ' : '✗ '/.test(js) && /没写成/.test(js), '写完之后逐块摊开结果（成功/失败各自一行）');
say(/bad\.length/.test(js) && /单独重刷/.test(js),
    '有失败就点出来是哪块、并说明可以单独重刷（不假装成功）');
say(/say\('有 ' \+ bad\.length \+ ' 块没写成/.test(js),
    '有失败时说的是"没写成"，不是"写完了"');
// 服务端那半边：角色怎么来的、未知怎么写、有没有多开一个刷写口子
say(/角色未知/.test(srvSrc) && /不是本项目的板/.test(srvSrc),
    'server.py 里"认不出角色"与"不是本项目的板"都有明确说法（未知不猜）');
say(/AP_SSID_ROLE = \{"206dash-master": "MASTER", "206dash-slave": "SLAVE"\}/.test(srvSrc),
    '角色来自板子自己起的 AP 名（206dash-MASTER|SLAVE）—— 不是按 COM 号推的');
say(/BSSID/.test(srvSrc), 'server.py 会读 BSSID（= 板子的 MAC）');
say(/ssid_role == role and mac/.test(srvSrc),
    '★ 只有"连的就是这个角色的板子 AP"才写缓存（连着家里路由器读 /status 时不许凭空记）');
say((srvSrc.match(/"\/api\/flash"/g) || []).length === 1,
    'server.py 里 /api/flash 只有一个入口（两块那条路没有在服务端另开一个刷写实现）');
say(!/^\s*(?:import|from)\s+serial\b/m.test(srvSrc),
    'server.py 没有引入任何串口库（认角色走的是既有能力：pio device list + netsh + /status）');

// ⑪ 「写进板子」**不产生文件**、**不再要求先保存**（2026-10-15 车主："为什么每次刷写都要
//   保存一次 bin 呢，这样我的下载文件夹会有很多个相同的 bin。"）
//   ★ 做法：现场用编辑器**当前状态**打容器（复用编辑器唯一的打包路 exportAssetsBin），
//     把它的 download() 临时截下来 ⇒ <a download> 不会发生。
//   ★ 更细的逻辑判据在 check-live-flash.js（拿假的编辑器 window 把这三个函数真跑一遍）。
const jsCode = js.replace(/\/\*[\s\S]*?\*\//g, '').replace(/(^|[^:])\/\/[^\n]*/g, '$1');
say(/id="b-flash"[^>]*>写进这块板（配色\+图片，不存文件）/.test(html),
    '「写进这块板」按钮明说**不存文件**');
say(/不会往下载文件夹里丢文件/.test(html), '卡片上明说不会往下载文件夹里丢文件');
say(/保存 = 留一份快照/.test(html) && /写进板子 = 直接刷/.test(html),
    '「保存 = 留快照」与「写进板子 = 直接刷」两件事分开写清了');
say((js.match(/buildAssetsInMemory\(\)/g) || []).length >= 3,
    '两颗按钮（这块板 / 两块板）都是现场用 buildAssetsInMemory() 打容器');
// 两条路各查各的：主按钮**不看文件输入**（不要求先选文件/先保存）；刷文件那条仍然读文件
const liveBody = /\$\('#b-flash'\)\.onclick[\s\S]*?\)\);\n/.exec(js);
say(!!liveBody && !/f-bin/.test(liveBody[0]) && /doWrite\(false/.test(liveBody[0]),
    '「写进这块板」这条路**不看文件输入**（不要求先选文件、也不要求先保存）');
say(/async function doWrite\(/.test(js) && /if \(!checkEditorApi\(\)\) return;/.test(js),
    '两条路都先过 doWrite：版本自检不过就当场喊，不往下走（绝不静默）');
say(/async function writeOneBoard\(/.test(js) && /async function writeBothBoards\(/.test(js) &&
    (js.match(/function buildAssetsInMemory\(/g) || []).length === 1,
    '单块/两块共用同一份实现（buildAssetsInMemory 只有一份）');
const fileBody = /\$\('#b-flash-file'\)\.onclick[\s\S]*?\n  \};/.exec(js);
say(!!fileBody && /f-bin/.test(fileBody[0]),
    '「把这个文件写进上面选的那块板」那条路仍然读文件输入（能力不丢）');
say(/w\.download = function \(blob\) \{ captured = blob; \}/.test(js),
    '把编辑器的 download() 临时换成"截字节" ⇒ 不会点 <a download>');
say(/w\.download = orig;/.test(js), '截完把 download() 还原回去（导出那条路照旧）');
say(!/createObjectURL/.test(jsCode), '外壳里没有 createObjectURL（自己不会造下载）');
say(!/saveOwnerPackage/.test(jsCode), '外壳不调编辑器的「保存安装包」⇒ 刷之前不要求先保存');
say(/w\.exportAssetsBin\(\)/.test(jsCode) && !/AssetPackageCore\.packBytes\s*\(/.test(jsCode),
    '容器一律由编辑器那条唯一的打包路产出（外壳没有第二套打包）');
say(/构建失败：/.test(js) && /编辑器没有给出文件（可能还没有图片）/.test(js),
    '构建失败有**明确报错**（带出编辑器状态栏那一句），不静默');
say(/api\('\/api\/flash', \{ port, kind: 'assets', path: up\.path \}\)/.test(js),
    '「写进这块板」仍然只调现成的 /api/flash（kind=assets）');
say(/id="b-flash-file"/.test(html) && /id="f-kind"/.test(html),
    '原来那条"刷手上这个文件"的路**没删**（藏在折叠区里，能力不丢）');
say(/<summary>想刷一份已经存下来的文件/.test(html),
    '它被收进「想刷一份已经存下来的文件」那个折叠区（默认看不见，但还在）');

// ⑫ 「点了没反应」那一课（2026-10-16 车主实测）—— 五条一起钉住：
//   ① 写入按钮**放到车主正在看的那一屏**（表盘屏，编辑器框上面）；
//   ② 点下去**立刻**有反馈（按钮自己变「正在写…」+ 旁边牌子）；
//   ③ 每种失败都说话（没图片/没串口/版本旧/端口占用），**没有静默 return**；
//   ④ 界面来源（仓库 / exe 包内）+ 版本自检，**不许混搭**，缺函数就显眼报错；
//   ⑤ 静态响应 no-cache（改了代码/重启客户端之后不该还看旧页面）。
say(/id="b-dash-flash"[^>]*>写进这块板（配色\+图片，不存文件）/.test(html) &&
    /id="b-dash-flash-both"[^>]*>写进两块板（不存文件）/.test(html),
    '表盘屏（车主那一屏）上有那两颗写入按钮');
say(/\$\('#b-dash-flash'\)\.onclick = \(\) => doWrite\(false/.test(js) &&
    /\$\('#b-dash-flash-both'\)\.onclick = \(\) => doWrite\(true/.test(js),
    '表盘屏那两颗与工具屏走**同一份实现**（doWrite），不是抄一份');
say(/id="dash-flash-say"/.test(html) && /function dashSay\(/.test(js),
    '按钮旁边有一个牌子（#dash-flash-say）：写没写成一眼看得见');
say(/function btnBusy\(btn, text\)/.test(js) && /btnBusy\(btn, '正在写 ' \+ port \+ '…'\)/.test(js),
    '★ 点下去按钮自己变成「正在写 COMx…」（最直白的"它动了"），完事还原');
say(/绝不会有"点了没反应"|绝不会点了什么都不说/.test(html),
    '页面上明说"绝不会点了什么都不说"（车主那一课）');
say(/点下去应该看到/.test(html), '页面上写清"点完应该看到什么"（便于车主自己判断）');
say(/退出应用/.test(html) && /重新双击/.test(html) && /别让旧进程继续跑/.test(html),
    '页面上有"换过 exe 要完全退出再启动"的提示');
say(/function checkEditorApi\(\)/.test(js) && /编辑器页面太旧/.test(js) && /Ctrl\+F5/.test(js),
    '★ 编辑器页缺东西 ⇒ 显眼报错（太旧 + 怎么更新），不静默');
say(/addEventListener\('load', \(\) => checkEditorApi\(\)\)/.test(js) &&
    /checkEditorApi\(\)\)/.test(js) && /if \(!checkEditorApi\(\)\) return;/.test(js),
    '版本自检接在"编辑器框载入"上，并且**写入之前**先过它');
say(/id="p-pages"/.test(html) && /state\.pagesFrom = h\.pages_from/.test(js) &&
    /pages_from/.test(srvSrc) && /app_js/.test(srvSrc) && /editor_html/.test(srvSrc),
    '页面上有一行「界面：仓库/exe 包内（app.js sha · 编辑器 sha · 时间）」；服务端如实报');
say(/def pages_source\(\)/.test(srvSrc) && /def app_files_dir\(\)/.test(srvSrc) &&
    /p = os\.path\.join\(app_files_dir\(\), name\)/.test(srvSrc) &&
    /return \[os\.path\.join\(pages_source\(\)\[1\], "tools", "theme-editor"\)\]/.test(srvSrc),
    '★ 外壳与编辑器**同一个来源**（pages_source 一处决定）—— 不许"外壳新、编辑器旧"混搭');
say(/def end_headers\(self\)/.test(srvSrc) && /no-store, no-cache, must-revalidate/.test(srvSrc) &&
    (srvSrc.match(/send_header\("Cache-Control"/g) || []).length === 1,
    '★ 所有静态响应都 no-cache（在 end_headers 里统一加一处）⇒ 重启/刷新就能拿到新版');

// ⑬ 读编辑器**自己暴露**的版本/能力标记（车主 2026-10-16 拍板："这样'发的是旧页面'这类坑彻底关上"）
//   约定（编辑器那一页，**只加不改语义**）：
//     window.__OWNER_MODE_VERSION / window.__OWNER_MODE_CAPS / <html data-owner-mode-version>
//   ★ 动态判据（真点、真拦）在 check-click-flash.py：那里会造一个"旧编辑器页"真跑一遍。
say(/const EDITOR_MIN_VERSION = '2026-10-15\.3'/.test(js) &&
    /const EDITOR_NEED_CAPS = \['flashNoSave'\]/.test(js),
    '外壳声明了最低版本 EDITOR_MIN_VERSION 与必需能力 EDITOR_NEED_CAPS（flashNoSave）');
say(/w\.__OWNER_MODE_VERSION/.test(js) && /w\.__OWNER_MODE_CAPS/.test(js) &&
    /data-owner-mode-version/.test(js),
    '★ 三个读法都读了：__OWNER_MODE_VERSION / __OWNER_MODE_CAPS / data-owner-mode-version');
say(/function readEditorMarkers\(\)/.test(js) && /不同源/.test(js) && /catch \(e\) \{/.test(js),
    '读标记失败（跨源 SecurityError）⇒ **明确说"不同源"**，不静默');
say(/缺能力 ' \+ c/.test(js) && /EDITOR_NEED_CAPS\.forEach/.test(js),
    '缺 flashNoSave ⇒ 报的是"缺能力 flashNoSave"（不是笼统一句失败）');
say(/编辑器页面太旧/.test(js) && /退出应用/.test(js) && /Ctrl\+F5/.test(js),
    '★ 报错文案点明「太旧」+ 怎么更新（退出应用 / Ctrl+F5）');
say(/if \(!checkEditorApi\(\)\) return;/.test(js) && /if \(both\) return await writeBothBoards/.test(js),
    '★ 自检不过就 return ⇒ 单块/两块都**不会**往下走（绝不发 /api/flash）');
say(/cmpVersion\(m\.version, EDITOR_MIN_VERSION\) < 0/.test(js) && /能力齐全/.test(js),
    '版本低于最低版但**能力齐全** ⇒ 照写（只提一句），不误拦');
say(/state\.editorVersion = m\.version/.test(js) && /function renderPagesLine\(\)/.test(js) &&
    /编辑器 v' \+ \(state\.editorVersion \|\| '未报版本'\)/.test(js),
    '读到的**实际版本**显示在页面上（和「界面：仓库（…）」同一行）');
say(/state\.editorVersion = h\.editor_version/.test(js) && /h\.editor_caps/.test(js),
    '页面那行的初值来自 /api/health（服务端从"要发出去的那一份"里读的）');
say(/def editor_markers\(\)/.test(srvSrc) && /"editor_version": ""/.test(srvSrc) &&
    /"editor_caps": \{\}/.test(srvSrc),
    '/api/health 里报 editor_version 与 editor_caps（只读解析，不改那一页）');
say(/__OWNER_MODE_VERSION/.test(srvSrc) && /OWNER_MODE_CAPS/.test(srvSrc) &&
    /data-owner-mode-version/.test(srvSrc),
    'server.py 的三个读法与编辑器约定一致（JS 变量 + DOM 属性都认）');

// ⑭ 原生窗口（WebView2）那一层：native.py + 打包脚本 + 界面里的「重新载入」
//   车主 2026-10-16："打开 exe 为什么还是在浏览器里进行操作呢" ⇒ 用**系统已有**的
//   WebView2 开真窗口（不是 Electron/Tauri、不自带内核）。★ 真窗口与"关窗即停服务"
//   是在**打好后的 exe 上实测**的（见 README 的回报），这里钉的是结构别退化。
const nativePy = path.join(dir, 'native.py');
const nativeSrc = fs.existsSync(nativePy) ? fs.readFileSync(nativePy, 'utf8') : '';
const buildPs1 = fs.readFileSync(path.join(dir, 'build-exe.ps1'), 'utf8');
say(!!nativeSrc, '有 native.py（原生窗口宿主）');
say(/import server as app_server/.test(nativeSrc) &&
    /ThreadingHTTPServer\(\("127\.0\.0\.1", port\), app_server\.Handler\)/.test(nativeSrc),
    '★ 用的是**同一个** server.Handler，服务仍只听 127.0.0.1（没有第二套服务）');
say(!/class\s+Handler\b/.test(nativeSrc), 'native.py 里没有另写一个 Handler（路由/白名单不复制）');
say(/webview\.start\(gui="edgechromium"/.test(nativeSrc),
    'pywebview 走 edgechromium（= 系统自带的 WebView2 运行时，不自带内核）');
say(/--devtools/.test(nativeSrc) && /debug=bool\(a\.devtools\)/.test(nativeSrc),
    '--devtools 才开 DevTools（默认关）');
say(/def open_in_browser\(url, why\)/.test(nativeSrc) && /webbrowser\.open\(url\)/.test(nativeSrc) &&
    /WebView2 运行时/.test(nativeSrc),
    '★ 窗口起不来 ⇒ 明确提示（点名 WebView2 怎么装）+ 回退系统浏览器，不静默');
say(/win\.events\.closed \+= on_closed/.test(nativeSrc) && /srv\.shutdown\(\)/.test(nativeSrc),
    '★ 关窗 = 停服务（closed 事件里 shutdown）');
say(/stopped\.wait\(\)/.test(nativeSrc) && /win\.destroy\(\)/.test(nativeSrc),
    '反过来：界面点「退出应用」把服务停了 ⇒ 窗口也一起关（不留空窗口）');
say(/'--console'/.test(buildPs1) && !/'--noconsole'/.test(buildPs1),
    '--console 没被改成 --noconsole（日志照旧看得见）');
say(/'--collect-all', 'webview'/.test(buildPs1) && /'--hidden-import', 'clr'/.test(buildPs1) &&
    /Join-Path \$here 'native\.py'/.test(buildPs1),
    '打包带上 pywebview/pythonnet（--collect-all + hidden-import），入口是 native.py');
say(/if \(\$buildCode -ne 0\)/.test(buildPs1) && /Remove-Item \$exe -Force/.test(buildPs1),
    '★ 打包失败要**当场报错**（先删旧的 exe + 查退出码）—— 免得拿旧 exe 当成功');
say(/id="b-reload"/.test(html) && /\$\('#b-reload'\)\.onclick/.test(js) && /location\.reload\(\)/.test(js),
    '界面上有「重新载入这个窗口」（原生窗口里没有 Ctrl+F5；服务端已 no-cache）');

console.log(bad === 0 ? '\n全部通过。' : `\n${bad} 项没通过。`);
process.exit(bad === 0 ? 0 : 1);
