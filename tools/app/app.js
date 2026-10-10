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
        logTo('#ota-log', '★ 板子正在重启成新版；30 秒后可以点「检测开发板」看新版本号\n', true);
      }
    };
    xhr.onerror = () => {
      logTo('#ota-log', '★ 请求失败：多半是没连上板子的热点，或浏览器拦了跨域请求。\n' +
        '  备份路径：连上热点后直接开 http://192.168.4.1/ 用板子自带的上传页。\n', true);
      setStatus('上传失败');
    };
    xhr.send(fd);
  }
}

/* ---- 文件：读 + 烧 -------------------------------------------------------*/
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
    $('#fr-preview').src = '/preview/preview.html?t=' + Date.now();
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
