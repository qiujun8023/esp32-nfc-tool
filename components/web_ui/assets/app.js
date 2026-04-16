'use strict';

// NFC 操作状态机
const NfcState = {
  IDLE: 'idle',
  SCANNING: 'scanning',
  READING: 'reading',
  WRITING: 'writing',
  CLONING: 'cloning',
  NDEF_READ: 'ndef_read',
  NDEF_WRITE: 'ndef_write',
};

// 全局状态
const state = {
  tab: 'scan',
  nfc: NfcState.IDLE,  // 状态机
  status: null,
  scanResult: null,
  taskId: null,
  ws: null,
  log: [],
  sectors: [],
  total: 16,
};

function isBusy() { return state.nfc !== NfcState.IDLE; }

function setNfcState(s) {
  state.nfc = s;
  // 更新所有操作按钮的可用性
  document.querySelectorAll('[data-nfc-btn]').forEach(b => {
    b.disabled = s !== NfcState.IDLE;
  });
}

const $ = (s) => document.querySelector(s);
const view = $('#view');
const toast = $('#toast');

// ---- 工具 ----
function showToast(msg, kind) {
  toast.textContent = msg;
  toast.className = 'toast ' + (kind || '');
  setTimeout(() => toast.classList.add('hidden'), 2400);
  toast.classList.remove('hidden');
}

async function api(path, opts) {
  opts = opts || {};
  if (opts.body && typeof opts.body !== 'string') {
    opts.body = JSON.stringify(opts.body);
    opts.headers = Object.assign({ 'Content-Type': 'application/json' }, opts.headers || {});
  }
  const r = await fetch(path, opts);
  const ct = r.headers.get('content-type') || '';
  if (ct.indexOf('application/json') >= 0) return r.json();
  return r.text();
}

function fmtBytes(n) {
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1024 / 1024).toFixed(2) + ' MB';
}

function logLine(msg, kind) {
  state.log.unshift({ msg, kind: kind || '' });
  if (state.log.length > 50) state.log.pop();
  const el = document.querySelector('.log');
  if (el) renderLog(el);
}

function renderLog(el) {
  el.innerHTML = state.log.slice(0, 8).map(l =>
    `<div class="${l.kind}">${l.msg}</div>`).join('');
}

// ---- WebSocket ----
function wsConnect() {
  if (state.ws) return;
  const ws = new WebSocket(`ws://${location.host}/ws`);
  state.ws = ws;
  ws.onopen = () => { logLine('[ws] 已连接', 'ok'); };
  ws.onmessage = (ev) => {
    try {
      const m = JSON.parse(ev.data);
      handleWsMsg(m);
    } catch (e) {}
  };
  ws.onclose = () => {
    state.ws = null;
    logLine('[ws] 断开，2s 后重连', 'err');
    setTimeout(wsConnect, 2000);
  };
  ws.onerror = () => { try { ws.close(); } catch (e) {} };
}

function handleWsMsg(m) {
  if (m.type === 'progress') {
    state.sectors[m.sector] = { a: m.keyA, b: m.keyB, done: true };
    state.total = m.total;
    updateProgress();
    if (m.keyA || m.keyB) {
      logLine(`[扇区 ${m.sector}] ${m.keyA ? 'KeyA✓' : ''} ${m.keyB ? 'KeyB✓' : ''}`, 'ok');
    } else {
      logLine(`[扇区 ${m.sector}] 未找到密钥`, 'err');
    }
  } else if (m.type === 'done') {
    const r = m.result || {};
    if (r.verifyFails !== undefined) {
      if (r.verifyFails === 0) {
        logLine('[完成] 写入验证通过', 'ok');
        showToast('写入完成，验证通过', 'ok');
      } else {
        logLine(`[完成] 验证失败 ${r.verifyFails} 个 block`, 'err');
        showToast(`写入完成，${r.verifyFails} 个 block 验证失败`, 'error');
      }
    } else {
      logLine('[完成] ' + JSON.stringify(r), 'ok');
      showToast('操作完成', 'ok');
    }
    setNfcState(NfcState.IDLE);
    if (state.tab === 'library') renderLibrary();
    setTimeout(() => $('#progressWrap')?.classList.remove('show'), 800);
  } else if (m.type === 'error') {
    logLine('[错误] ' + m.msg, 'err');
    showToast(m.msg || '操作失败', 'error');
    setNfcState(NfcState.IDLE);
  }
}

function updateProgress() {
  let done = 0;
  for (let i = 0; i < state.total; i++) if (state.sectors[i]?.done) done++;
  const pct = state.total > 0 ? Math.round(done * 100 / state.total) : 0;
  const fill = $('.bar-fill');
  if (fill) fill.style.width = pct + '%';
  const grid = $('.sector-grid');
  if (grid) {
    let html = '';
    for (let i = 0; i < state.total; i++) {
      const s = state.sectors[i];
      let cls = 'sector';
      if (s) {
        if (s.a && s.b) cls += ' both';
        else if (s.a) cls += ' a-found';
        else if (s.b) cls += ' b-found';
        else cls += ' fail';
      }
      html += `<div class="${cls}">${i}</div>`;
    }
    grid.innerHTML = html;
  }
}

// ---- 顶部状态 ----
async function refreshStatus() {
  try {
    const s = await api('/api/status');
    state.status = s;
    const fw = s.pn532Fw ? '0x' + s.pn532Fw.toString(16).padStart(8, '0') : '未连接';
    $('#meta').textContent = `PN532 ${fw} · ${fmtBytes(s.fsTotal - s.fsUsed)}空闲`;
  } catch (e) {}
}

// ---- 扫描页 ----
function renderScan() {
  view.innerHTML = `
    <h2 class="page-title">扫描卡片</h2>
    <p class="page-sub">把卡片贴近 PN532 模块</p>
    <div class="scan-area">
      <button class="scan-btn" id="scanBtn">读卡</button>
      <div id="scanResult" class="hidden" style="width:100%;">
        <div class="uid-display" id="uidDisp"></div>
        <div style="text-align:center;"><span class="card-badge" id="cardType"></span></div>
        <div class="actions">
          <button class="btn primary" id="cloneBtn">克隆 UID</button>
          <button class="btn" id="dumpBtn">完整 dump</button>
        </div>
        <div id="ndefArea" class="hidden">
          <h3 style="font-size:14px;margin:16px 0 8px;">NDEF 操作</h3>
          <div class="actions">
            <button class="btn" id="ndefReadBtn">读取 NDEF</button>
          </div>
          <div id="ndefResult" class="hidden" style="margin-top:8px;"></div>
          <div style="margin-top:12px;">
            <select id="ndefType" class="input" style="height:36px;margin-bottom:8px;">
              <option value="uri">URL</option>
              <option value="text">文本</option>
            </select>
            <input class="input" id="ndefPayload" placeholder="https://example.com" style="margin-bottom:8px;">
            <div id="ndefLangRow" class="hidden" style="margin-bottom:8px;">
              <input class="input" id="ndefLang" placeholder="语言代码 (如 en, zh)" value="en" style="width:120px;">
            </div>
            <button class="btn primary" id="ndefWriteBtn" style="width:100%;">写入 NDEF</button>
          </div>
        </div>
        <div id="progressWrap" class="progress-wrap">
          <div class="bar"><div class="bar-fill"></div></div>
          <div class="sector-grid"></div>
          <div class="log"></div>
        </div>
      </div>
    </div>
  `;
  $('#scanBtn').onclick = doScan;
}

async function doScan() {
  if (isBusy()) { showToast('另一项操作进行中', 'error'); return; }
  setNfcState(NfcState.SCANNING);
  const btn = $('#scanBtn');
  btn.classList.add('scanning');
  btn.textContent = '扫描中…';
  try {
    const r = await api('/api/scan', { method: 'POST' });
    btn.classList.remove('scanning');
    setNfcState(NfcState.IDLE);
    btn.textContent = '读卡';
    if (!r.ok) {
      showToast(r.err || '未发现卡片', 'error');
      $('#scanResult').classList.add('hidden');
      return;
    }
    btn.classList.add('found');
    state.scanResult = r;
    $('#uidDisp').textContent = r.uid.match(/.{1,2}/g).join(' ');
    $('#cardType').textContent = r.typeName + (r.magic ? ' [' + r.magic + ']' : '');
    $('#scanResult').classList.remove('hidden');
    setTimeout(() => btn.classList.remove('found'), 1200);

    $('#cloneBtn').onclick = () => doClone(r.uid);
    $('#dumpBtn').onclick = doDump;

    // NDEF 区域仅对 NTAG/Ultralight 显示
    if (r.type === 3 || r.typeName.indexOf('NTAG') >= 0 || r.typeName.indexOf('Ultralight') >= 0) {
      $('#ndefArea').classList.remove('hidden');
      $('#ndefReadBtn').onclick = doNdefRead;
      $('#ndefWriteBtn').onclick = doNdefWrite;
      $('#ndefType').onchange = () => {
        const isText = $('#ndefType').value === 'text';
        $('#ndefLangRow').classList.toggle('hidden', !isText);
        $('#ndefPayload').placeholder = isText ? '输入文本内容' : 'https://example.com';
      };
    } else {
      $('#ndefArea').classList.add('hidden');
    }
  } catch (e) {
    btn.classList.remove('scanning');
    setNfcState(NfcState.IDLE);
    btn.textContent = '读卡';
    showToast('请求失败', 'error');
  }
}

async function doClone(uid) {
  if (isBusy()) { showToast('另一项操作进行中', 'error'); return; }
  if (!confirm(`确认把 UID ${uid} 写入当前贴近的魔术卡？`)) return;
  setNfcState(NfcState.CLONING);
  const r = await api('/api/clone-uid', { method: 'POST', body: { uid } });
  setNfcState(NfcState.IDLE);
  if (r.ok) {
    showToast(r.verified ? '克隆成功，验证通过' : '克隆完成，验证未通过', r.verified ? 'ok' : 'error');
  } else {
    showToast(r.err || '克隆失败', 'error');
  }
}

async function doDump() {
  if (isBusy()) { showToast('另一项操作进行中', 'error'); return; }
  state.sectors = [];
  state.total = state.scanResult.type === 2 ? 40 : 16;
  state.log = [];
  $('#progressWrap').classList.add('show');
  updateProgress();
  const name = prompt('给这张卡起个名字（可空）') || '';
  setNfcState(NfcState.READING);
  const r = await api('/api/read', { method: 'POST', body: { name } });
  if (!r.ok) {
    setNfcState(NfcState.IDLE);
    showToast(r.err || '启动失败', 'error');
    return;
  }
  state.taskId = r.taskId;
  logLine('[开始] 字典攻击中…');
  // 状态由 WS done/error 回调恢复为 IDLE
}

async function doNdefRead() {
  if (isBusy()) { showToast('另一项操作进行中', 'error'); return; }
  setNfcState(NfcState.NDEF_READ);
  const r = await api('/api/ndef/read', { method: 'POST' });
  setNfcState(NfcState.IDLE);
  const el = $('#ndefResult');
  el.classList.remove('hidden');
  if (r.ok) {
    el.innerHTML = `<div class="dump-card" style="padding:8px;">
      <span class="card-badge">${escapeHtml(r.type.toUpperCase())}</span>
      <p style="margin:8px 0 0;font-family:var(--font-mono);font-size:13px;word-break:break-all;">${escapeHtml(r.payload)}</p>
      ${r.lang ? '<p style="margin:4px 0 0;font-size:11px;color:var(--text-dim);">lang: ' + escapeHtml(r.lang) + '</p>' : ''}
    </div>`;
  } else {
    el.innerHTML = `<div style="color:var(--error);font-size:13px;">${escapeHtml(r.err || '读取失败')}</div>`;
  }
}

async function doNdefWrite() {
  if (isBusy()) { showToast('另一项操作进行中', 'error'); return; }
  const type = $('#ndefType').value;
  const payload = $('#ndefPayload').value.trim();
  if (!payload) { showToast('请输入内容', 'error'); return; }
  setNfcState(NfcState.NDEF_WRITE);
  const body = { type, payload };
  if (type === 'text') body.lang = ($('#ndefLang').value.trim() || 'en');
  const r = await api('/api/ndef/write', { method: 'POST', body });
  setNfcState(NfcState.IDLE);
  showToast(r.ok ? 'NDEF 写入成功' : (r.err || 'NDEF 写入失败'), r.ok ? 'ok' : 'error');
}

// ---- 卡库 ----
async function renderLibrary() {
  view.innerHTML = `
    <h2 class="page-title">卡库</h2>
    <div style="margin-bottom:12px;">
      <button class="btn primary" id="uploadBtn" style="width:100%;">导入 dump 文件 (.bin / .mfd)</button>
      <input type="file" id="uploadFile" accept=".bin,.mfd" class="hidden">
    </div>
    <div id="dumpList" class="dump-list"></div>
  `;
  $('#uploadBtn').onclick = () => $('#uploadFile').click();
  $('#uploadFile').onchange = async (ev) => {
    const file = ev.target.files[0];
    if (!file) return;
    const name = prompt('给这张卡起个名字', file.name.replace(/\.\w+$/, '')) || '导入';
    const type = file.size <= 924 ? 'ntag' : 'mfc';
    const buf = await file.arrayBuffer();
    const r = await fetch('/api/dumps/upload?name=' + encodeURIComponent(name) + '&type=' + type, {
      method: 'POST', body: buf, headers: { 'Content-Type': 'application/octet-stream' }
    }).then(r => r.json());
    if (r.ok) { showToast('导入成功', 'ok'); renderLibrary(); }
    else showToast(r.err || '导入失败', 'error');
    ev.target.value = '';
  };
  const dl = $('#dumpList');
  const list = await api('/api/dumps');
  if (!list.length) {
    dl.innerHTML = '<div class="empty">空空如也\n\n去「扫描」页\n读一张卡吧</div>';
    return;
  }
  list.sort((a, b) => b.seq - a.seq);
  dl.innerHTML = list.map(d => `
    <div class="dump-card">
      <div class="dump-head">
        <div>
          <p class="dump-name">${escapeHtml(d.name)}</p>
          <div class="dump-uid">${d.uid.match(/.{1,2}/g).join(' ')}</div>
        </div>
        <span class="card-badge">${d.type === 1 ? 'MFC' : 'NTAG'}</span>
      </div>
      <div class="dump-meta">
        <span>${d.blocks} 块</span>
        <span>${d.knownKeys} 密钥</span>
        <span>${fmtBytes(d.binSize)}</span>
        <span>#${d.seq || '-'}</span>
      </div>
      <div class="dump-actions">
        <button class="btn" data-act="rename" data-id="${d.id}">改名</button>
        ${d.type === 1 ? '<button class="btn" data-act="detail" data-id="' + d.id + '">权限</button>' : ''}
        <button class="btn" data-act="dl-bin" data-id="${d.id}">.bin</button>
        <button class="btn" data-act="dl-txt" data-id="${d.id}">.txt</button>
        <button class="btn primary" data-act="write" data-id="${d.id}">写回</button>
        <button class="btn danger" data-act="del" data-id="${d.id}">删</button>
      </div>
      <div class="detail-area hidden" data-detail-id="${d.id}"></div>
    </div>
  `).join('');

  dl.onclick = async (ev) => {
    const b = ev.target.closest('button[data-act]');
    if (!b) return;
    const id = b.dataset.id;
    const act = b.dataset.act;
    if (act === 'dl-bin') {
      window.location.href = `/api/dumps/${id}/bin`;
    } else if (act === 'dl-txt') {
      window.location.href = `/api/dumps/${id}/txt`;
    } else if (act === 'del') {
      if (!confirm('确认删除？')) return;
      await api(`/api/dumps/${id}`, { method: 'DELETE' });
      renderLibrary();
    } else if (act === 'rename') {
      const name = prompt('新名字');
      if (!name) return;
      await api(`/api/dumps/${id}`, { method: 'PATCH', body: { name } });
      renderLibrary();
    } else if (act === 'detail') {
      const area = document.querySelector(`.detail-area[data-detail-id="${id}"]`);
      if (area.classList.contains('hidden')) {
        const r = await api(`/api/dumps/${id}/detail`);
        if (r.ok) {
          area.innerHTML = r.sectors.map(s => `
            <div class="access-sector">
              <div class="access-header">Sector ${s.sector} ${s.keyA ? '| A:' + s.keyA : ''} ${s.keyB ? '| B:' + s.keyB : ''}</div>
              ${s.blocks.map(b => `<div class="access-block ${b.role}">
                <span>Block ${b.block}</span>
                <span class="access-badge">${b.desc}</span>
              </div>`).join('')}
            </div>`).join('');
          area.classList.remove('hidden');
        } else {
          showToast(r.err || '获取详情失败', 'error');
        }
      } else {
        area.classList.add('hidden');
      }
    } else if (act === 'write') {
      if (isBusy()) { showToast('另一项操作进行中', 'error'); return; }
      if (!confirm('把 dump 写回当前贴近的卡？需对应密钥已知')) return;
      setNfcState(NfcState.WRITING);
      const r = await api('/api/write', { method: 'POST', body: { dumpId: id } });
      if (!r.ok) { setNfcState(NfcState.IDLE); }
      showToast(r.ok ? '已开始写回，看进度日志' : (r.err || '失败'), r.ok ? 'ok' : 'error');
      // 状态由 WS done/error 回调恢复为 IDLE
    }
  };
}

function escapeHtml(s) {
  return String(s).replace(/[&<>"']/g, c => ({
    '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;'
  }[c]));
}

// ---- 密钥 ----
async function renderKeys() {
  view.innerHTML = `
    <h2 class="page-title">密钥库</h2>
    <div class="key-section">
      <p class="page-sub">默认密钥（只读）</p>
      <div class="key-list" id="defaultKeys"></div>
    </div>
    <div class="key-section">
      <p class="page-sub">自定义密钥</p>
      <div class="key-list" id="userKeys"></div>
      <div class="input-row">
        <input class="input" id="keyInput" placeholder="12 位 hex，如 FFFFFFFFFFFF" maxlength="12">
        <button class="btn primary" id="addKey">添加</button>
      </div>
    </div>
  `;
  const data = await api('/api/keys');
  $('#defaultKeys').innerHTML = data.defaults.map(k =>
    `<div class="key-item">${k}</div>`).join('');
  $('#userKeys').innerHTML = data.user.length
    ? data.user.map((k, i) => `<div class="key-item">${k}<button class="x" data-i="${i}">×</button></div>`).join('')
    : '<div class="empty" style="grid-column:1/-1;padding:24px;">尚无自定义密钥</div>';

  $('#userKeys').onclick = async (ev) => {
    const x = ev.target.closest('.x');
    if (!x) return;
    await api(`/api/keys/${x.dataset.i}`, { method: 'DELETE' });
    renderKeys();
  };
  $('#addKey').onclick = async () => {
    const v = $('#keyInput').value.trim().toUpperCase();
    if (!/^[0-9A-F]{12}$/.test(v)) {
      showToast('需要 12 位 hex', 'error');
      return;
    }
    const r = await api('/api/keys', { method: 'POST', body: { key: v } });
    if (r.ok) renderKeys();
    else showToast(r.err || '失败', 'error');
  };
}

// ---- 设置 ----
function renderSettings() {
  const s = state.status || {};
  view.innerHTML = `
    <h2 class="page-title">设置</h2>
    <div class="setting-row"><span class="k">固件版本</span><span class="v">${s.version || '-'}</span></div>
    <div class="setting-row"><span class="k">PN532</span><span class="v">${s.pn532Fw ? '0x' + s.pn532Fw.toString(16).padStart(8,'0') : '未连接'}</span></div>
    <div class="setting-row"><span class="k">运行时间</span><span class="v">${s.uptime || 0} 秒</span></div>
    <div class="setting-row"><span class="k">空闲堆</span><span class="v">${fmtBytes(s.freeHeap || 0)}</span></div>
    <div class="setting-row"><span class="k">存储</span><span class="v">${fmtBytes(s.fsUsed||0)} / ${fmtBytes(s.fsTotal||0)}</span></div>
    <div class="setting-row"><span class="k">AP SSID</span><span class="v">esp32-nfc-tool</span></div>
    <div class="setting-row"><span class="k">AP 地址</span><span class="v">192.168.4.1</span></div>
    <div style="margin-top:24px;">
      <p class="page-sub">固件更新 (OTA)</p>
      <p style="font-size:12px;color:var(--text-dim);margin-bottom:8px;">选择编译好的 .bin 固件文件上传，更新后设备自动重启</p>
      <input type="file" id="otaFile" accept=".bin" class="hidden">
      <button class="btn primary" id="otaBtn" style="width:100%;">选择固件文件</button>
      <div id="otaProgress" class="hidden" style="margin-top:8px;">
        <div class="bar"><div class="bar-fill" id="otaBar"></div></div>
        <p id="otaStatus" style="font-size:12px;color:var(--text-dim);margin-top:4px;text-align:center;"></p>
      </div>
    </div>
    <div style="margin-top:24px;">
      <p class="page-sub">硬件信息</p>
      <p style="font-size:12px;color:var(--text-dim);">ESP32-C3 + PN532 · SPI(SCK=4 MISO=5 MOSI=6 SS=7)</p>
    </div>
  `;

  $('#otaBtn').onclick = () => $('#otaFile').click();
  $('#otaFile').onchange = async (ev) => {
    const file = ev.target.files[0];
    if (!file) return;
    if (!confirm('确认上传固件 ' + file.name + '？更新后设备将重启')) return;

    const btn = $('#otaBtn');
    btn.disabled = true;
    btn.textContent = '上传中…';
    $('#otaProgress').classList.remove('hidden');
    $('#otaStatus').textContent = '正在上传固件…';
    $('#otaBar').style.width = '0%';

    try {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/ota');
      xhr.setRequestHeader('Content-Type', 'application/octet-stream');
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
          const pct = Math.round(e.loaded * 100 / e.total);
          $('#otaBar').style.width = pct + '%';
          $('#otaStatus').textContent = '上传中 ' + pct + '%';
        }
      };
      xhr.onload = () => {
        try {
          const r = JSON.parse(xhr.responseText);
          if (r.ok) {
            $('#otaStatus').textContent = '更新完成，设备重启中…';
            $('#otaBar').style.width = '100%';
            showToast('固件更新成功，设备即将重启', 'ok');
          } else {
            $('#otaStatus').textContent = r.err || '更新失败';
            showToast(r.err || '更新失败', 'error');
            btn.disabled = false;
            btn.textContent = '选择固件文件';
          }
        } catch (e) {
          $('#otaStatus').textContent = '响应解析失败';
          btn.disabled = false;
          btn.textContent = '选择固件文件';
        }
      };
      xhr.onerror = () => {
        $('#otaStatus').textContent = '上传失败';
        showToast('上传失败', 'error');
        btn.disabled = false;
        btn.textContent = '选择固件文件';
      };
      xhr.send(file);
    } catch (e) {
      showToast('上传失败', 'error');
      btn.disabled = false;
      btn.textContent = '选择固件文件';
    }
    ev.target.value = '';
  };
}

// ---- 路由 ----
function setTab(name) {
  state.tab = name;
  document.querySelectorAll('.tab').forEach(t => {
    t.classList.toggle('active', t.dataset.tab === name);
  });
  if (name === 'scan') renderScan();
  else if (name === 'library') renderLibrary();
  else if (name === 'keys') renderKeys();
  else if (name === 'settings') renderSettings();
}

document.querySelectorAll('.tab').forEach(t => {
  t.onclick = () => setTab(t.dataset.tab);
});

// ---- 启动 ----
refreshStatus();
setInterval(refreshStatus, 5000);
wsConnect();
setTab('scan');
