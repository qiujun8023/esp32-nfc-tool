'use strict';

/* ============================================================
   ESP32 NFC Tool 前端应用
   ============================================================ */

// ---- 状态 ----
const S = {
  tab: 'scan',
  busy: false,
  busyType: '',   // reading | writing | scanning | ndef_read | ndef_write
  card: null,     // last scan result
  status: null,   // device status
  ws: null,
  taskId: null,
  // 进度追踪
  sectors: [],
  total: 16,
  logs: [],
  // 卡库详情子页
  libDetailId: null,
  libDetailMeta: null,  // 当前选中卡的元数据（来自 list）
  libDetailData: null,  // detail API 返回值（懒加载）
  // 最近一次读取的摘要（方案 C：读完停留在摘要状态，用户主动点才跳详情/继续）
  lastRead: null,       // { id, kind, success, summary }
  lastCardUid: null,    // 上一张被监控任务识别的卡 UID，用于判断 card_in 是否是同一张卡
  // 写卡子页
  writeTarget: null,    // { dumpId, dumpName, total, verifyFails, error, canceled }
};

const $ = (sel, ctx) => (ctx || document).querySelector(sel);
const $$ = (sel, ctx) => (ctx || document).querySelectorAll(sel);
const app = $('#app');

// ---- 工具函数 ----
function esc(s) {
  const d = document.createElement('div');
  d.textContent = String(s);
  return d.innerHTML;
}

function fmtBytes(n) {
  if (n < 1024) return n + ' B';
  if (n < 1048576) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1048576).toFixed(1) + ' MB';
}

function fmtUid(uid) {
  if (!uid) return '';
  return uid.match(/.{1,2}/g)?.join(' ') || uid;
}

function fmtUptime(sec) {
  if (!sec) return '0s';
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  if (h) return h + 'h ' + m + 'm';
  if (m) return m + 'm ' + s + 's';
  return s + 's';
}

/* 基于浏览器当前时间的相对时间（设备无 RTC，保存时由浏览器打戳） */
function fmtRelTime(ms) {
  if (!ms) return '';
  const now = Date.now();
  const diff = now - ms;
  if (diff < 0) return new Date(ms).toLocaleString();
  const sec = Math.floor(diff / 1000);
  if (sec < 60) return '刚刚';
  const min = Math.floor(sec / 60);
  if (min < 60) return min + ' 分钟前';
  const hr = Math.floor(min / 60);
  if (hr < 24) return hr + ' 小时前';
  const day = Math.floor(hr / 24);
  if (day < 7) return day + ' 天前';
  // 同年省略年份
  const d = new Date(ms);
  const sameYear = d.getFullYear() === new Date(now).getFullYear();
  const pad = n => String(n).padStart(2, '0');
  return (sameYear ? '' : d.getFullYear() + '-') +
         pad(d.getMonth() + 1) + '-' + pad(d.getDate()) +
         ' ' + pad(d.getHours()) + ':' + pad(d.getMinutes());
}

function setBusy(type) {
  S.busy = !!type;
  S.busyType = type || '';
  syncBusyUi();
}

function syncBusyUi() {
  $$('.nfc-op').forEach(b => b.disabled = S.busy);
}

// ---- Toast 提示 ----
let _toastTimer = null;
function toast(msg, kind) {
  const el = $('#toast');
  el.textContent = msg;
  el.className = 'toast ' + (kind || '') + ' show';
  clearTimeout(_toastTimer);
  const dur = kind === 'error' ? 5000 : 2800;
  _toastTimer = setTimeout(() => { el.classList.remove('show'); }, dur);
}

// ---- 自定义对话框（替代 confirm/prompt） ----
function confirm2(msg) {
  return new Promise(resolve => {
    const overlay = $('#modal');
    $('#modalMsg').textContent = msg;
    overlay.classList.remove('hidden');
    const done = (val) => { overlay.classList.add('hidden'); resolve(val); };
    $('#modalOk').onclick = () => done(true);
    $('#modalCancel').onclick = () => done(false);
    overlay.onclick = (e) => { if (e.target === overlay) done(false); };
  });
}

function prompt2(msg, defaultVal) {
  return new Promise(resolve => {
    const overlay = $('#promptModal');
    $('#promptMsg').textContent = msg;
    const inp = $('#promptInput');
    inp.value = defaultVal || '';
    overlay.classList.remove('hidden');
    setTimeout(() => inp.focus(), 50);
    const done = (val) => { overlay.classList.add('hidden'); resolve(val); };
    $('#promptOk').onclick = () => done(inp.value.trim());
    $('#promptCancel').onclick = () => done(null);
    overlay.onclick = (e) => { if (e.target === overlay) done(null); };
    inp.onkeydown = (e) => { if (e.key === 'Enter') done(inp.value.trim()); };
  });
}

// ---- API 请求 ----
async function api(path, opts) {
  opts = opts || {};
  if (opts.body && typeof opts.body === 'object' && !(opts.body instanceof ArrayBuffer)) {
    opts.body = JSON.stringify(opts.body);
    opts.headers = Object.assign({ 'Content-Type': 'application/json' }, opts.headers || {});
  }
  const ctl = new AbortController();
  const timeoutMs = opts.timeout || 8000;
  const tid = setTimeout(() => ctl.abort(), timeoutMs);
  opts.signal = ctl.signal;
  try {
    const r = await fetch(path, opts);
    const ct = r.headers.get('content-type') || '';
    if (ct.includes('application/json')) return r.json();
    return r.text();
  } finally {
    clearTimeout(tid);
  }
}

// ---- 日志 ----
function addLog(msg, kind) {
  S.logs.unshift({ msg, kind: kind || '' });
  if (S.logs.length > 60) S.logs.length = 60;
  renderLogs();
}

function renderLogs() {
  const el = $('.log-box');
  if (!el) return;
  el.innerHTML = S.logs.map(l =>
    '<div class="' + esc(l.kind) + '">' + esc(l.msg) + '</div>'
  ).join('');
}

// ---- WebSocket 连接 ----
function wsConnect() {
  if (S.ws) return;
  const ws = new WebSocket('ws://' + location.host + '/ws');
  S.ws = ws;
  ws.onopen = () => {
    addLog('[WS] 已连接', 'ok');
    $('#statusDot').classList.add('online');
    const hs = $('#hdrState'); if (hs) hs.textContent = '在线';
  };
  ws.onmessage = (ev) => {
    try { handleWs(JSON.parse(ev.data)); } catch (e) {}
  };
  ws.onclose = () => {
    S.ws = null;
    $('#statusDot').classList.remove('online');
    const hs = $('#hdrState'); if (hs) hs.textContent = '离线';
    addLog('[WS] 断开，3 s 后重连', 'err');
    setTimeout(wsConnect, 3000);
  };
  ws.onerror = () => { try { ws.close(); } catch (e) {} };
}

function handleWs(m) {
  if (m.type === 'progress') {
    S.sectors[m.sector] = { a: m.keyA, b: m.keyB };
    S.total = m.total;
    updateProgress();
    if (m.keyA || m.keyB) {
      addLog('[S' + m.sector + '] ' + (m.keyA ? 'A✓' : '') + (m.keyB ? ' B✓' : ''), 'ok');
    } else {
      addLog('[S' + m.sector + '] 未找到', 'err');
    }
  } else if (m.type === 'done') {
    const r = m.result || {};
    const wasReading = S.busyType === 'reading';
    const wasWriting = S.busyType === 'writing';
    if (r.verifyFails !== undefined) {
      if (r.verifyFails === 0) {
        addLog('[完成] 写入验证全部通过', 'ok');
        toast('写入完成，全部通过', 'ok');
      } else {
        addLog('[完成] ' + r.verifyFails + ' 个块验证失败', 'err');
        toast(r.verifyFails + ' 个块验证失败', 'error');
      }
      if (wasWriting && S.writeTarget) {
        S.writeTarget.verifyFails = r.verifyFails;
      }
    } else if (wasReading) {
      // 读取成功：生成摘要保留在扫描页
      let knownKeys = 0;
      for (let i = 0; i < S.total; i++) {
        const s = S.sectors[i];
        if (s) { if (s.a) knownKeys++; if (s.b) knownKeys++; }
      }
      const isNtag = r.pages !== undefined;
      const summary = isNtag
        ? (r.pages + ' 页已读取')
        : (S.total + ' 扇区 · ' + knownKeys + ' 把密钥');
      S.lastRead = { id: r.id, kind: isNtag ? 'ntag' : 'mfc', success: true, summary };
      addLog('[完成] ' + summary, 'ok');
    } else {
      addLog('[完成] 操作成功', 'ok');
      toast('操作完成', 'ok');
    }
    setBusy(null);
    if (S.tab === 'library') renderLibrary();
    else if (S.tab === 'library_detail') renderLibraryDetail();
    else if (S.tab === 'write_progress') renderWriteProgress();
    else if (S.tab === 'scan') renderScan();
  } else if (m.type === 'card_in') {
    const uid = m.uid || '';
    const isNewCard = uid !== S.lastCardUid;
    S.card = { uid, type: m.cardType, typeName: m.typeName, magic: m.magic || null };
    S.lastCardUid = uid;
    if (isNewCard) {
      // 换了一张新卡，清掉上一张的读取记录
      S.lastRead = null;
      S.sectors = [];
      S.logs = [];
      toast((m.typeName || '未知卡') + ' · ' + fmtUid(uid), 'ok');
    }
    if (S.tab === 'scan' && !S.busy) renderScan();
  } else if (m.type === 'card_meta') {
    // monitor PRESENT 阶段补的 magic 信息（avoid 持锁内做 magic 检测）
    if (!S.card) return;
    if (S.card.magic === m.magic) return;
    S.card.magic = m.magic || null;
    if (S.tab === 'scan' && !S.busy) renderScan();
  } else if (m.type === 'card_out') {
    // S.card 可能已在 error 处理里被提前清空（"卡被拿走"场景），这里不再重复渲染
    if (S.card === null) return;
    S.card = null;
    // lastCardUid 保留，供下次 card_in 判断是否同一张卡
    if (S.tab === 'scan' && !S.busy) renderScan();
  } else if (m.type === 'error') {
    addLog('[错误] ' + (m.msg || '未知'), 'err');
    const wasReading = S.busyType === 'reading';
    const wasWriting = S.busyType === 'writing';
    setBusy(null);
    // 读取失败保留进度面板 + 失败摘要
    if (wasReading) {
      S.lastRead = { id: null, kind: null, success: false, summary: m.msg || '读取失败' };
    }
    if (wasWriting && S.writeTarget) {
      if (m.msg && m.msg.indexOf('取消') >= 0) S.writeTarget.canceled = true;
      else S.writeTarget.error = m.msg || '写入失败';
    }
    // 卡被拿走导致的失败，后端先于监控任务给出错误；这里同步清掉卡片状态，
    // 避免 300~600ms 后到来的 card_out 再触发一次 renderScan 引发 UI 闪烁
    if (m.msg && m.msg.indexOf('卡被拿走') >= 0) {
      S.card = null;
    }
    toast(m.msg || '操作失败', 'error');
    if (S.tab === 'scan') renderScan();
    else if (S.tab === 'library_detail') renderLibraryDetail();
    else if (S.tab === 'write_progress') renderWriteProgress();
  }
}

function updateProgress() {
  let done = 0;
  for (let i = 0; i < S.total; i++) if (S.sectors[i]) done++;
  const pct = S.total > 0 ? Math.round(done * 100 / S.total) : 0;

  const fill = $('.progress-bar-fill');
  if (fill) {
    fill.style.width = pct + '%';
    fill.classList.toggle('writing', S.busyType === 'writing');
  }
  const pctEl = $('.progress-pct');
  if (pctEl) pctEl.textContent = pct + '%';

  const map = $('.sector-map');
  if (map) {
    let h = '';
    for (let i = 0; i < S.total; i++) {
      const s = S.sectors[i];
      let cls = 'sec';
      if (s) {
        if (s.a && s.b) cls += ' key-ab';
        else if (s.a) cls += ' key-a';
        else if (s.b) cls += ' key-b';
        else cls += ' fail';
      }
      h += '<div class="' + cls + '">' + i + '</div>';
    }
    map.innerHTML = h;
  }
  renderLogs();
}

// ---- 进度面板 HTML ----
function progressHtml() {
  return '<div class="progress-panel" id="progressPanel">' +
    '<div class="progress-bar-wrap"><div class="progress-bar-fill"></div></div>' +
    '<div class="progress-pct">0%</div>' +
    '<div class="sector-map"></div>' +
    '<div class="log-box"></div>' +
    '</div>';
}

function showProgress() {
  let p = $('#progressPanel');
  if (!p) {
    app.insertAdjacentHTML('beforeend', progressHtml());
  }
  updateProgress();
  renderLogs();
}

async function doCancelOp() {
  try { await api('/api/cancel', { method: 'POST' }); } catch (e) {}
  addLog('[取消] 已请求中断');
}

// ---- 状态查询 ----
async function refreshStatus() {
  try {
    const s = await api('/api/status');
    S.status = s;
    if (s.pn532Fw) $('#statusDot').classList.add('online');
  } catch (e) {}
}

/* ============================================================
   扫描页
   ============================================================ */
/*
 * 扫描页渲染分为四块独立面板，互相不依赖：
 *   - 扫描环：只表达"当前"状态（等待 / 扫描中 / 读取中 / 写入中 / 识别到卡）
 *   - 卡片面板：有卡就显示，含 UID / 类型 / 针对当前卡的操作按钮（读取 Dump / 读 NDEF / NDEF 写入）
 *   - 结果面板：有 lastRead 就显示，讲"上次读取"的结果和可点操作（查看详情）
 *   - 进度面板：正在读写 OR 有 lastRead 时显示
 *
 * 之前把"当前卡"与"上次结果"塞在一起，导致卡离开时操作按钮消失、
 * 状态交叠时按钮互相替换。拆开后每块独立，不再出现 UI 状态歧义。
 */

function isNtagCard(c) {
  if (!c) return false;
  return c.type === 3 ||
         (c.typeName || '').indexOf('NTAG') >= 0 ||
         (c.typeName || '').indexOf('Ultralight') >= 0;
}

function renderScan() {
  const hasCard    = !!S.card;
  const isScanning = S.busy && S.busyType === 'scanning';
  const isReading  = S.busy && S.busyType === 'reading';
  const isWriting  = S.busy && S.busyType === 'writing';
  const lr         = S.lastRead;
  const isNtag     = isNtagCard(S.card);

  const parts = [ringSectionHtml({isScanning, isReading, isWriting, hasCard})];
  if (hasCard) parts.push(cardInfoSectionHtml(S.card, {isReading, isNtag}));
  if (lr)      parts.push(resultPanelHtml(lr));
  app.innerHTML = parts.join('');

  if ((isReading || isWriting) || lr) {
    showProgress();
  }

  bindScanActions();
  syncBusyUi();
}

function ringSectionHtml({isScanning, isReading, isWriting, hasCard}) {
  // 无卡时用大圆环做主视觉（引导用户"贴卡"），有卡时收缩为顶部状态条，
  // 把视觉焦点让给下方的卡片面板
  if (!hasCard) {
    const ringCls = isScanning ? ' scanning' : '';
    const label   = isScanning ? '扫描中...' : '等待贴卡';
    return '<div class="scan-hero">' +
      '<div class="scan-ring' + ringCls + '" id="scanRing">' +
        '<svg class="scan-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5">' +
          '<rect x="2" y="5" width="20" height="14" rx="2"/>' +
          '<path d="M2 10h20"/>' +
          '<circle cx="12" cy="15" r="1.5"/>' +
        '</svg>' +
      '</div>' +
      '<div class="scan-label">' + esc(label) + '</div>' +
      '</div>';
  }

  let text, dotMod = '';
  if (isReading)      { text = '读取中'; dotMod = ' read'; }
  else if (isWriting) { text = '写入中'; dotMod = ' write'; }
  else                  text = '已识别';
  const sub = S.card.typeName || '';

  return '<div class="scan-status">' +
    '<div class="scan-status-dot' + dotMod + '"></div>' +
    '<div class="scan-status-text">' + esc(text) + '</div>' +
    (sub ? '<div class="scan-status-sub">' + esc(sub) + '</div>' : '') +
    '</div>';
}

function cardInfoSectionHtml(c, {isReading, isNtag}) {
  let h = '<div class="card-info">' +
    '<div class="card-header">' +
      '<div class="card-uid">' + esc(fmtUid(c.uid)) + '</div>' +
      '<div class="card-tags">' +
        '<span class="tag">' + esc(c.typeName || 'Unknown') + '</span>';
  if (c.magic) h += '<span class="tag magic">' + esc(c.magic) + '</span>';
  h += '</div></div>';

  h += '<div class="action-grid">';
  if (isReading) {
    h += '<button class="act-btn cancel-btn full" id="actCancelRead">' +
      '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>' +
      '取消读取</button>';
  } else {
    h += '<button class="act-btn primary nfc-op' + (isNtag ? '' : ' full') + '" id="actDump">' +
      '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M12 3v12M8 11l4 4 4-4"/><path d="M4 17v2a2 2 0 002 2h12a2 2 0 002-2v-2"/></svg>' +
      '读取 Dump</button>';
    if (isNtag) {
      h += '<button class="act-btn nfc-op" id="actNdefRead">' +
        '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><path d="M4 7V4a2 2 0 012-2h8l6 6v12a2 2 0 01-2 2H6a2 2 0 01-2-2v-3"/><path d="M14 2v6h6"/><path d="M2 15h10M8 11l4 4-4 4"/></svg>' +
        '读取 NDEF</button>';
    }
  }
  h += '</div>';

  if (isNtag && !isReading) {
    h += '<div class="ndef-section" id="ndefSection">' +
      '<div class="ndef-title">NDEF 写入</div>' +
      '<div id="ndefResult"></div>' +
      '<div class="form-row">' +
        '<select class="select" id="ndefType" style="width:90px;flex:none;">' +
          '<option value="uri">URL</option>' +
          '<option value="text">文本</option>' +
        '</select>' +
        '<input class="input" id="ndefPayload" placeholder="https://example.com">' +
      '</div>' +
      '<div class="form-row hidden" id="ndefLangRow">' +
        '<input class="input" id="ndefLang" placeholder="语言 (en, zh)" value="en" style="width:100px;">' +
      '</div>' +
      '<button class="btn primary full nfc-op mt8" id="actNdefWrite">写入 NDEF</button>' +
    '</div>';
  }

  h += '</div>';
  return h;
}

function resultPanelHtml(lr) {
  const okCls  = lr.success ? '' : ' result-err';
  const title  = lr.success ? '上次读取成功' : '上次读取失败';
  const iconSvg = lr.success
    ? '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6"><polyline points="20 6 9 17 4 12"/></svg>'
    : '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.6"><line x1="18" y1="6" x2="6" y2="18"/><line x1="6" y1="6" x2="18" y2="18"/></svg>';

  let h = '<div class="result-bar' + okCls + '">' +
    '<div class="result-bar-icon">' + iconSvg + '</div>' +
    '<div class="result-bar-main">' +
      '<div class="result-bar-title">' + esc(title) + '</div>' +
      '<div class="result-bar-summary">' + esc(lr.summary) + '</div>' +
    '</div>';

  if (lr.success && lr.id) {
    h += '<button class="result-bar-action" id="actViewDetail">查看详情</button>';
  }
  h += '</div>';
  return h;
}

function bindScanActions() {
  // 大圆环在 hasCard=true 时会被替换为不可点击的状态条，此时找不到 #scanRing
  const ring = $('#scanRing');
  if (ring) ring.onclick = doScan;

  const dumpBtn = $('#actDump');
  if (dumpBtn) dumpBtn.onclick = doDump;

  const cancelBtn = $('#actCancelRead');
  if (cancelBtn) cancelBtn.onclick = doCancelOp;

  const ndefReadBtn = $('#actNdefRead');
  if (ndefReadBtn) ndefReadBtn.onclick = doNdefRead;

  const ndefWriteBtn = $('#actNdefWrite');
  if (ndefWriteBtn) ndefWriteBtn.onclick = doNdefWrite;

  const viewBtn = $('#actViewDetail');
  if (viewBtn) viewBtn.onclick = viewLastReadDetail;

  const ndefType = $('#ndefType');
  if (ndefType) {
    ndefType.onchange = () => {
      const isText = ndefType.value === 'text';
      const langRow = $('#ndefLangRow');
      if (langRow) langRow.classList.toggle('hidden', !isText);
      const payload = $('#ndefPayload');
      if (payload) payload.placeholder = isText ? '输入文本内容' : 'https://example.com';
    };
  }
}

async function viewLastReadDetail() {
  const lr = S.lastRead;
  if (!lr || !lr.id) { toast('没有可查看的详情', 'error'); return; }
  try {
    const list = await api('/api/dumps');
    if (Array.isArray(list)) S.libList = list;
  } catch (e) {}
  const meta = (S.libList || []).find(x => x.id === lr.id);
  if (!meta) { toast('未找到对应 Dump', 'error'); return; }
  S.libDetailId = lr.id;
  S.libDetailMeta = meta;
  S.libDetailData = null;
  setTab('library_detail');
}

async function doScan() {
  if (S.busy) { toast('操作进行中', 'error'); return; }
  // 清扫描页所有上轮状态，界面立刻进入"扫描中"
  S.lastRead = null;
  S.card = null;
  S.sectors = [];
  S.logs = [];
  setBusy('scanning');
  renderScan();

  try {
    const r = await api('/api/scan', { method: 'POST' });
    setBusy(null);
    if (!r.ok) {
      S.card = null;
      renderScan();
      toast(r.err || '未发现卡片', 'error');
      return;
    }
    S.card = r;
    // 同步 lastCardUid，避免 monitor 紧接的 card_in 把同一张卡判为新卡再 toast 一次
    S.lastCardUid = r.uid;
    renderScan();
    toast(r.typeName + ' · ' + fmtUid(r.uid), 'ok');
  } catch (e) {
    setBusy(null);
    S.card = null;
    renderScan();
    toast('通信失败', 'error');
  }
}

async function doDump() {
  if (S.busy) { toast('操作进行中', 'error'); return; }
  S.lastRead = null;

  const c = S.card || {};
  const uidShort = (c.uid || '').slice(-8).toUpperCase();
  const typeTag = c.typeName || (c.type === 2 ? 'MFC4K' : c.type === 1 ? 'MFC' : 'Card');
  const name = typeTag + '-' + (uidShort || Date.now().toString(36));

  S.sectors = [];
  S.logs = [];
  const t = c.type;
  S.total = t === 2 ? 40 : (t === 1 ? 16 : 0);

  setBusy('reading');
  renderScan();           // 重渲染让"读取 Dump"变成"取消读取"
  updateProgress();

  try {
    const r = await api('/api/read', { method: 'POST', body: { name, ts: Date.now() } });
    if (!r.ok) {
      setBusy(null);
      renderScan();
      toast(r.err || '启动读取失败', 'error');
      return;
    }
    S.taskId = r.taskId;
    addLog('[开始] 字典攻击中...');
  } catch (e) {
    setBusy(null);
    renderScan();
    toast('通信失败', 'error');
  }
}

async function doNdefRead() {
  if (S.busy) { toast('操作进行中', 'error'); return; }
  setBusy('ndef_read');
  try {
    const r = await api('/api/ndef/read', { method: 'POST' });
    setBusy(null);
    const el = $('#ndefResult');
    if (el && r.ok) {
      el.innerHTML = '<div class="ndef-result">' +
        '<div class="ndef-type-badge">' + esc(r.type) + '</div>' +
        '<div>' + esc(r.payload) + '</div>' +
        (r.lang ? '<div style="color:var(--text3);font-size:11px;margin-top:4px;">lang: ' + esc(r.lang) + '</div>' : '') +
        '</div>';
    } else if (el) {
      el.innerHTML = '<div class="ndef-result" style="color:var(--red);">' + esc(r.err || '读取失败') + '</div>';
    }
    if (!r.ok) toast(r.err || '读取失败', 'error');
  } catch (e) {
    setBusy(null);
    toast('通信失败', 'error');
  }
}

async function doNdefWrite() {
  if (S.busy) { toast('操作进行中', 'error'); return; }
  const type = $('#ndefType')?.value || 'uri';
  const payload = ($('#ndefPayload')?.value || '').trim();
  if (!payload) { toast('请输入内容', 'error'); return; }

  setBusy('ndef_write');
  try {
    const body = { type, payload };
    if (type === 'text') body.lang = ($('#ndefLang')?.value || '').trim() || 'en';
    const r = await api('/api/ndef/write', { method: 'POST', body });
    setBusy(null);
    toast(r.ok ? 'NDEF 写入成功' : (r.err || '写入失败'), r.ok ? 'ok' : 'error');
    if (r.ok) {
      const pl = $('#ndefPayload'); if (pl) pl.value = '';
    }
  } catch (e) {
    setBusy(null);
    toast('通信失败', 'error');
  }
}

/* ============================================================
   卡库页
   ============================================================ */
async function renderLibrary() {
  app.innerHTML =
    '<h2 class="pg-title">卡库</h2>' +
    '<p class="pg-desc">管理已保存的卡片数据</p>' +
    '<div class="lib-toolbar">' +
      '<button class="btn primary flex1" id="importBtn">导入 Dump 文件</button>' +
      '<button class="btn" id="refreshLibBtn" title="刷新">↻</button>' +
      '<input type="file" id="importFile" accept=".bin,.mfd" class="hidden">' +
    '</div>' +
    '<div id="dumpList" class="dump-list"></div>';

  $('#importBtn').onclick = () => $('#importFile').click();
  $('#importFile').onchange = handleImport;
  $('#refreshLibBtn').onclick = () => renderLibrary();

  // 写入中显示进度面板
  if (S.busy && S.busyType === 'writing') {
    showProgress();
  }

  await loadDumps();
}

function importDialog(file) {
  return new Promise(resolve => {
    const overlay = $('#importModal');
    const defaultName = file.name.replace(/\.\w+$/, '');
    const defaultType = file.size <= 924 ? 'ntag' : 'mfc';
    $('#importFileInfo').textContent = file.name + ' · ' + fmtBytes(file.size);
    $('#importName').value = defaultName;
    $$('input[name="importType"]').forEach(r => { r.checked = r.value === defaultType; });
    overlay.classList.remove('hidden');
    setTimeout(() => $('#importName').focus(), 50);

    const done = (val) => { overlay.classList.add('hidden'); resolve(val); };
    $('#importOkBtn').onclick = () => {
      const name = $('#importName').value.trim();
      if (!name) { toast('请输入名字', 'error'); return; }
      const typeEl = document.querySelector('input[name="importType"]:checked');
      done({ name, type: typeEl ? typeEl.value : 'mfc' });
    };
    $('#importCancelBtn').onclick = () => done(null);
    overlay.onclick = (e) => { if (e.target === overlay) done(null); };
  });
}

async function handleImport(ev) {
  const file = ev.target.files[0];
  if (!file) return;
  const result = await importDialog(file);
  if (!result) { ev.target.value = ''; return; }
  try {
    const buf = await file.arrayBuffer();
    const r = await fetch('/api/dumps/upload?name=' + encodeURIComponent(result.name) + '&type=' + result.type + '&ts=' + Date.now(), {
      method: 'POST', body: buf, headers: { 'Content-Type': 'application/octet-stream' }
    }).then(r => r.json());
    if (r.ok) { toast('导入成功', 'ok'); renderLibrary(); }
    else toast(r.err || '导入失败', 'error');
  } catch (e) {
    toast('导入失败', 'error');
  }
  ev.target.value = '';
}

async function loadDumps() {
  const dl = $('#dumpList');
  let list;
  try {
    list = await api('/api/dumps');
  } catch (e) {
    dl.innerHTML = '<div class="empty-state"><div class="empty-text">加载失败</div></div>';
    return;
  }
  if (!Array.isArray(list) || !list.length) {
    dl.innerHTML =
      '<div class="empty-state">' +
        '<svg class="empty-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.5"><rect x="2" y="6" width="20" height="16" rx="2"/><path d="M2 10h20"/><circle cx="12" cy="16" r="2"/></svg>' +
        '<div class="empty-text">还没有保存的卡片</div>' +
        '<div class="empty-hint">去扫描页读取一张卡吧</div>' +
      '</div>';
    return;
  }

  list.sort((a, b) => (b.seq || 0) - (a.seq || 0));
  S.libList = list;  // 缓存以便子页面读取
  dl.innerHTML = list.map(d => {
    const typeName = d.type === 1 ? 'MFC' : (d.type === 2 ? 'MFC 4K' : 'NTAG');
    return '<div class="dump-card clickable" data-id="' + esc(d.id) + '">' +
      '<div class="dump-body">' +
        '<div class="dump-top">' +
          '<div>' +
            '<div class="dump-name">' + esc(d.name) + '</div>' +
            '<div class="dump-uid">' + esc(fmtUid(d.uid)) + '</div>' +
          '</div>' +
          '<span class="tag">' + esc(typeName) + '</span>' +
        '</div>' +
        '<div class="dump-stats">' +
          '<span class="dump-stat">' +
            '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><rect x="3" y="3" width="7" height="7"/><rect x="14" y="3" width="7" height="7"/><rect x="3" y="14" width="7" height="7"/><rect x="14" y="14" width="7" height="7"/></svg>' +
            esc(d.blocks) + ' 块</span>' +
          '<span class="dump-stat">' +
            '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="8" cy="15" r="4"/><path d="M10.85 12.15L19 4"/></svg>' +
            esc(d.knownKeys) + ' 密钥</span>' +
          '<span class="dump-stat">' + esc(fmtBytes(d.binSize)) + '</span>' +
          (d.createdMs ? '<span class="dump-stat" title="' + esc(new Date(d.createdMs).toLocaleString()) + '">' +
            '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></svg>' +
            esc(fmtRelTime(d.createdMs)) + '</span>' : '') +
        '</div>' +
      '</div>' +
      '<svg class="dump-chevron" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="9 6 15 12 9 18"/></svg>' +
    '</div>';
  }).join('');

  dl.onclick = (ev) => {
    const card = ev.target.closest('.dump-card');
    if (!card) return;
    openLibraryDetail(card.dataset.id);
  };
  syncBusyUi();
}

function openLibraryDetail(id) {
  S.libDetailId = id;
  S.libDetailMeta = (S.libList || []).find(x => x.id === id) || null;
  S.libDetailData = null;
  setTab('library_detail');
}

async function renderLibraryDetail() {
  const id = S.libDetailId;
  const meta = S.libDetailMeta;
  if (!id || !meta) { setTab('library'); return; }
  const typeName = meta.type === 1 ? 'MFC' : (meta.type === 2 ? 'MFC 4K' : 'NTAG');
  const isNtag = meta.type === 3;

  // detail body：缓存命中直接渲染，否则显示加载占位
  const cached = S.libDetailData && S.libDetailData._id === id ? S.libDetailData : null;
  const detailBodyHtml = cached
    ? (cached.kind === 'ntag' ? renderNtagDetail(cached) : renderMfcDetail(cached))
    : '<div class="detail-loading">加载详情中...</div>';

  app.innerHTML =
    '<div class="detail-topbar">' +
      '<button class="back-btn" id="backToLib"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="15 6 9 12 15 18"/></svg>卡库</button>' +
      '<div class="detail-title">' + esc(meta.name) + '</div>' +
    '</div>' +

    '<div class="detail-actbar">' +
      '<button class="dump-act nfc-op" data-act="rename">改名</button>' +
      '<button class="dump-act nfc-op" data-act="download">下载</button>' +
      (isNtag ? '' : '<button class="dump-act write-act nfc-op" data-act="write">写卡</button>') +
      '<button class="dump-act del-act nfc-op" data-act="del">删除</button>' +
    '</div>' +

    '<div class="info-card">' +
      '<div class="info-row"><span class="info-k">类型</span><span class="info-v">' + esc(typeName) + '</span></div>' +
      '<div class="info-row"><span class="info-k">UID</span><span class="info-v mono">' + esc(fmtUid(meta.uid)) + '</span></div>' +
      '<div class="info-row"><span class="info-k">块/页数</span><span class="info-v">' + esc(meta.blocks) + '</span></div>' +
      '<div class="info-row"><span class="info-k">已知密钥</span><span class="info-v">' + esc(meta.knownKeys) + '</span></div>' +
      '<div class="info-row"><span class="info-k">大小</span><span class="info-v">' + esc(fmtBytes(meta.binSize)) + '</span></div>' +
      (meta.createdMs ? '<div class="info-row"><span class="info-k">保存时间</span><span class="info-v">' +
        esc(new Date(meta.createdMs).toLocaleString()) + '</span></div>' : '') +
    '</div>' +

    '<div id="detailBody" class="detail-body">' + detailBodyHtml + '</div>';

  $('#backToLib').onclick = () => setTab('library');
  $('.detail-actbar').onclick = handleDumpDetailAction;

  syncBusyUi();

  if (cached) return;

  // 懒加载详情，仅首次进入需要拉
  try {
    const r = await api('/api/dumps/detail/' + encodeURIComponent(id));
    if (S.tab !== 'library_detail' || S.libDetailId !== id) return;
    if (!r.ok) {
      $('#detailBody').innerHTML = '<div class="detail-loading err">' + esc(r.err || '获取详情失败') + '</div>';
      return;
    }
    r._id = id;
    S.libDetailData = r;
    $('#detailBody').innerHTML = r.kind === 'ntag' ? renderNtagDetail(r) : renderMfcDetail(r);
  } catch (e) {
    if ($('#detailBody')) $('#detailBody').innerHTML = '<div class="detail-loading err">网络错误</div>';
  }
}

async function handleDumpDetailAction(ev) {
  const btn = ev.target.closest('[data-act]');
  if (!btn) return;
  const id = S.libDetailId;
  const meta = S.libDetailMeta || {};
  const act = btn.dataset.act;

  if (act === 'rename') {
    const name = await prompt2('新名字', meta.name || '');
    if (!name) return;
    try {
      await api('/api/dumps/' + encodeURIComponent(id), { method: 'PATCH', body: { name } });
      meta.name = name;
      $('.detail-title').textContent = name;
      toast('已改名', 'ok');
    } catch (e) { toast('重命名失败', 'error'); }

  } else if (act === 'download') {
    window.location.href = '/api/dumps/bin/' + encodeURIComponent(id);

  } else if (act === 'write') {
    if (S.busy) { toast('操作进行中', 'error'); return; }
    const warn = '把 Dump 写回当前贴近的卡？\n普通卡 Block 0（UID）不会被改写，其余块按已知密钥覆盖。';
    const ok = await confirm2(warn);
    if (!ok) return;
    S.sectors = [];
    S.logs = [];
    S.total = (meta.type === 2) ? 40 : 16;
    S.writeTarget = {
      dumpId: id,
      dumpName: meta.name || '',
      total: S.total,
      verifyFails: undefined,
      error: null,
      canceled: false,
    };
    setBusy('writing');
    setTab('write_progress');
    try {
      const r = await api('/api/write', { method: 'POST', body: { dumpId: id } });
      if (!r.ok) {
        setBusy(null);
        S.writeTarget.error = r.err || '启动写入失败';
        renderWriteProgress();
        toast(r.err || '启动写入失败', 'error');
      } else {
        S.taskId = r.taskId;
        addLog('[开始] 写回中...');
      }
    } catch (e) {
      setBusy(null);
      S.writeTarget.error = '通信失败';
      renderWriteProgress();
      toast('通信失败', 'error');
    }

  } else if (act === 'del') {
    const ok = await confirm2('确认删除这张卡？');
    if (!ok) return;
    try {
      await api('/api/dumps/' + encodeURIComponent(id), { method: 'DELETE' });
      toast('已删除', 'ok');
      setTab('library');
    } catch (e) { toast('删除失败', 'error'); }
  }
}

/* ============================================================
   写入进度子页
   ============================================================ */
function renderWriteProgress() {
  const t = S.writeTarget || {};
  const isBusy = S.busy && S.busyType === 'writing';

  const btnCls = isBusy ? 'cancel-btn-wide' : 'primary';
  const btnText = isBusy ? '写入中... 点击取消' : '返回详情';

  app.innerHTML =
    '<div class="detail-topbar">' +
      '<button class="back-btn" id="backFromWrite"><svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"><polyline points="15 6 9 12 15 18"/></svg>详情</button>' +
      '<div class="detail-title">写入 ' + esc(t.dumpName || '') + '</div>' +
    '</div>' +
    '<button class="btn full mt8 ' + btnCls + '" id="btnWriteAction">' + esc(btnText) + '</button>' +
    progressHtml();

  $('#backFromWrite').onclick = () => {
    if (isBusy) { toast('请先取消再返回', 'error'); return; }
    setTab('library_detail');
  };
  $('#btnWriteAction').onclick = isBusy ? doCancelOp : (() => setTab('library_detail'));

  updateProgress();
  renderLogs();
  syncBusyUi();
}

function renderMfcDetail(r) {
  return (r.sectors || []).map(s => {
    const noKey = !s.keyA && !s.keyB;
    return '<details class="detail-sector"' + (noKey ? '' : ' open') + '>' +
      '<summary class="detail-hdr' + (noKey ? ' nokey' : '') + '">Sector ' + s.sector +
        (s.keyA ? ' | A:' + esc(s.keyA) : '') +
        (s.keyB ? ' | B:' + esc(s.keyB) : '') +
        (noKey ? ' | 无密钥' : '') +
      '</summary>' +
      (s.blocks || []).map(b =>
        '<div class="detail-block' + (b.role === 'trailer' ? ' trailer' : '') + '">' +
          '<span>Block ' + b.block + '</span>' +
          '<span class="detail-badge">' + esc(b.desc) + '</span>' +
        '</div>'
      ).join('') +
    '</details>';
  }).join('');
}

function renderNtagDetail(r) {
  const typeName = r.ntagType ? ('NTAG' + r.ntagType) : '未知 NTAG';
  const cc = r.cc || {};
  const ndef = r.ndef || { found: false };
  let ndefHtml;
  if (ndef.found) {
    if (ndef.type === 'uri') {
      ndefHtml = '<span class="detail-badge">URL</span> <a href="' + esc(ndef.payload) + '" target="_blank" rel="noopener">' + esc(ndef.payload) + '</a>';
    } else if (ndef.type === 'text') {
      ndefHtml = '<span class="detail-badge">文本</span> <code>[' + esc(ndef.lang || '') + ']</code> ' + esc(ndef.payload);
    } else {
      ndefHtml = '<span class="detail-badge">未知类型</span>';
    }
  } else {
    ndefHtml = '<span class="detail-badge">未发现 NDEF</span>';
  }
  const pages = (r.pages || []).map(p =>
    '<div class="detail-block"><span>Page ' + p.page + '</span><span class="detail-badge mono">' + esc(p.hex) + '</span></div>'
  ).join('');
  return (
    '<div class="detail-sector">' +
      '<div class="detail-hdr">' + esc(typeName) + ' | ' + r.totalPages + ' 页 | 用户区 ' + r.userBytes + ' B | UID ' + esc(r.uid || '') + '</div>' +
      '<div class="detail-block"><span>CC</span><span class="detail-badge">magic ' + esc(cc.magic || '') +
        (cc.magicOk ? ' ✓' : ' ✗') + ' | ver ' + esc(cc.version || '') + ' | size ' + (cc.sizeBytes || 0) + ' B | access ' + esc(cc.access || '') + '</span></div>' +
      '<div class="detail-block"><span>静态锁</span><span class="detail-badge mono">' + esc(r.lock || '') + '</span></div>' +
      '<div class="detail-block"><span>NDEF</span><span>' + ndefHtml + '</span></div>' +
    '</div>' +
    '<details class="detail-sector"><summary class="detail-hdr">全部页 (' + (r.pages || []).length + ')</summary>' + pages + '</details>'
  );
}

/* ============================================================
   密钥页
   ============================================================ */
async function renderKeys() {
  app.innerHTML =
    '<h2 class="pg-title">密钥库</h2>' +
    '<p class="pg-desc">字典攻击时使用的密钥列表</p>' +
    '<div class="key-section">' +
      '<div class="key-section-title">自定义密钥</div>' +
      '<div class="key-add-row">' +
        '<input class="input" id="keyInput" placeholder="FFFFFFFFFFFF" maxlength="12" style="flex:1;">' +
        '<button class="btn primary" id="addKeyBtn">添加</button>' +
      '</div>' +
      '<div class="key-grid" id="userKeys"></div>' +
    '</div>' +
    '<div class="key-section">' +
      '<div class="key-section-title">默认密钥</div>' +
      '<div class="key-grid" id="defaultKeys"></div>' +
    '</div>';

  let data;
  try {
    data = await api('/api/keys');
  } catch (e) {
    $('#defaultKeys').innerHTML = '<div class="empty-state"><div class="empty-text">加载失败</div></div>';
    return;
  }

  $('#defaultKeys').innerHTML = (data.defaults || []).map(k =>
    '<div class="key-chip">' + esc(k) + '</div>'
  ).join('');

  const userArr = data.user || [];
  if (userArr.length) {
    $('#userKeys').innerHTML = userArr.map((k, i) =>
      '<div class="key-chip">' + esc(k) +
        '<button class="key-del" data-idx="' + i + '">×</button>' +
      '</div>'
    ).join('');
  } else {
    $('#userKeys').innerHTML = '<div style="grid-column:1/-1;color:var(--text3);font-size:13px;padding:12px 0;">暂无自定义密钥</div>';
  }

  $('#userKeys').onclick = async (ev) => {
    const del = ev.target.closest('.key-del');
    if (!del) return;
    const chip = del.parentElement;
    const keyText = chip ? (chip.firstChild.textContent || '').trim() : '';
    const ok = await confirm2('删除密钥 ' + keyText + '？');
    if (!ok) return;
    try {
      await api('/api/keys/' + del.dataset.idx, { method: 'DELETE' });
      renderKeys();
    } catch (e) { toast('删除失败', 'error'); }
  };

  const keyInput = $('#keyInput');
  $('#addKeyBtn').onclick = async () => {
    const v = (keyInput.value || '').trim().toUpperCase();
    if (!/^[0-9A-F]{12}$/.test(v)) {
      toast('需要 12 位十六进制', 'error');
      return;
    }
    try {
      const r = await api('/api/keys', { method: 'POST', body: { key: v } });
      if (r.ok) { toast('已添加', 'ok'); renderKeys(); }
      else toast(r.err || '添加失败', 'error');
    } catch (e) { toast('通信失败', 'error'); }
  };

  keyInput.onkeydown = (e) => {
    if (e.key === 'Enter') $('#addKeyBtn').click();
  };
}

/* ============================================================
   设置页
   ============================================================ */
function renderSettings() {
  const s = S.status || {};
  const fw = s.pn532Fw ? '0x' + s.pn532Fw.toString(16).padStart(8, '0') : '--';
  const free = (s.fsTotal && s.fsUsed) ? fmtBytes(s.fsTotal - s.fsUsed) : '--';

  app.innerHTML =
    '<h2 class="pg-title">设置</h2>' +
    '<p class="pg-desc">设备信息与固件更新</p>' +

    '<div class="info-card">' +
      '<div class="info-card-title">运行状态</div>' +
      '<div class="info-row"><span class="info-k">固件版本</span><span class="info-v">' + esc(s.version || '--') + '</span></div>' +
      '<div class="info-row"><span class="info-k">PN532 固件</span><span class="info-v">' + esc(fw) + '</span></div>' +
      '<div class="info-row"><span class="info-k">运行时间</span><span class="info-v">' + esc(fmtUptime(s.uptime)) + '</span></div>' +
      '<div class="info-row"><span class="info-k">空闲堆</span><span class="info-v">' + esc(fmtBytes(s.freeHeap || 0)) + '</span></div>' +
      '<div class="info-row"><span class="info-k">存储</span><span class="info-v">' +
        esc(fmtBytes(s.fsUsed || 0)) + ' / ' + esc(fmtBytes(s.fsTotal || 0)) +
        ' <span style="color:var(--text3)">（剩 ' + esc(free) + '）</span>' +
      '</span></div>' +
    '</div>' +

    '<div class="ota-section">' +
      '<div class="key-section-title">固件更新（OTA）</div>' +
      '<div class="ota-hint">选择编译好的 .bin 固件，更新后设备自动重启</div>' +
      '<input type="file" id="otaFile" accept=".bin" class="hidden">' +
      '<button class="btn primary full" id="otaBtn">选择固件文件</button>' +
      '<div class="ota-progress hidden" id="otaProgress">' +
        '<div class="progress-bar-wrap"><div class="progress-bar-fill" id="otaBar"></div></div>' +
        '<div class="progress-pct" id="otaStatus">0%</div>' +
        '<button class="btn full mt8" id="otaAbortBtn">取消上传</button>' +
      '</div>' +
    '</div>';

  $('#otaBtn').onclick = () => $('#otaFile').click();
  $('#otaFile').onchange = handleOta;
}

async function handleOta(ev) {
  const file = ev.target.files[0];
  if (!file) return;
  const ok = await confirm2('确认上传固件 ' + file.name + '？\n更新后设备将重启');
  if (!ok) { ev.target.value = ''; return; }

  const btn = $('#otaBtn');
  btn.disabled = true;
  btn.textContent = '上传中...';
  $('#otaProgress').classList.remove('hidden');
  $('#otaStatus').textContent = '0%';
  $('#otaBar').style.width = '0%';

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/api/ota');
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');

  const abortBtn = $('#otaAbortBtn');
  if (abortBtn) abortBtn.onclick = () => {
    xhr.abort();
    $('#otaStatus').textContent = '已取消';
    btn.disabled = false;
    btn.textContent = '选择固件文件';
  };

  xhr.upload.onprogress = (e) => {
    if (e.lengthComputable) {
      const pct = Math.round(e.loaded * 100 / e.total);
      $('#otaBar').style.width = pct + '%';
      $('#otaStatus').textContent = pct + '%';
    }
  };

  xhr.onload = () => {
    try {
      const r = JSON.parse(xhr.responseText);
      if (r.ok) {
        $('#otaStatus').textContent = '更新完成，重启中...';
        $('#otaBar').style.width = '100%';
        toast('固件更新成功，设备即将重启', 'ok');
      } else {
        $('#otaStatus').textContent = r.err || '失败';
        toast(r.err || '更新失败', 'error');
        btn.disabled = false;
        btn.textContent = '选择固件文件';
      }
    } catch (e) {
      $('#otaStatus').textContent = '响应异常';
      btn.disabled = false;
      btn.textContent = '选择固件文件';
    }
  };

  xhr.onerror = () => {
    $('#otaStatus').textContent = '上传失败';
    toast('上传失败', 'error');
    btn.disabled = false;
    btn.textContent = '选择固件文件';
  };

  xhr.send(file);
  ev.target.value = '';
}

/* ============================================================
   路由
   ============================================================ */
function setTab(name) {
  // S.card 由 WS card_in / card_out 实时维护，S.lastRead 由读取完成事件维护，
  // 两者都是"当前事实"，不需要在切页时清理；否则从详情页返回扫描页会丢失上次结果。
  S.tab = name;
  // library_detail / write_progress 都仍然高亮卡库 tab
  const navName = (name === 'library_detail' || name === 'write_progress') ? 'library' : name;
  $$('.nav-item').forEach(t => t.classList.toggle('active', t.dataset.tab === navName));
  if (name === 'scan') renderScan();
  else if (name === 'library') renderLibrary();
  else if (name === 'library_detail') renderLibraryDetail();
  else if (name === 'write_progress') renderWriteProgress();
  else if (name === 'keys') renderKeys();
  else if (name === 'settings') renderSettings();
}

$$('.nav-item').forEach(t => {
  t.onclick = () => {
    if (S.tab === 'write_progress' && S.busy) { toast('写入中，请先取消', 'error'); return; }
    setTab(t.dataset.tab);
  };
});

/* ============================================================
   启动
   ============================================================ */
refreshStatus();
setInterval(refreshStatus, 6000);
wsConnect();
setTab('scan');
