// 碎片档案 · P2P 协作解谜（网页案情板）
// 网络连接复用 C 核心：server.py 通过命令桥 spawn ./client_transfer，
// 把浏览器操作翻译成 client 的 stdin 命令，再轮询它每次重绘落盘的 ui_state.json。

const $ = (id) => document.getElementById(id);
const LS = { nick: 'fa.nick', color: 'fa.color', sound: 'fa.sound' };
const COLORS = ['#4fc3f7', '#ffb74d', '#81c784', '#ba68c8', '#f06292', '#ff8a65'];
const FILE_NAMES = { 1: '现场勘查图', 2: '证物特写', 3: '案卷正文' };
// 与 include/ui_state.h 对齐
const PHASE_LABEL = { 0: '空闲', 1: '在房', 2: '第一章', 3: '章间问答', 4: '第二章', 5: '三项择一', 6: '结束' };
const PATH_LABEL = { 0: '未知', 1: '本网直连', 2: '打洞', 3: '中继', 4: '通告发现' };

const S = {
  nick: localStorage.getItem(LS.nick) || '',
  color: localStorage.getItem(LS.color) || COLORS[0],
  sound: localStorage.getItem(LS.sound) !== '0',
  bridge: sessionStorage.getItem('fa.bridge') || '',
  role: sessionStorage.getItem('fa.role') || '',
  running: false,
  state: null,
  prevPhase: null,
  pollTimer: null,
};

// ---------------- 工具 ----------------
async function api(path, opts = {}) {
  const r = await fetch(path, {
    method: opts.method || 'GET',
    headers: opts.body ? { 'Content-Type': 'application/json' } : {},
    body: opts.body ? JSON.stringify(opts.body) : undefined,
  });
  const j = await r.json().catch(() => ({}));
  if (!r.ok) throw new Error(j.error || ('请求失败 ' + r.status));
  return j;
}

function toast(msg, err = false) {
  document.querySelectorAll('.toast').forEach((t) => t.remove());
  const t = document.createElement('div');
  t.className = 'toast' + (err ? ' err' : '');
  t.textContent = msg;
  document.body.appendChild(t);
  setTimeout(() => t.remove(), 2600);
}

function beep() {
  if (!S.sound) return;
  try {
    const ctx = new (window.AudioContext || window.webkitAudioContext)();
    const o = ctx.createOscillator(); const g = ctx.createGain();
    o.frequency.value = 880; o.type = 'sine';
    g.gain.value = 0.08; o.connect(g); g.connect(ctx.destination);
    o.start(); o.stop(ctx.currentTime + 0.12);
  } catch (e) { /* 忽略 */ }
}

function fmtSize(n) {
  if (!n) return '0 B';
  if (n >= 1048576) return (n / 1048576).toFixed(1) + ' MB';
  if (n >= 1024) return (n / 1024).toFixed(1) + ' KB';
  return n + ' B';
}

function sessionHex() {
  const sid = S.state?.session_id;
  if (!sid) return '';
  return sid.toString(16).padStart(8, '0');
}

// ---------------- 视图路由 ----------------
function showView(name) {
  ['lobby', 'game', 'history', 'settings'].forEach((v) => {
    $('view-' + v).hidden = (v !== name);
  });
  const inGame = S.bridge && name === 'game';
  $('meta-session').hidden = !inGame;
  $('meta-phase').hidden = !inGame;
  if (inGame) syncMeta();
}

function syncMeta() {
  const st = S.state;
  $('badge-session').textContent = sessionHex() ? '#' + sessionHex().toUpperCase() : '—';
  $('badge-phase').textContent = PHASE_LABEL[st?.phase ?? 0] || '—';
}

// ---------------- 灯箱 ----------------
function openLightbox(file, sidHexOverride) {
  const { file_id, kind, img_path, desc } = file;
  const sidHex = sidHexOverride || sessionHex();
  const imgBox = $('lb-img');
  const descEl = $('lb-desc');
  imgBox.className = 'lb-img';
  imgBox.innerHTML = '';
  descEl.textContent = '';
  $('lb-title').textContent = FILE_NAMES[file_id] || ('证物 ' + file_id);
  $('lb-sub').textContent = `${kind === 1 ? '图' : '文'}证物 · 章${file.chapter ?? '?'}`;

  const byPath = img_path ? ('/ui_prototype/' + img_path) : '';
  const byApi = sidHex ? `/api/evidence/${sidHex}/${file_id}` : '';
  const url = byPath || byApi;

  if (!url) {
    descEl.textContent = '证物尚未下载，先点「下载证物」获取';
    $('lightbox').hidden = false;
    requestAnimationFrame(() => $('lightbox').classList.add('open'));
    return;
  }

  if (kind === 1) {
    imgBox.innerHTML = '<div style="color:#8b9bb8">加载中…</div>';
    fetch(url).then((r) => {
      if (!r.ok) throw new Error();
      return r.blob();
    }).then((blob) => {
      const img = document.createElement('img');
      img.alt = FILE_NAMES[file_id] || '';
      img.onload = () => { imgBox.innerHTML = ''; imgBox.appendChild(img); imgBox.classList.add('has-img'); };
      img.src = URL.createObjectURL(blob);
    }).catch(() => { imgBox.innerHTML = ''; descEl.textContent = desc || '图片加载失败（可能尚未下载）'; });
  } else {
    descEl.textContent = '读取中…';
    fetch(url).then((r) => r.text()).then((txt) => {
      descEl.textContent = txt.split('[判据]')[0].trim() || desc || '(空)';
    }).catch(() => { descEl.textContent = '读取失败（可能尚未下载）'; });
  }
  $('lightbox').hidden = false;
  requestAnimationFrame(() => $('lightbox').classList.add('open'));
}

function closeLightbox() {
  const lb = $('lightbox');
  lb.classList.remove('open');
  setTimeout(() => { lb.hidden = true; }, 280);
}

// ---------------- 大厅 ----------------
let cases = [];
async function loadCases() {
  try { cases = (await api('/api/cases')).cases || []; } catch (e) { cases = []; }
  const sel = $('case-pick');
  sel.innerHTML = cases.map((c) =>
    `<option value="${c.sid}">${c.title} · #${c.sid.toUpperCase()}</option>`).join('')
    || '<option value="">（暂无历史案卷）</option>';
  renderHistory(cases);
}

async function loadCapability() {
  try {
    const cap = await api('/api/bridge/capability');
    const note = $('bridge-note');
    if (!cap.available) {
      note.hidden = false;
      note.className = 'bridge-note warn';
      note.textContent = cap.wsl_hint
        ? '⚠ 检测到 WSL 编译的 C 核心（ELF 产物），Windows 无法直接执行。请在 WSL/Linux 下运行 `python ui_prototype/server.py`，届时进房/下载/择一将走 C 核心 P2P。本地功能（历史案卷、设置、现场生成）仍可用。'
        : '⚠ C 核心 client_transfer 未编译：请在 WSL/Linux 下运行 `make transfer`（room_server / relay_server / client_transfer）。本地功能（历史案卷、设置、现场生成）仍可用。';
    } else {
      note.hidden = false;
      note.className = 'bridge-note';
      note.textContent = '✓ C 核心命令桥就绪：进房 / 下载 / 择一均走 P2P（room_server + relay_server + transfer）。';
    }
  } catch (e) { /* 忽略 */ }
}

// 案件来源切换
$('case-source').addEventListener('click', (e) => {
  const btn = e.target.closest('.seg-btn');
  if (!btn) return;
  document.querySelectorAll('#case-source .seg-btn').forEach((b) => b.classList.remove('active'));
  btn.classList.add('active');
  $('case-pick-wrap').hidden = (btn.dataset.src !== 'pick');
});

// 现场生成新案：POST 后轮询直到完成
async function generateAndPick() {
  toast('正在生成新案（约 30~90 秒）…');
  try { await api('/api/generate', { method: 'POST' }); } catch (e) { toast(e.message, true); return null; }
  while (true) {
    await new Promise((r) => setTimeout(r, 2000));
    try {
      const g = await api('/api/generate');
      if (!g.busy) {
        if (g.error) { toast('生成失败：' + g.error, true); return null; }
        break;
      }
    } catch (e) { /* 继续 */ }
  }
  await loadCases();
  toast('新案已生成');
  return cases.length ? cases[0].sid : null;
}

async function startBridge(payload) {
  const j = await api('/api/bridge/start', { method: 'POST', body: payload });
  S.bridge = j.bridge_id;
  S.role = j.role;
  sessionStorage.setItem('fa.bridge', S.bridge);
  sessionStorage.setItem('fa.role', S.role);
  S.state = null;
  S.prevPhase = null;
  showView('game');
  renderGame();
  startPolling();
  pollNow();
}

$('btn-host-join').addEventListener('click', async () => {
  const nick = ($('host-nick').value || '').trim() || S.nick || 'detective';
  if (nick) { S.nick = nick; localStorage.setItem(LS.nick, nick); }
  const src = document.querySelector('#case-source .seg-btn.active').dataset.src;
  try {
    let sid = '';
    if (src === 'auto') {
      sid = await generateAndPick();
      if (!sid) return;
    } else {
      sid = $('case-pick').value || '';
      if (!sid) { toast('请选择历史案卷', true); return; }
    }
    const mode = $('host-mode').value;
    toast('正在启动 C 核心并进房…');
    await startBridge({ role: 'host', nick, session: sid, mode });
  } catch (e) { toast(e.message, true); }
});

$('btn-player-join').addEventListener('click', async () => {
  const nick = ($('player-nick').value || '').trim() || S.nick || 'detective';
  if (nick) { S.nick = nick; localStorage.setItem(LS.nick, nick); }
  const host = ($('player-host').value || '').trim() || 'localhost';
  const mode = $('player-mode').value;
  try {
    toast('正在启动 C 核心并进房…');
    await startBridge({ role: 'player', nick, host, mode });
  } catch (e) { toast(e.message, true); }
});

$('btn-history').addEventListener('click', () => showView('history'));
$('btn-history-back').addEventListener('click', () => showView('lobby'));
$('btn-settings').addEventListener('click', () => { renderSettings(); showView('settings'); });

// ---------------- 命令桥（游戏内操作） ----------------
async function sendCmd(cmd) {
  try {
    await api(`/api/bridge/${S.bridge}/command`, { method: 'POST', body: { cmd } });
    return true;
  } catch (e) {
    toast(e.message, true);
    return false;
  }
}

async function leaveGame() {
  if (S.bridge) {
    api(`/api/bridge/${S.bridge}/quit`, { method: 'POST' }).catch(() => {});
  }
  stopPolling();
  S.bridge = ''; S.role = ''; S.state = null; S.prevPhase = null; S.running = false;
  sessionStorage.removeItem('fa.bridge');
  sessionStorage.removeItem('fa.role');
  showView('lobby');
}

// ---------------- 游戏渲染 ----------------
function renderConnBar() {
  const st = S.state;
  const phase = st?.phase ?? 0;
  $('conn-dot').className = 'dot ' + (S.running ? 'on' : 'off');
  $('conn-phase').textContent = PHASE_LABEL[phase] || '—';
  $('conn-path').textContent = PATH_LABEL[st?.path_type ?? 0] || '—';
  $('conn-session').textContent = sessionHex() ? '#' + sessionHex().toUpperCase() : '—';
  $('conn-room').textContent = st?.room_id ?? '—';
  $('conn-status').textContent = st?.status || (S.running ? '正在连接房间…' : '连接已断开');
}

function renderGame() {
  if (!S.bridge) return;
  syncMeta();
  renderConnBar();
  const st = S.state;
  const phase = st?.phase ?? 0;

  // 案件标题：优先从案件库匹配 session
  const sidHex = sessionHex();
  const c = sidHex ? cases.find((x) => x.sid === sidHex) : null;
  $('game-title').textContent = c ? c.title : (st && sidHex ? ('案件 #' + sidHex.toUpperCase()) : '案件');

  renderBoard(st, sidHex);
  renderMembers(st);
  renderEvents(st);
  renderActions(st, phase, c);
}

function renderBoard(st, sidHex) {
  const board = $('board');
  board.innerHTML = '';
  const files = (st && st.files) || [];
  if (!files.length) {
    board.innerHTML = '<div class="empty">' +
      (st ? '等待证物清单（C 核心分发中）…' : '正在启动 client_transfer…') + '</div>';
    return;
  }
  files.forEach((f) => {
    const have = !!f.have;
    const name = FILE_NAMES[f.file_id] || ('证物 ' + f.file_id);
    const icon = f.kind === 1 ? '📷' : '📄';
    const kindTxt = f.kind === 1 ? '图' : '文';

    const code = 'E-' + String(f.file_id + 1).padStart(2, '0');
    const evMeta = have
      ? '点击卡片在灯下检视这件证物'
      : '碎片在队友手中，点「下载证物」经 P2P 拉取';

    const card = document.createElement('div');
    card.className = 'card ' + (have ? 'owned' : 'missing');
    card.setAttribute('data-ev', name);
    card.setAttribute('data-evno', code + ' · ' + kindTxt);
    card.setAttribute('data-evmeta', evMeta);
    card.innerHTML = `
      <div class="card-inner">
        <div class="face">
          <span class="card-pin"></span>
          <div class="icon">${icon}</div>
          <div class="name">${name}</div>
          <div class="kind">${kindTxt}证物 · ${fmtSize(f.size)}</div>
          <div class="hint-face">${have ? '点击查看' : '尚未获取'}</div>
        </div>
        <div class="back">
          <div class="q">?</div>
          <div class="wait">${have ? '已获取' : '待 P2P 下载'}</div>
          <div class="recv">${code} · ${kindTxt}证物</div>
        </div>
      </div>
      <button class="share-btn">📤 查看证物</button>`;

    if (have) {
      card.addEventListener('click', (e) => {
        if (e.target.classList.contains('share-btn')) return;
        openLightbox(f, sidHex);
      });
      card.querySelector('.share-btn').addEventListener('click', (e) => {
        e.stopPropagation();
        openLightbox(f, sidHex);
      });
    }
    board.appendChild(card);
  });
}

function renderMembers(st) {
  const ul = $('members');
  const peers = (st && st.peers) || [];
  const myPeer = st?.my_peer_id;
  ul.innerHTML = peers.map((p) => `
    <li class="${p.peer_id === myPeer ? 'me' : ''}">
      <span class="dot on"></span>
      <span class="who">${p.nick || ('peer_' + p.peer_id)}${p.peer_id === myPeer ? '（我）' : ''}</span>
      <span class="cnt">${p.refl_ip || p.priv_ip || '-'}:${p.tcp_port || '?'}</span>
    </li>`).join('') || '<li class="empty">暂无成员</li>';
}

function renderEvents(st) {
  const ul = $('events');
  const evs = (st && st.events) || [];
  ul.innerHTML = evs.map((e) => `<li>${e}</li>`).join('') || '<li>（无）</li>';
}

function renderActions(st, phase, c) {
  const wrap = $('actions');
  const hasFiles = (st?.files || []).length > 0;
  const allHave = hasFiles && st.files.every((f) => f.have);

  const leave = '<button class="btn" id="btn-exit">离开</button>';

  if (phase === 6) { // DONE
    wrap.innerHTML = `
      <span class="conn"><span class="dot on"></span>真相已揭晓</span>
      <span class="spacer"></span>
      <button class="btn primary" id="btn-result">查看结果</button>
      ${leave}`;
    $('btn-result').addEventListener('click', () => showResult(c));
    $('btn-exit').addEventListener('click', leaveGame);
    return;
  }

  if (phase === 5) { // CHOICE 三项择一
    wrap.innerHTML = `
      <span class="conn"><span class="dot on"></span>三项择一 · 指认真凶</span>
      <span class="spacer"></span>
      <button class="btn primary" id="btn-choice">我有答案了</button>
      ${leave}`;
    $('btn-choice').addEventListener('click', openChoice);
    $('btn-exit').addEventListener('click', leaveGame);
    return;
  }

  if (phase === 3) { // QUIZ 章间问答
    wrap.innerHTML = `
      <span class="conn"><span class="dot on"></span>章间问答 · 回答后进入第二章</span>
      <span class="spacer"></span>
      <button class="btn primary" id="btn-quiz">提交问答</button>
      ${leave}`;
    $('btn-quiz').addEventListener('click', openQuiz);
    $('btn-exit').addEventListener('click', leaveGame);
    return;
  }

  // phase 0/1/2/4
  const getBtn = (phase >= 1) ? '<button class="btn" id="btn-get">📥 下载证物</button>' : '';
  const nextBtn = (phase === 2 || phase === 4) ? '<button class="btn primary" id="btn-next">推进阶段</button>' : '';
  const label = phase === 0 ? '正在连接房间…'
    : phase === 1 ? '已入房 · 等待证物清单'
    : allHave ? '证物已集齐 · 可推进阶段'
    : '交换证物 · 集齐后推进';
  wrap.innerHTML = `
    <span class="conn"><span class="dot ${S.running ? 'on' : 'off'}"></span>${label}</span>
    <span class="spacer"></span>
    ${getBtn}${nextBtn}${leave}`;
  const g = $('btn-get');
  if (g) g.addEventListener('click', async () => {
    toast('已发送下载请求（C 核心 P2P 拉取中）…');
    await sendCmd('get');
  });
  const n = $('btn-next');
  if (n) n.addEventListener('click', async () => { await sendCmd('next'); });
  $('btn-exit').addEventListener('click', leaveGame);
}

// ---------------- 择一 ----------------
let suspects = [];
async function openChoice() {
  if (!suspects.length) { try { suspects = (await api('/api/suspects')).suspects || []; } catch (e) {} }
  $('suspect-btns').innerHTML = suspects.map((s) =>
    `<button class="suspect-btn" data-name="${s}">${s}</button>`).join('');
  $('choice-modal').hidden = false;
}
$('btn-choice-cancel').addEventListener('click', () => { $('choice-modal').hidden = true; });
$('suspect-btns').addEventListener('click', async (e) => {
  const name = e.target.dataset.name;
  if (!name) return;
  $('choice-modal').hidden = true;
  const ok = await sendCmd('choose ' + name);
  if (ok) { toast('已提交：' + name); beep(); }
});

// ---------------- 章间问答 ----------------
function openQuiz() {
  $('quiz-answer').value = '';
  $('quiz-modal').hidden = false;
}
$('btn-quiz-cancel').addEventListener('click', () => { $('quiz-modal').hidden = true; });
$('btn-quiz-submit').addEventListener('click', async () => {
  const a = ($('quiz-answer').value || '').trim();
  if (!a) { toast('请先填写回答', true); return; }
  $('quiz-modal').hidden = true;
  const ok = await sendCmd('answer ' + a);
  if (ok) toast('问答已提交');
});

// ---------------- 结算 ----------------
function showResult(c) {
  $('result-title').textContent = '真相揭晓';
  $('result-body').innerHTML = `
    <div class="result-truth">真凶：${c?.murderer || '（见终端 logs/choice_summary.log）'}</div>
    <div class="result-list" style="margin-top:12px">
      <div class="result-row"><span class="rn">你的答案</span>
      <span class="rc">已记录于 logs/choice_summary.log</span></div>
    </div>`;
  $('result-modal').hidden = false;
}
$('btn-result-ok').addEventListener('click', () => { $('result-modal').hidden = true; leaveGame(); });

// ---------------- 历史案卷 ----------------
function renderHistory(list) {
  if (!list.length) {
    $('history-list').innerHTML = '<div class="empty" style="grid-column:1/-1">暂无历史案卷，先去大厅开一局吧</div>';
    return;
  }
  $('history-list').innerHTML = list.map((c) => `
    <div class="case-card" data-sid="${c.sid}">
      <div class="case-title">${c.title}</div>
      <div class="case-summary">${c.summary || '（无摘要）'}</div>
      <div class="case-meta">
        <span class="sid">#${c.sid.toUpperCase()}</span>
        <span>${c.files.length} 份证物</span>
        <span>${new Date(c.mtime * 1000).toLocaleString()}</span>
      </div>
    </div>`).join('');

  document.querySelectorAll('.case-card').forEach((el) => {
    el.addEventListener('click', () => openHistoryDetail(el.dataset.sid));
  });
}

async function openHistoryDetail(sid) {
  try {
    const j = await api('/api/cases/' + sid);
    const c = j.case;
    if (!c) return;
    openLightbox({ file_id: 3, chapter: 2, kind: 2, img_path: '', desc: '' }, sid);
    setTimeout(() => {
      $('lb-title').textContent = c.title;
      const desc = $('lb-desc');
      if (c.murderer) {
        const b = document.createElement('button');
        b.className = 'btn primary';
        b.style.cssText = 'margin-top:12px;width:100%';
        b.textContent = '🔍 揭示真凶';
        b.onclick = () => { b.textContent = '真凶：' + c.murderer; b.disabled = true; };
        desc.appendChild(b);
      }
    }, 150);
  } catch (e) { toast(e.message, true); }
}

// ---------------- 设置 ----------------
function renderSettings() {
  $('set-nick').value = S.nick;
  $('set-sound').checked = S.sound;
  $('set-colors').innerHTML = COLORS.map((c) =>
    `<div class="color-swatch ${c === S.color ? 'active' : ''}" style="background:${c}" data-c="${c}"></div>`).join('');
  document.querySelectorAll('.color-swatch').forEach((el) => {
    el.addEventListener('click', () => {
      S.color = el.dataset.c;
      document.querySelectorAll('.color-swatch').forEach((x) => x.classList.remove('active'));
      el.classList.add('active');
    });
  });
}
$('btn-settings-save').addEventListener('click', () => {
  S.nick = ($('set-nick').value || '').trim();
  S.sound = $('set-sound').checked;
  localStorage.setItem(LS.nick, S.nick);
  localStorage.setItem(LS.color, S.color);
  localStorage.setItem(LS.sound, S.sound ? '1' : '0');
  toast('设置已保存');
  showView('lobby');
});

// ---------------- 轮询 ui_state.json ----------------
function startPolling() {
  stopPolling();
  S.pollTimer = setInterval(pollNow, 900);
}
function stopPolling() {
  if (S.pollTimer) { clearInterval(S.pollTimer); S.pollTimer = null; }
}
async function pollNow() {
  if (!S.bridge) return;
  try {
    const j = await api(`/api/bridge/${S.bridge}/state`);
    const st = j.state;
    S.running = !!j.running;
    if (!j.running && !st) {
      toast('client_transfer 已退出', true);
      leaveGame();
      return;
    }
    const prevPhase = S.state?.phase;
    S.state = st;
    renderGame();
    const nowPhase = st?.phase ?? 0;
    if (nowPhase === 6 && prevPhase !== 6) {
      beep();
      showResult(sessionHex() ? cases.find((x) => x.sid === sessionHex()) : null);
    }
  } catch (e) {
    toast(e.message, true);
  }
}

// ---------------- 初始化 ----------------
$('lb-close').addEventListener('click', closeLightbox);
document.querySelector('.lightbox-backdrop').addEventListener('click', closeLightbox);
document.querySelectorAll('.modal-backdrop').forEach((b) => b.addEventListener('click', () => {
  b.parentElement.hidden = true;
}));
document.addEventListener('keydown', (e) => {
  if (e.key === 'Escape') { closeLightbox(); document.querySelectorAll('.modal').forEach((m) => m.hidden = true); }
});

async function init() {
  $('host-nick').value = S.nick;
  $('player-nick').value = S.nick;
  await loadCases();
  await loadCapability();
  if (S.bridge) {
    showView('game');
    renderGame();
    startPolling();
    pollNow();
  } else {
    showView('lobby');
  }
}
init();
