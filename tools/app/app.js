/* ============================================================================
 * 206Dash 表盘助手 —— 应用外壳
 *
 * 结构（要加功能就改这一处）：
 *   SCREENS —— 顶栏那三个屏：{id, 标题}。加一个屏 = 加一条 + HTML 里放一个
 *              <section id="tab-<id>">；需要初始化就在下面加一个 init 函数。
 *   三个屏  —— ① 刷固件（含开发板状态）② 表盘（配色/图片/预览）③ 工具（USB 素材 + 构建）
 *   其它    —— 每个功能一个 init 函数，最后在 init() 里逐个挂上（每个都单独 try，
 *              一个坏了不连累别的 —— 否则症状是"点了没反应"，最难查）。
 *
 * 写给非技术用户的规矩（改文案时照这个来）：
 *   OTA→无线升级 | 热点→板子的 WiFi | env/bin→固件 | 校验/验槽→确认升级成功
 *   slave/master→转速表/速度表（左/右）| 槽位/running/target/heap/psram→只出现在「高级」里
 * ==========================================================================*/
'use strict';

/* 三个屏。id 必须和 <section id="tab-<id>"> 对上（check-app.js 会钉这件事）。 */
const SCREENS = [
  { id: 'flash', title: '刷固件' },
  { id: 'dash',  title: '表盘' },
  { id: 'tools', title: '工具' },
];

/* 两块表：slave=从板=转速表=左屏，master=主板=速度表=右屏。
   串口默认值沿用老界面的经验值（左表常用 COM8、右表常用 COM9）。 */
const BOARDS = {
  left:  { name: '转速表（左）', ssid: '206dash-SLAVE',  env: 'esp32s3-rgb-slave-ota',  port: 'COM8' },
  right: { name: '速度表（右）', ssid: '206dash-MASTER', env: 'esp32s3-rgb-master-ota', port: 'COM9' },
};
const ROLE_TO_BOARD = { slave: 'left', master: 'right' };
/* env → 人话。英文档位名只在「高级」里出现。 */
const ENV_HUMAN = {
  'esp32s3-rgb-slave-ota': '转速表（左）· 无线升级',
  'esp32s3-rgb-master-ota': '速度表（右）· 无线升级',
  'esp32s3-rgb-slave-obdtest': '转速表（左）· 测试版（不能无线升级）',
  'esp32s3-rgb-slave-now': '转速表（左）· 临时版',
  'esp32s3-rgb-master-now': '速度表（右）· 临时版',
  'pcpreview': '电脑上的预览（不是固件）',
};

const state = { board: 'left', bins: [], upgrading: false };

const $ = (s) => document.querySelector(s);
const setStatus = (s) => { $('#f-status').textContent = s; };
const human = (n) => {
  if (n == null) return '—';
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1024 / 1024).toFixed(2) + ' MB';
};
async function api(path, body) {
  const opt = body
    ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }
    : {};
  const r = await fetch(path, opt);
  return r.json();
}
function logTo(sel, text, append) {
  const el = $(sel);
  el.textContent = append ? (el.textContent + text) : text;
  el.scrollTop = el.scrollHeight;
}
function kv(sel, pairs) {
  $(sel).innerHTML = pairs.map(([k, v]) => `<b>${k}</b><span>${v}</span>`).join('');
}
/* 第 3 步下面那一行"人话状态"（原始日志在「高级」里） */
function setFw(text, kind) {
  const el = $('#fw-status');
  el.innerHTML = text;
  el.className = 'fwstatus' + (kind ? ' ' + kind : '');
}

/* ---- 屏切换 --------------------------------------------------------------*/
function buildNav() {
  const nav = $('#nav');
  nav.innerHTML = '';
  SCREENS.forEach((t) => {
    const b = document.createElement('button');
    b.textContent = t.title;
    b.dataset.tab = t.id;
    b.onclick = () => go(t.id);
    nav.appendChild(b);
  });
  document.querySelectorAll('[data-go]').forEach((el) => {
    el.onclick = () => go(el.dataset.go);
  });
}
function go(id) {
  if (!SCREENS.some((s) => s.id === id)) id = SCREENS[0].id;   // 旧书签（#ota 之类）也不至于白屏
  document.querySelectorAll('#nav button').forEach((b) => b.classList.toggle('sel', b.dataset.tab === id));
  document.querySelectorAll('main section').forEach((s) => s.classList.toggle('sel', s.id === 'tab-' + id));
  location.hash = id;
}
/* 表盘屏里的二级页签（配色 / 图片 / 预览）—— 三个面板一直都在，只是显示一个 */
function subGo(kind) {
  const panels = { theme: $('#sub-theme'), image: $('#sub-image'), preview: $('#sub-preview') };
  if (!panels[kind]) kind = 'theme';
  Object.keys(panels).forEach((k) => { if (panels[k]) panels[k].classList.toggle('sel', k === kind); });
  document.querySelectorAll('#subnav button').forEach((b) => b.classList.toggle('sel', b.dataset.sub === kind));
}

/* ---- 服务自检 / 固件列表 -------------------------------------------------*/
async function initServer() {
  try {
    const h = await api('/api/health');
    $('#p-srv').textContent = '服务：' + (h.ok ? '在线' : '异常') + (h.frozen ? '（打包版）' : '');
    $('#p-srv').className = 'pill ' + (h.ok ? 'on' : 'off');
    $('#p-repo').textContent = h.repo + (h.frozen ? '  ·  exe: ' + h.exe : '');
    $('#p-repo').title = '本地服务只监听 127.0.0.1；只服务这个仓库目录';
    if (!h.repo_ok) {
      const msg = '找不到项目文件夹（仓库）：配色、图片、预览这几页用不了。' +
        '让这个程序带上仓库路径重开一次就行：<code>--repo &lt;仓库路径&gt;</code>';
      $('#banner').innerHTML = msg;
      $('#banner').className = 'banner on';
      $('#dash-warn').innerHTML = msg;
      $('#dash-warn').className = 'banner on';
      setStatus('没找到仓库 —— 配色/图片/预览用不了');
    }
  } catch (e) {
    $('#p-srv').textContent = '服务：离线（用 tools/app/run.bat 或那个 exe 启动）';
    $('#p-srv').className = 'pill off';
    setStatus('本地服务没起来 —— 重新双击那个 exe');
  }
}

async function refreshBins() {
  const r = await api('/api/bins');
  const bins = r.bins || [];
  state.bins = bins;
  kv('#bins-kv', bins.length
    ? bins.map((b) => [b.env, `${human(b.bytes)} · ${b.mtime}${b.ota_ok ? ' · <span class="ok">可无线升级</span>' : ''}`])
    : [['（空）', '还没有构建产物；去「工具 → 高级：构建新固件」构建一次']]);
  const sel = $('#ota-bin');
  sel.innerHTML = bins.map((b, i) =>
    `<option value="${i}">${ENV_HUMAN[b.env] || b.env} — ${human(b.bytes)} · ${b.mtime}</option>`).join('')
    || '<option value="">（还没有构建好的固件）</option>';
  const envSel = $('#b-env');
  if (envSel.options.length === 0) {
    envSel.innerHTML = ['esp32s3-rgb-slave-ota', 'esp32s3-rgb-master-ota',
      'esp32s3-rgb-slave-obdtest', 'esp32s3-rgb-slave-now', 'esp32s3-rgb-master-now', 'pcpreview']
      .map((e) => `<option>${e}</option>`).join('');
  }
  autoPickFirmware();
}

/* 第 2 步：自动挑这块表该用的最新固件（列表已按时间倒序，最新的在最前面） */
function autoPickFirmware() {
  const bins = state.bins;
  if (!bins.length) {
    $('#fw-pick').innerHTML = '这台电脑上还没有构建好的固件 —— 去「工具 → 高级：构建新固件」构建一次。';
    return;
  }
  const want = BOARDS[state.board].env;
  let i = bins.findIndex((b) => b.env === want && b.ota_ok);
  if (i < 0) i = bins.findIndex((b) => b.env === want);
  if (i < 0) i = bins.findIndex((b) => b.ota_ok);
  if (i < 0) i = 0;
  const sel = $('#ota-bin');
  if (sel.options.length === bins.length) sel.value = String(i);
  const b = bins[i];
  $('#fw-pick').innerHTML = `已自动选好：<b>${ENV_HUMAN[b.env] || b.env}</b> · ${human(b.bytes)} · ${b.mtime}` +
    (b.ota_ok ? '' : '<br><span class="note">★ 这一档不支持无线升级 —— 装上去之后就再也不能无线升级了。</span>');
}

/* ---- 两块板的状态（顶栏 + 第 1 步那两条，同一份数据，绝不会互相打架） ----*/
function chipHTML(key, online, fw) {
  const name = BOARDS[key].name;
  return online
    ? `<b>${name}</b><span class="st on">在线</span><span class="fw">固件 ${fw || '—'}</span>`
    : `<b>${name}</b><span class="st off">未连接</span><span class="fw">电脑没连它的 WiFi</span>`;
}
function applyBoard(st) {
  const which = st.online && ROLE_TO_BOARD[st.role] ? ROLE_TO_BOARD[st.role]
    : (st.online ? state.board : null);
  /* 写成字面量而不是拼字符串：check-app.js 只认"$('# + 名字 + ')"这种单引号写法，
     拼出来的选择器会从静态检查里漏掉（漏掉 = 以后改错没人拦）。 */
  const chips = { left: $('#chip-left'), right: $('#chip-right') };
  Object.keys(chips).forEach((k) => {
    chips[k].innerHTML = chipHTML(k, which === k, which === k ? st.fw : '');
  });
  if (st.online) {
    $('#p-board').textContent = '板子：' + BOARDS[which].name + ' · 在线';
    $('#p-board').className = 'pill on';
    const miss = which !== state.board
      ? `　★ 但现在连上的是<b>${BOARDS[which].name}</b>，和你第 1 步选的不一样。` : '';
    $('#board-hint').innerHTML = `通了（${st.ms}ms）· 正在跟 <b>${BOARDS[which].name}</b> 说话。` + miss;
    if (st.raw) kv('#board-kv', Object.entries(st.raw).map(([k, v]) => [k, String(v)]));
  } else {
    $('#p-board').textContent = '板子：未连接';
    $('#p-board').className = 'pill off';
    $('#board-hint').innerHTML = '连不上。顺序是：① 让板子进入接收状态（第 3 步右边那个按钮）' +
      ' ② 电脑 WiFi 连到板子自己的 WiFi ③ 再点「看看板子连上没有」。';
    if (st.err) logTo('#ota-log', '读板子没成功：' + st.err + '\n', true);   // 原始报错只进「高级」日志
    if (st.raw) kv('#board-kv', Object.entries(st.raw).map(([k, v]) => [k, String(v)]));
  }
}
/* 读板子自己的 /status：**先请本地服务代读**（不受浏览器跨域/私网限制，这是可靠的那条路），
   拿不到再试浏览器直连（有些板子的 /status 带 CORS 头）。返回 /status 对象或 null。 */
async function readBoard(host) {
  const s = await api('/api/board-status?host=' + encodeURIComponent(host));
  if (s && !s._error) {
    applyBoard({ online: true, role: s.role, fw: s.fw, raw: s, ms: 0 });
    return s;
  }
  return await boardStatus(host);
}
/* 浏览器直连板子（原「检测开发板」那条路，留着当兜底） */
async function boardStatus(host) {
  const t0 = performance.now();
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), 4000);
  try {
    const r = await fetch(`http://${host}/status`, { signal: ctl.signal });
    clearTimeout(timer);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const j = await r.json();
    applyBoard({ online: true, role: j.role, fw: j.fw, raw: j, ms: Math.round(performance.now() - t0) });
    return j;
  } catch (e) {
    clearTimeout(timer);
    applyBoard({ online: false, err: String(e.message || e) });
    return null;
  }
}

/* ---- ① 刷固件：三步 ------------------------------------------------------*/
function initFlash() {
  const hostOf = () => ($('#ota-host').value.trim() || '192.168.4.1');
  const portOf = () => ($('#ota-port').value.trim() || BOARDS[state.board].port);

  /* 第 1 步：两块大卡片（radio 行为） */
  document.querySelectorAll('[data-board]').forEach((el) => {
    el.onclick = () => {
      state.board = el.dataset.board;
      document.querySelectorAll('[data-board]').forEach((b) =>
        b.classList.toggle('sel', b.dataset.board === state.board));
      $('#ota-port').value = BOARDS[state.board].port;
      autoPickFirmware();
    };
  });
  document.querySelectorAll('[data-board]').forEach((b) =>
    b.classList.toggle('sel', b.dataset.board === state.board));
  $('#ota-port').value = BOARDS[state.board].port;

  /* 「看看板子连上没有」 */
  $('#b-board').onclick = async () => {
    setStatus('读板子…');
    await readBoard(hostOf());
    setStatus('就绪');
  };
  $('#b-ota-check').onclick = () => $('#b-board').click();

  /* 「让它准备接收」：走既有的 /api/ota-verify（板子已经在接收状态就不会再打扰它） */
  $('#b-arm').onclick = async () => {
    const port = portOf();
    if (!confirm(`让 ${BOARDS[state.board].name} 进入接收状态？\n\n会通过串口 ${port} 发一个准备命令，` +
      '板子重启、屏幕黑掉 —— 这都是正常的。')) return;
    setFw('正在让板子准备接收…（最多约 40 秒，屏幕黑掉是正常的）', 'busy');
    setStatus('让板子准备接收…');
    try {
      const r = await api('/api/ota-verify', { host: hostOf(), port, before: '', timeout_s: 40 });
      if (r.ok) {
        setFw(`板子准备好了 ✓ 现在把电脑 WiFi 连到 <b>${BOARDS[state.board].ssid}</b>，再点「开始升级」。`, 'ok');
        logTo('#ota-log', '板子已进入接收状态：' + JSON.stringify(r) + '\n', true);
      } else {
        setFw('没能让板子准备接收：' + (r.error || '') +
          '　（串口 ' + port + ' 选对了吗？USB 线插好了吗？也可以在「高级」里改串口。）', 'bad');
      }
    } catch (e) {
      setFw('让板子准备接收时出错：' + e, 'bad');
    }
    setStatus('就绪');
  };

  /* 「高级」里的固件来源：从本机选文件（默认是用构建好的，用户一般不用动） */
  $('#ota-src').onchange = () => {
    const useFile = $('#ota-src').value === 'file';
    $('#ota-filewrap').style.display = useFile ? '' : 'none';
    $('#ota-bin').style.display = useFile ? 'none' : '';
  };

  /* 第 3 步：一个按钮做完全部（上传 → 板子重启 → 确认成功） */
  $('#b-ota').onclick = async () => {
    if (state.upgrading) return;
    const host = hostOf();
    const useFile = $('#ota-src').value === 'file';
    let file = null, label = '';
    if (useFile) {
      file = $('#ota-file').files[0];
      if (!file) { setFw('先在「高级 → 固件从哪来」里选一个固件文件。', 'bad'); return; }
      label = file.name;
    } else {
      const b = state.bins[Number($('#ota-bin').value)];
      if (!b) { setFw('这台电脑上还没有构建好的固件 —— 先去「工具 → 高级：构建新固件」构建一次。', 'bad'); return; }
      label = `${b.env}/firmware.bin`;
    }
    state.upgrading = true;
    $('#b-ota').disabled = true;
    $('#ota-bar').style.width = '0%';
    setFw('先看一眼板子在不在…', 'busy');

    // 先确认板子在等着收 —— 连不上就别白等二三十秒（用户看到的是一句人话）
    const st = await readBoard(host);
    if (!st) {
      setFw(`还没连上 <b>${BOARDS[state.board].name}</b>。请把电脑 WiFi 连到 <b>${BOARDS[state.board].ssid}</b>，` +
        '再点一次「开始升级」。<br>板子可能还没进入接收状态 —— 那就先点右边那个「板子没反应？让它准备接收」。', 'bad');
      setStatus('没连上板子');
      state.upgrading = false; $('#b-ota').disabled = false;
      return;
    }
    const before = st.running || '';
    logTo('#ota-log', `→ 板子 ${host}：固件 ${st.fw} role=${st.role} running=${st.running} target=${st.target} ota_mode=${st.ota_mode}\n`, false);

    if (useFile) {
      upload(host, file, label, before);
    } else {
      const b = state.bins[Number($('#ota-bin').value)];
      logTo('#ota-log', `→ 取本机构建产物 ${b.env}（${human(b.bytes)}）\n`, true);
      // 由本地服务把文件吐出来（同源），产物在 scratch 镜像里也拿得到
      fetch('/api/bin?env=' + encodeURIComponent(b.env))
        .then((r) => r.blob())
        .then((blob) => upload(host, new File([blob], 'firmware.bin'), label, before))
        .catch((e) => {
          logTo('#ota-log', '读文件失败：' + e + '\n', true);
          setFw('读不到那个固件文件：' + e, 'bad');
          state.upgrading = false; $('#b-ota').disabled = false;
        });
    }
  };

  /* 上传（浏览器直连板子的 /update）—— 用户只看进度条，细节进「高级」日志 */
  function upload(host, file, label, before) {
    const url = `http://${host}/update`;
    logTo('#ota-log', `→ POST ${url}\n  文件 ${label} (${human(file.size)})\n`, true);
    setFw(`正在升级 ${BOARDS[state.board].name}…`, 'busy');
    setStatus('上传中…');
    const fd = new FormData();
    fd.append('firmware', file, 'firmware.bin');
    const xhr = new XMLHttpRequest();
    let settled = false;
    const done = (replied) => {
      if (settled) return;
      settled = true;
      confirmUpgrade(host, before, replied);
    };
    xhr.open('POST', url);
    xhr.upload.onprogress = (e) => {
      if (!e.lengthComputable) return;
      const p = Math.round((e.loaded / e.total) * 100);
      $('#ota-bar').style.width = p + '%';
      setFw(`正在升级 ${BOARDS[state.board].name}…  ${p}%（${human(e.loaded)} / ${human(e.total)}）`, 'busy');
      setStatus(`上传 ${p}%`);
    };
    xhr.onload = () => {
      logTo('#ota-log', `← ${xhr.status} ${xhr.responseText || ''}\n`, true);
      $('#ota-bar').style.width = '100%';
      logTo('#ota-log', '★ 板子收完就重启，回复常常死在那一瞬 —— 所以下面用板子自己的账确认。\n', true);
      done(xhr.status === 200 && /OK/.test(xhr.responseText || ''));
    };
    xhr.onerror = () => {
      logTo('#ota-log', '★ 请求失败 —— 但**这不等于没写进去**：板子收完固件会立刻重启，回复常常死在那一瞬。\n' +
        '  备份路径：连上板子的 WiFi 后直接开 http://192.168.4.1/ 用板子自带的上传页。\n', true);
      done(null);
    };
    xhr.send(fd);
  }

  /* 确认升级成功（原「自动验槽」）：由本地服务代读板子的账，读出来才算数。
     ★ 板子已经在接收状态时不会再发准备命令（否则会把 10 分钟窗口重新计时）。 */
  async function confirmUpgrade(host, before, replied) {
    setFw('固件传完了，正在确认有没有成功…（最多约 1 分钟；板子屏幕黑着是正常的）', 'busy');
    setStatus('确认升级成功…');
    try {
      const r = await api('/api/ota-verify', { host, port: portOf(), before, timeout_s: 70 });
      logTo('#ota-log', '确认结果：' + JSON.stringify(r) + '\n', true);
      if (!r.ok) {
        setFw('升级结果没确认下来：' + (r.error || '') +
          '<br>（板子还在接收状态吗？电脑连上它的 WiFi 了吗？串口选对了吗？）再看一眼「高级」里的日志。', 'bad');
        setStatus('没能确认升级结果');
      } else if (r.flipped) {
        setFw(`升级完成 ✓ 板子已经用新固件重启了。<br>` +
          `<span class="dim">版本 ${r.fw || '—'}；现在把电脑 WiFi 切回平时的网络就行。</span>`, 'ok');
        setStatus('升级完成：确认换到新固件了');
      } else {
        setFw('这次没写进去 ✗ —— 再点一次「开始升级」就好（板子还在接收状态）。' +
          (replied === false ? '<br>上一次的回复也不是成功，可以一起看「高级」里的日志。' : ''), 'bad');
        setStatus('没写进去，需要再来一次');
      }
    } catch (e) {
      logTo('#ota-log', '确认时出错：' + e + '\n', true);
      setFw('确认时出错：' + e + '　（看「高级」里的日志）', 'bad');
      setStatus('确认出错');
    }
    state.upgrading = false;
    $('#b-ota').disabled = false;
  }

  /* 「高级」里：原始 /status 读数（heap/psram 这类字段只在看得懂的地方出现） */
  const rawOut = $('#b-out');
  let timer = null;
  async function refreshRaw() {
    rawOut.textContent = '读取中…';
    const s = await api('/api/board-status?host=' + encodeURIComponent($('#b-host').value.trim() || '192.168.4.1'));
    if (!s || s._error) {
      rawOut.textContent = '读不到：' + ((s && s._error) || '空响应') + '\n' +
        '（板子进入接收状态了吗？电脑 WiFi 连上 206dash-… 了吗？）';
      return;
    }
    rawOut.textContent = JSON.stringify(s, null, 1);
    applyBoard({ online: true, role: s.role, fw: s.fw, raw: s, ms: 0 });
  }
  $('#b-refresh').onclick = refreshRaw;
  $('#b-auto').onchange = () => {
    if (timer) { clearInterval(timer); timer = null; }
    if ($('#b-auto').checked) timer = setInterval(refreshRaw, 5000);
  };

  $('#b-bins').onclick = refreshBins;
  refreshBins();
  $('#b-board').click();          // 开工先看一眼板子在不在
}

/* ---- ② 表盘：二级页签 + 两个编辑器 + 预览 --------------------------------*/
/* ---- 预览页内联（不再用 iframe）：取回 /preview/preview.html 再注入 ----
   ★ 两个必须处理的点（不处理必踩）:
     ① `innerHTML` 注入的 <script> **不会执行** ⇒ 要手动重建 <script> 元素；
     ② 那个页面住在 /preview/ 下，它的相对路径（frames/l_0001.bmp）搬进本页会指错
        ⇒ 注入前把相对字面量补成 /preview/… 前缀。
   ★ 另外把它的 CSS 作用域化到 #pv-host：它里面有 body/h2/figure 这种通用选择器，
      不隔离会把整个外壳的样式带跑。 */
function initPreviewInline() {
  const host = $('#pv-host');
  if (!host) return;
  async function load() {
    host.textContent = '载入预览页…';
    try {
      const html = await (await fetch('/preview/preview.html', { cache: 'no-store' })).text();
      const doc = new DOMParser().parseFromString(html, 'text/html');
      const css = [...doc.querySelectorAll('style')].map((s) => s.textContent).join('\n');
      const scoped = css.replace(/(^|\})\s*([^{}@]+)\{/g, (m, close, sel) => {
        const s = sel.split(',').map((x) => '#pv-host ' + x.trim()).join(', ');
        return close + '\n' + s + '{';
      });
      doc.querySelectorAll('.navstrip').forEach((n) => n.remove());   // 单独打开用的导航条，外壳里不要
      const scripts = [...doc.querySelectorAll('script')];
      scripts.forEach((s) => s.remove());
      host.innerHTML = '<style>' + scoped + '</style>' + doc.body.innerHTML;
      for (const s of scripts) {
        const el = document.createElement('script');
        if (s.getAttribute('src')) {
          const src = s.getAttribute('src');
          el.src = src.charAt(0) === '/' ? src : '/preview/' + src;
        } else {
          el.textContent = (s.textContent || '')
            .replace(/(['"])(frames\/)/g, '$1/preview/$2')
            .replace(/(['"])(preview-28c\.html|preview\.html)/g, '$1/preview/$2');
        }
        host.appendChild(el);
      }
    } catch (e) { host.textContent = '预览页载入失败：' + e; }
  }
  window.__pvReload = load;   // 给外壳里"跑完预览后刷新"那条接线用（原来直接改 iframe.src）
  const b = $('#b-pv-reload'); if (b) b.onclick = load;
  load();
}
function initDash() {
  document.querySelectorAll('#subnav button').forEach((b) => {
    b.onclick = () => subGo(b.dataset.sub);
  });
  subGo('theme');
  $('#b-theme-open').onclick = () => window.open('/tools/theme-editor/index.html', '_blank');
  $('#b-image-open').onclick = () => window.open('/tools/theme-editor/image-editor.html', '_blank');
  const runPreview = async () => {
    logTo('#preview-log', '跑一遍电脑上的预览…（约 40 秒，正常会自己停）\n', false);
    setStatus('预览运行中…');
    const r = await api('/api/preview', {});
    logTo('#preview-log', (r.out || '') + `\n落帧 ${r.frames} 张\n`, false);
    setStatus(r.ok ? '预览完成' : '预览没出帧');
    if (window.__pvReload) window.__pvReload();   // 内联版：重取预览页
  };
  $('#b-preview').onclick = runPreview;
  $('#b-preview-open').onclick = () => window.open('/preview/preview.html', '_blank');
}

/* ---- ③ 工具：USB 写素材 + 构建 -------------------------------------------*/
function initFiles() {
  $('#f-bin').onchange = async (e) => {
    const f = e.target.files[0];
    if (!f) return;
    $('#f-info').textContent = `${f.name} · ${human(f.size)}`;
    const buf = new Uint8Array(await f.slice(0, 64).arrayBuffer());
    const hex = [...buf].map((b) => b.toString(16).padStart(2, '0')).join(' ');
    logTo('#f-head', `${f.name}\n大小 ${human(f.size)}\n头 64 字节:\n${hex}\n`);
  };
  $('#b-ports').onclick = async () => {
    setStatus('读串口…');
    const r = await api('/api/ports');
    const ps = r.ports || [];
    $('#f-port').innerHTML = ps.map((p) => `<option value="${p.port}">${p.port} ${p.detail || ''}</option>`).join('')
      || '<option value="">（没发现串口）</option>';
    logTo('#flash-log', ps.length ? '串口：' + ps.map((p) => p.port).join(', ') + '\n'
      : '没发现串口（板子插好了吗？）\n' + (r.raw || ''), false);
    setStatus('就绪');
  };
  $('#b-flash').onclick = async () => {
    const file = $('#f-bin').files[0];
    if (!file) { alert('先在上面「选一个文件」里选要写的文件'); return; }
    const port = $('#f-port').value;
    if (!port) { alert('先刷新串口并选一个'); return; }
    const kind = $('#f-kind').value;
    setStatus('上传文件到本地服务…');
    // 页面拿不到文件的真实路径 ⇒ 先传给本地服务落盘（scratch/app-upload），再交给 esptool
    const up = await fetch('/api/upload', {
      method: 'POST',
      headers: { 'Content-Type': 'application/octet-stream', 'X-Filename': file.name },
      body: file,
    }).then((r) => r.json());
    if (!up.ok) { logTo('#flash-log', '上传失败：' + JSON.stringify(up) + '\n', false); return; }
    logTo('#flash-log', `文件已落到 ${up.path}（${human(up.bytes)}）\n写进板子中…\n`, false);
    setStatus('写进板子中…');
    const r = await api('/api/flash', { port, kind, path: up.path });
    logTo('#flash-log', r.out || '', true);
    setStatus(r.ok ? '写完了 ✓（细节见下面的日志）' : '写失败（看下面的日志）');
  };
}
function initBuild() {
  $('#b-build').onclick = async () => {
    const env = $('#b-env').value;
    logTo('#build-log', `$ platformio run -e ${env}\n（第一次可能要几分钟）\n`, false);
    setStatus('构建中…');
    const r = await api('/api/build', { env });
    logTo('#build-log', r.out || '', false);
    setStatus(r.ok ? '构建成功' : '构建失败');
    refreshBins();
  };
}

/* ---- 退出 ---------------------------------------------------------------*/
function initQuit() {
  $('#b-quit').onclick = async () => {
    if (!confirm('退出 206Dash 表盘助手？（本地服务会停掉，页面随后失效）')) return;
    try { await api('/api/quit', {}); } catch (e) { /* 服务已经把连接断了，正常 */ }
    document.body.innerHTML =
      '<div style="padding:40px;font:16px system-ui;color:#eef1f7">已退出。' +
      '想再用就重新双击那个 exe（或 tools\\app\\run.bat）。</div>';
  };
}

/* ---- 启动 ---------------------------------------------------------------*/
(async function init() {
  /* 每个功能单独 try：一个挂了别的还能用（整段挂掉的表现是"点了没反应"） */
  const boot = (name, fn) => {
    try { fn(); } catch (e) { console.error('[206Dash] ' + name + ' 起不来:', e); }
  };
  boot('nav', () => { buildNav(); go((location.hash || '#flash').slice(1)); });
  window.addEventListener('hashchange', () => go((location.hash || '#flash').slice(1)));
  await initServer();
  boot('ota', () => initFlash());
  boot('dash', () => initDash());
  boot('preview', () => initPreviewInline());
  boot('files', () => initFiles());
  boot('build', () => initBuild());
  boot('quit', () => initQuit());
})();
