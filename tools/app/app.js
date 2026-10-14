/* ============================================================================
 * 206Dash 开发台 —— 应用外壳
 *
 * 结构（要加功能就改这一处）：
 *   TABS   —— 顶栏那一排页签：{id, 标题, 渲染函数(可选)}。加一个页签 = 加一条，
 *             HTML 里放一个 <section id="tab-<id>">，需要的话在 render 里做初始化。
 *   其余   —— 每个功能一个 init 函数，最后统一 init() 里挂上。
 * ==========================================================================*/
'use strict';

const TABS = [
  { id: 'home',    title: '总览' },
  { id: 'ota',     title: '无线刷写' },
  { id: 'board',   title: '开发板状态' },
  { id: 'theme',   title: '主题配色' },
  { id: 'image',   title: '图片素材' },
  { id: 'preview', title: '预览表盘' },
  { id: 'files',   title: '文件与烧写' },
];

const $ = (s) => document.querySelector(s);
const setStatus = (s) => { $('#f-status').textContent = s; };

/* ---- 页签 ----------------------------------------------------------------*/
function buildNav() {
  const nav = $('#nav');
  nav.innerHTML = '';
  TABS.forEach((t) => {
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
  document.querySelectorAll('#nav button').forEach((b) => b.classList.toggle('sel', b.dataset.tab === id));
  document.querySelectorAll('main section').forEach((s) => s.classList.toggle('sel', s.id === 'tab-' + id));
  location.hash = id;
}

/* ---- 小工具 --------------------------------------------------------------*/
function logTo(sel, text, append) {
  const el = $(sel);
  el.textContent = append ? (el.textContent + text) : text;
  el.scrollTop = el.scrollHeight;
}
function kv(sel, pairs) {
  $(sel).innerHTML = pairs.map(([k, v]) => `<b>${k}</b><span>${v}</span>`).join('');
}
function human(n) {
  if (n == null) return '—';
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1024 / 1024).toFixed(2) + ' MB';
}
async function api(path, body) {
  const opt = body
    ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) }
    : {};
  const r = await fetch(path, opt);
  return r.json();
}

/* ---- 总览：服务 / 固件列表 ----------------------------------------------*/
async function initServer() {
  try {
    const h = await api('/api/health');
    $('#p-srv').textContent = '服务：' + (h.ok ? '在线' : '异常') + (h.frozen ? '（打包版）' : '');
    $('#p-srv').className = 'pill ' + (h.ok ? 'on' : 'off');
    $('#p-repo').textContent = h.repo + (h.frozen ? '  ·  exe: ' + h.exe : '');
    $('#p-repo').title = '本地服务只监听 127.0.0.1；只服务这个仓库目录';
    if (!h.repo_ok) {
      $('#f-status').innerHTML = '<span class="bad">没找到仓库</span>：' +
        '构建/烧写/编辑器用不了 —— 用 <code>--repo &lt;仓库路径&gt;</code> 启动';
    }
  } catch (e) {
    $('#p-srv').textContent = '服务：离线（用 tools/app/run.bat 或那个 exe 启动）';
    $('#p-srv').className = 'pill off';
  }
}

async function refreshBins() {
  const r = await api('/api/bins');
  const bins = r.bins || [];
  kv('#bins-kv', bins.length
    ? bins.map((b) => [b.env, `${human(b.bytes)} · ${b.mtime}${b.ota_ok ? ' · <span class="ok">可无线刷</span>' : ''}`])
    : [['（空）', '还没有构建产物；去「文件与烧写」构建一次']]);
  const sel = $('#ota-bin');
  sel.innerHTML = bins.map((b, i) =>
    `<option value="${i}">${b.env} — ${human(b.bytes)}${b.ota_ok ? ' [OTA]' : ''}</option>`).join('');
  sel.dataset.bins = JSON.stringify(bins);
  const envSel = $('#b-env');
  if (envSel.options.length === 0) {
    envSel.innerHTML = ['esp32s3-rgb-slave-ota', 'esp32s3-rgb-master-ota',
      'esp32s3-rgb-slave-obdtest', 'esp32s3-rgb-slave-now', 'esp32s3-rgb-master-now', 'pcpreview']
      .map((e) => `<option>${e}</option>`).join('');
  }
}

/* ---- 开发板状态（板子自己的 /status，需要固件的 CORS 头） ----------------*/
async function boardStatus(host) {
  const t0 = performance.now();
  const ctl = new AbortController();
  const timer = setTimeout(() => ctl.abort(), 4000);
  try {
    const r = await fetch(`http://${host}/status`, { signal: ctl.signal });
    clearTimeout(timer);
    if (!r.ok) throw new Error('HTTP ' + r.status);
    const j = await r.json();
    const ms = Math.round(performance.now() - t0);
    $('#p-board').textContent = '开发板：已连接';
    $('#p-board').className = 'pill on';
    kv('#board-kv', Object.entries(j).map(([k, v]) => [k, String(v)]));
    $('#board-hint').textContent = `通了（${ms}ms）`;
    return true;
  } catch (e) {
    clearTimeout(timer);
    $('#p-board').textContent = '开发板：未连接';
    $('#p-board').className = 'pill off';
    $('#board-hint').innerHTML =
      '连不上：先确认板子进了 OTA 模式、电脑 WiFi 已切到 <code>206dash-…</code> 热点';
    kv('#board-kv', [['错误', String(e.message || e)]]);
    return false;
  }
}

/* ---- 无线刷写（浏览器直传板子的 /update） -------------------------------*/
function initOta() {
  $('#ota-src').onchange = () => {
    const useFile = $('#ota-src').value === 'file';
    $('#ota-filewrap').style.display = useFile ? '' : 'none';
    $('#ota-bin').style.display = useFile ? 'none' : '';
  };
  $('#b-ota-check').onclick = () => boardStatus($('#ota-host').value.trim() || '192.168.4.1');

  $('#b-ota').onclick = () => {
    const host = $('#ota-host').value.trim() || '192.168.4.1';
    const useFile = $('#ota-src').value === 'file';
    let file = null, label = '';
    if (useFile) {
      file = $('#ota-file').files[0];
      label = file ? file.name : '';
      if (!file) { alert('先选一个 firmware.bin'); return; }
    } else {
      const bins = JSON.parse($('#ota-bin').dataset.bins || '[]');
      const b = bins[Number($('#ota-bin').value)];
      if (!b) { alert('列表里没有可烧的固件，先去构建'); return; }
      if (!b.ota_ok) {
        if (!confirm(`${b.env} 不是 -ota 档，烧上去之后就不能再用无线刷写了。继续？`)) return;
      }
      label = b.env + '/firmware.bin';
      // 由本地服务把文件吐出来（同源），产物在 scratch 镜像里也拿得到
      fetch('/api/bin?env=' + encodeURIComponent(b.env))
        .then((r) => r.blob())
        .then((blob) => upload(host, new File([blob], 'firmware.bin'), label))
        .catch((e) => logTo('#ota-log', '读文件失败：' + e + '\n', true));
      return;
    }
    upload(host, file, label);
  };

  // ---- 上传后自动验槽（2026-10-10）--------------------------------------------
  // 为什么必须有这一步：板子 Update 收完立刻重启 ⇒ HTTP 回执可能死在路上，浏览器
  // **无法**区分"写进去了但回复丢了"和"压根没写进去"。唯一可信的判据是板子自己的账：
  // 串口 `u` 重新进 OTA 模式，再读 /status —— running/target 翻转过来才算真的换了槽。
  // 这两件事由本地服务代做（浏览器打不到串口，也读不到重启后的板子），见 /api/ota-verify。
  async function verifySlot(host, replied) {
    const port = (($('#ota-port') && $('#ota-port').value.trim()) || 'COM8');
    logTo('#ota-log', replied
      ? '\n★ 自动验槽：读 /status 判槽位；**板子若还没进 OTA 模式才会发 u**（进 OTA 模式 = 屏幕黑着，最长 10 分钟）\n'
      : '\n★ 自动验槽（回执不可信，所以直接问板子）…\n', true);
    setStatus('自动验槽…');
    try {
      const before = await api('/api/board-status?host=' + encodeURIComponent(host));
      const r = await api('/api/ota-verify', {
        method: 'POST',
        body: JSON.stringify({ host, port, before: (before && before.running) || '' }),
      });
      if (!r.ok) {
        logTo('#ota-log', '✗ 验槽没结论：' + (r.error || '') + '\n' +
          '  （板子还在 OTA 模式吗？电脑连上热点了吗？串口 ' + port + ' 选对了吗？）\n', true);
        setStatus('验槽未完成');
        return;
      }
      const flip = r.flipped
        ? '★ 槽已轮换：' + r.before + ' → ' + r.running + ' ⇒ 这次真的写进去了 ✓'
        : '⚠ 槽没变（还是 ' + r.running + '）⇒ 这次多半没写进去';
      logTo('#ota-log', flip + '\n  现在 running=' + r.running + ' target=' + r.target +
        ' fw=' + (r.fw || '?') + '（验了 ' + (r.waited_ms / 1000).toFixed(1) + 's）\n' +
        '  ★ 板子现在停在 OTA 模式：不传固件它会在 10 分钟后自己退回正常固件。\n', true);
      setStatus(r.flipped ? '验槽通过：已换槽' : '验槽：槽没变');
    } catch (e) {
      logTo('#ota-log', '验槽出错：' + e + '\n', true);
      setStatus('验槽出错');
    }
  }
  function upload(host, file, label) {
    const url = `http://${host}/update`;
    logTo('#ota-log', `→ POST ${url}\n  文件 ${label} (${human(file.size)})\n`, false);
    setStatus('上传中…');
    const fd = new FormData();
    fd.append('firmware', file, 'firmware.bin');
    const xhr = new XMLHttpRequest();
    xhr.open('POST', url);
    xhr.upload.onprogress = (e) => {
      if (!e.lengthComputable) return;
      const p = Math.round((e.loaded / e.total) * 100);
      $('#ota-bar').style.width = p + '%';
      setStatus(`上传 ${p}%（${human(e.loaded)} / ${human(e.total)}）`);
    };
    xhr.onload = () => {
      logTo('#ota-log', `← ${xhr.status} ${xhr.responseText || ''}\n`, true);
      setStatus('上传结束');
      $('#ota-bar').style.width = '100%';
      if (xhr.status === 200 && /OK/.test(xhr.responseText || '')) {
        logTo('#ota-log', '★ 板子正在重启成新版。注意：收完就重启，回执丢了也不代表没写进去 —— 下面用板子自己的账验。\n', true);
      }
    };
    xhr.onerror = () => {
      logTo('#ota-log', '★ 请求失败 —— 但**这不等于没写进去**：板子收完固件会立刻重启，回复常常死在那一瞬。\n' +
        '  备份路径：连上热点后直接开 http://192.168.4.1/ 用板子自带的上传页。\n', true);
      setStatus('上传失败？先验槽');
    };
    xhr.send(fd);
  }
}

/* ---- 文件：读 + 烧 -------------------------------------------------------*/
/* ---- 开发板状态：读板子自己的 /status（服务端代读，绕开浏览器跨域/私网限制） ---- */
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
function initBoard() {
  const hostEl = $('#b-host'), out = $('#b-out'), autoEl = $('#b-auto');
  let timer = null;
  async function refresh() {
    out.textContent = '读取中…';
    try {
      const s = await api('/api/board-status?host=' + encodeURIComponent(hostEl.value.trim() || '192.168.4.1'));
      if (!s || s._error) {
        out.textContent = '读不到：' + ((s && s._error) || '空响应') + '\n' +
          '（板子进了 OTA 模式吗？电脑 WiFi 连上 206dash-… 热点了吗？）';
        return;
      }
      out.textContent =
        '固件        : ' + (s.fw || '?') + '\n' +
        '角色        : ' + (s.role || '?') + '\n' +
        '运行槽      : ' + (s.running || '?') + '     目标槽: ' + (s.target || '?') + '\n' +
        'OTA 就绪    : ' + (s.ota_ready ? '是' : '否') + '     OTA 模式: ' + (s.ota_mode ? '是' : '否') + '\n' +
        '内部 RAM    : ' + (s.heap_kb != null ? s.heap_kb + ' KB 可用' : '?') + '\n' +
        '已运行      : ' + Math.round((s.uptime_ms || 0) / 1000) + ' 秒\n' +
        '★ PSRAM/复位原因要固件报才有（/status 现在是这些字段）—— 需要我就加。';
    } catch (e) { out.textContent = '出错：' + e; }
  }
  $('#b-refresh').onclick = refresh;
  autoEl.onchange = () => {
    if (timer) { clearInterval(timer); timer = null; }
    if (autoEl.checked) timer = setInterval(refresh, 5000);
  };
  refresh();
}
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
    if (!file) { alert('先在左边「读一个 bin / json」里选文件'); return; }
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
    logTo('#flash-log', `文件已落到 ${up.path}（${human(up.bytes)}）\n烧写中…\n`, false);
    setStatus('烧写中…');
    const r = await api('/api/flash', { port, kind, path: up.path });
    logTo('#flash-log', r.out || '', true);
    setStatus(r.ok ? '烧写完成（看上面 Hash of data verified）' : '烧写失败');
  };
}

/* ---- 构建 / 预览 --------------------------------------------------------*/
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
  const runPreview = async () => {
    logTo('#preview-log', '跑 pcpreview…（约 40 秒，正常会自己停）\n', false);
    setStatus('预览运行中…');
    const r = await api('/api/preview', {});
    logTo('#preview-log', (r.out || '') + `\n落帧 ${r.frames} 张\n`, false);
    setStatus(r.ok ? '预览完成' : '预览没出帧');
    if (window.__pvReload) window.__pvReload();   // 内联版：重取预览页（等价于原来换 iframe.src + 缓存戳）
  };
  $('#b-preview').onclick = runPreview;
  $('#b-preview-open').onclick = () => window.open('/preview/preview.html', '_blank');
  $('#b-theme-open').onclick = () => window.open('/tools/theme-editor/index.html', '_blank');
  $('#b-image-open').onclick = () => window.open('/tools/theme-editor/image-editor.html', '_blank');
}

/* ---- 启动 ---------------------------------------------------------------*/
(async function init() {
  buildNav();
  go((location.hash || '#home').slice(1));
  window.addEventListener('hashchange', () => go((location.hash || '#home').slice(1)));
  await initServer();
  refreshBins();
  initOta();
  initFiles();
initBoard();
initPreviewInline();
  initBuild();
  $('#b-board').onclick = () => boardStatus($('#ota-host').value.trim() || '192.168.4.1');
  $('#b-bins').onclick = refreshBins;
  $('#b-quit').onclick = async () => {
    if (!confirm('退出 206Dash 开发台？（本地服务会停掉，页面随后失效）')) return;
    try { await api('/api/quit', {}); } catch (e) { /* 服务已经把连接断了，正常 */ }
    document.body.innerHTML =
      '<div style="padding:40px;font:16px system-ui;color:#e8eaf0">已退出。' +
      '想再用就重新双击那个 exe（或 tools\\app\\run.bat）。</div>';
  };
  $('#b-ports').click();
})();
