// 碎片档案 · P2P 协作解谜（网页案情板）
// 网络连接复用 C 核心：server.py 通过命令桥 spawn ./client_transfer，
// 把浏览器操作翻译成 client 的 stdin 命令，再轮询它每次重绘落盘的 ui_state.json。

const $ = (id) => document.getElementById(id);
const LS = { nick: 'fa.nick', color: 'fa.color', sound: 'fa.sound' };
const COLORS = ['#4fc3f7', '#ffb74d', '#81c784', '#ba68c8', '#f06292', '#ff8a65'];
const FILE_NAMES = { 1: '现场勘查图', 2: '证物特写', 3: '案卷正文' };
// 与 include/ui_state.h 对齐：IDLE0 IN_ROOM1 CH1=2 QUIZ=3 CH2=4 CHOICE=5 DONE=6
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
  answerPending: false,
  pollTimer: null,
  downloadPaused: false,
  progress: false,
  progTimer: null,
  progTick: 0,
  progCap: 92,
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

  // 一律优先走证据 API：服务端会去 cases/（及下载缓存）按 sid+file_id 找真实文件。
  // 不能直接用 img_path 拼 /ui_prototype/... —— C 核心给的路径是 session_xx/file_n，
  // 真实文件在项目根 cases/ 下而非 ui_prototype/，拼静态路径必然 404。
  const byApi = sidHex ? `/api/evidence/${sidHex}/${file_id}` : '';
  const byPath = img_path ? ('/ui_prototype/' + img_path) : '';
  const url = byApi || byPath;

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
    // 文本证物没有图：隐藏图像区，否则会残留一个 220px 高空相框块在正文上方
    imgBox.classList.add('text-only');
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
let caseTypes = ['随机'];
async function loadCases() {
  let types;
  try {
    const [cj, tj] = await Promise.all([api('/api/cases'), api('/api/case-types')]);
    cases = cj.cases || [];
    types = tj.types || caseTypes;
  } catch (e) {
    cases = [];
    types = caseTypes;
  }
  caseTypes = types;
  // 案件类型下拉（现场生成用）
  $('case-genre').innerHTML = caseTypes.map((t, i) =>
    `<option value="${i === 0 ? '' : t}">${t}</option>`).join('');
  // 历史案卷下拉（只在有内容时填充）
  const sel = $('case-pick');
  const empty = $('case-pick-empty');
  const prevSid = sel.value;   // 重建会把选中项重置成第一个，先记住用户已选的 sid
  if (cases.length) {
    sel.innerHTML = cases.map((c) =>
      `<option value="${c.sid}">${c.title} · #${c.sid.toUpperCase()}</option>`).join('');
    // 恢复之前的选择（点进入时会再 loadCases，否则用户选的案卷会被冲掉成列表第一个）
    if (prevSid && cases.some((c) => c.sid === prevSid)) sel.value = prevSid;
    sel.hidden = false;
    empty.hidden = true;
  } else {
    sel.innerHTML = '';
    sel.hidden = true;
    empty.hidden = false;
  }
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

// 案件来源切换：现场生成→选类型；历史案卷→选案件（先确认有内容）
$('case-source').addEventListener('click', async (e) => {
  const btn = e.target.closest('.seg-btn');
  if (!btn) return;
  document.querySelectorAll('#case-source .seg-btn').forEach((b) => b.classList.remove('active'));
  btn.classList.add('active');
  const isPick = btn.dataset.src === 'pick';
  $('genre-wrap').hidden = isPick;
  $('case-pick-wrap').hidden = !isPick;
  if (isPick) {
    // 切到历史前先检查统一文件夹里有没有内容
    await loadCases();
  }
});

// 无密钥时先弹确认框；用户选"仍要生成"才继续，选"去配置"跳设置。返回 true=继续，false=放弃
async function confirmNoKey() {
  let hasKey = false;
  try {
    hasKey = (await api('/api/settings/key')).has_api_key;
  } catch (e) { hasKey = true; }   // 状态接口异常时不阻拦，照常尝试
  if (hasKey) return true;
  return new Promise((resolve) => {
    const modal = $('nokey-modal');
    modal.hidden = false;
    const cleanup = () => {
      modal.hidden = true;
      $('btn-nokey-continue').onclick = null;
      $('btn-nokey-settings').onclick = null;
    };
    $('btn-nokey-continue').onclick = () => { cleanup(); resolve(true); };
    $('btn-nokey-settings').onclick = () => { cleanup(); renderSettings(); showView('settings'); resolve(false); };
  });
}

// 现场生成新案：无密钥先提示，确认后显示进度遮罩并轮询直到完成
async function generateAndPick(genre = '') {
  if (!(await confirmNoKey())) return null;
  const genreLabel = genre ? `「${genre}」` : '随机';
  showProgress({
    title: `正在现场生成${genreLabel}案卷…`,
    note: '正在推演案情、编排线索并绘制证物，约需 30~90 秒，请勿关闭页面。',
  });
  let failed = '';
  try {
    await api('/api/generate', { method: 'POST', body: genre ? { genre } : {} });
  } catch (e) { failed = e.message; }
  let sid = '';
  while (!failed) {
    await new Promise((r) => setTimeout(r, 2000));
    try {
      const g = await api('/api/generate');
      if (!g.busy) {
        if (g.error) failed = g.error;
        else sid = g.sid || '';
        break;
      }
    } catch (e) { /* 继续轮询 */ }
  }
  if (failed) {
    hideProgress();
    toast('生成失败：' + failed, true);
    return null;
  }
  await loadCases();
  if (!sid || !cases.some((c) => c.sid === sid)) {
    hideProgress();
    toast('案卷生成成功但列表暂未刷新到，可改从「历史案卷」手动选择', true);
    return null;
  }
  await finishProgress();
  toast('新案已生成');
  return sid;
}

// ---------------- 全局进度遮罩 ----------------
// mode='gen' 现场生成案卷（发生在大厅）；mode='wait' 玩家进房等待清单
function showProgress({ title, note = '', cap = 92 } = {}) {
  S.progress = true;
  S.progCap = cap;
  $('joining-title').textContent = title || '请稍候…';
  const noteEl = $('joining-note');
  if (note) { noteEl.textContent = note; noteEl.hidden = false; }
  else { noteEl.textContent = ''; noteEl.hidden = true; }
  $('joining').hidden = false;
  setFill(6);
  // 进度条平滑爬升、渐近上限：后台确实在干活，但完成时间不固定，故不假装精确百分比
  clearInterval(S.progTimer);
  S.progTick = 6;
  S.progTimer = setInterval(() => {
    if (!S.progress) return;
    const gap = S.progCap - S.progTick;
    S.progTick += Math.max(0.25, gap * 0.03);
    if (S.progTick > S.progCap) S.progTick = S.progCap;
    setFill(S.progTick);
  }, 300);
}
function setFill(pct) {
  $('joining-fill').style.width = Math.min(100, pct) + '%';
}
async function finishProgress() {
  if (!S.progress) return;
  clearInterval(S.progTimer); S.progTimer = null;
  S.progCap = 100; S.progTick = 100;
  setFill(100);
  await new Promise((r) => setTimeout(r, 480));   // 让用户看到“完成”
  hideProgress();
}
function hideProgress() {
  S.progress = false;
  clearInterval(S.progTimer); S.progTimer = null;
  $('joining').hidden = true;
}
// 玩家等待清单：依据真实 ui_state，清单到达（有文件）即结束
function syncProgress(st) {
  if (!S.progress) return;
  if ((st?.files || []).length > 0) finishProgress();
}

async function startBridge(payload) {
  const isPlayer = payload.role === 'player';
  // 房主自带案卷，单人即可开局、无需等待；玩家需要等房主分发证物清单
  if (isPlayer) {
    showProgress({
      title: '正在连接对局…',
      note: '已启动 C 核心并连接房间，正在等待房主分发证物清单。',
      cap: 70,
    });
  }
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
      sid = await generateAndPick($('case-genre').value || '');
      if (!sid) return;
    } else {
      await loadCases();
      if (!cases.length) { toast('案件文件夹里暂无历史案卷，请先现场生成', true); return; }
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

$('btn-history').addEventListener('click', async () => {
  await loadCases();
  if (!cases.length) toast('案件文件夹里还没有案卷');
  showView('history');
});
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
  hideProgress();
  S.bridge = ''; S.role = ''; S.state = null; S.prevPhase = null; S.running = false; S.downloadPaused = false;
  S.answerPending = false;
  $('board').dataset.sig = '';
  quizDraft = '';
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

function renderStepbar(phase) {
  document.querySelectorAll('#stepbar .step').forEach((el) => {
    const p = Number(el.dataset.p);
    el.classList.toggle('done', phase >= p);
    el.classList.toggle('current', phase === p);
  });
}

function renderGame() {
  if (!S.bridge) return;
  syncMeta();
  renderConnBar();
  const st = S.state;
  const phase = st?.phase ?? 0;
  renderStepbar(phase);

  // 案件标题：优先从案件库匹配 session
  const sidHex = sessionHex();
  const c = sidHex ? cases.find((x) => x.sid === sidHex) : null;
  $('game-title').textContent = c ? c.title : (st && sidHex ? ('案件 #' + sidHex.toUpperCase()) : '案件');

  renderBoard(st, sidHex);
  renderMembers(st);
  renderEvents(st);
  renderActions(st, phase, c);
}

function boardSignature(files) {
  return files.map((f) => f.file_id + ':' + (f.have ? 1 : 0) + ':' + f.kind + ':' + f.size).join('|');
}
function renderBoard(st, sidHex) {
  const board = $('board');
  const files = (st && st.files) || [];
  if (!files.length) {
    if (board.dataset.sig !== 'empty') {
      board.dataset.sig = 'empty';
      board.innerHTML = '<div class="empty">' +
        (st ? '等待证物清单（C 核心分发中）…' : '正在启动 client_transfer…') + '</div>';
    }
    return;
  }
  // 卡片集合（含归属/大小）未变化时复用现有 DOM：轮询不再每 900ms 重建卡片，
  // 悬停与位移状态保持稳定，避免节点被替换导致的事件失配。
  const sig = boardSignature(files);
  if (board.dataset.sig === sig) return;
  board.dataset.sig = sig;
  board.innerHTML = '';
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
    if (st.choice_correct === 1) {
      // 指认正确：停留，显示真凶，只给“离开”退出房间
      const truth = st.truth_name || c?.murderer || '（见 logs/choice_summary.log）';
      wrap.innerHTML = `
        <span class="conn"><span class="dot on"></span>指认成功 · 真凶：${truth}</span>
        <span class="spacer"></span>
        ${leave}`;
      $('btn-exit').addEventListener('click', leaveGame);
      return;
    }
    wrap.innerHTML = `
      <span class="conn"><span class="dot on"></span>三项择一 · 指认真凶</span>
      <span class="spacer"></span>
      <button class="btn primary" id="btn-choice">我有答案了</button>
      ${leave}`;
    $('btn-choice').addEventListener('click', openChoice);
    $('btn-exit').addEventListener('click', leaveGame);
    return;
  }

  if (phase === 4) { // CH2 第二章：阅读第二章证物后推进到指认
    wrap.innerHTML = `
      <span class="conn"><span class="dot on"></span>第二章 · 阅读证物后推进指认</span>
      <span class="spacer"></span>
      <button class="btn primary" id="btn-next">推进指认</button>
      ${leave}`;
    $('btn-next').addEventListener('click', async () => { await sendCmd('next'); });
    $('btn-exit').addEventListener('click', leaveGame);
    return;
  }

  if (phase === 3) { // QUIZ 章间问答
    if (st.answer_correct === 1) {
      // 答对后停留：点击“进入第二章”才推进
      wrap.innerHTML = `
        <span class="conn"><span class="dot on"></span>章间问答 · 回答正确</span>
        <span class="spacer"></span>
        <button class="btn primary" id="btn-next">进入第二章</button>
        ${leave}`;
      $('btn-next').addEventListener('click', async () => { await sendCmd('next'); });
      $('btn-exit').addEventListener('click', leaveGame);
      return;
    }
    const ansState = st.answer_correct === 0 ? '回答不符，可重答' : '回答正确后进入第二章';
    const quizLabel = st.answer_correct === 0 ? '重新提交问答' : '提交问答';
    wrap.innerHTML = `
      <span class="conn"><span class="dot on"></span>章间问答 · ${ansState}</span>
      <span class="spacer"></span>
      <button class="btn primary" id="btn-quiz">${quizLabel}</button>
      ${leave}`;
    $('btn-quiz').addEventListener('click', openQuiz);
    $('btn-exit').addEventListener('click', leaveGame);
    return;
  }

  // phase 0(IDLE) / 1(IN_ROOM) / 2(CH1)；QUIZ(3)/CH2(4)/CHOICE(5)/DONE(6) 已在上方处理
  const inRoom = phase >= 1;
  const getBtn = inRoom ? '<button class="btn" id="btn-get">📥 下载证物</button>' : '';
  const pauseBtn = inRoom
    ? `<button class="btn" id="btn-pause">${S.downloadPaused ? '▶️ 继续下载' : '⏸ 暂停下载'}</button>` : '';
  const probeBtn = inRoom ? '<button class="btn" id="btn-probe">📡 重新探测</button>' : '';
  const announceBtn = (inRoom && S.role === 'host')
    ? '<button class="btn" id="btn-announce">📢 关房通告</button>' : '';
  // CH1(phase2) 集齐三证物后，用 next 进入章间问答 QUIZ(phase3)
  const nextBtn = (phase === 2 && allHave) ? '<button class="btn primary" id="btn-next">进入章间问答</button>' : '';
  const label = phase === 0 ? '正在连接房间…'
    : !hasFiles ? '已入房 · 等待证物清单'
    : allHave ? '证物已集齐 · 可推进阶段'
    : '交换证物 · 集齐后推进';
  wrap.innerHTML = `
    <span class="conn"><span class="dot ${S.running ? 'on' : 'off'}"></span>${label}</span>
    <span class="spacer"></span>
    ${probeBtn}${announceBtn}${pauseBtn}${getBtn}${nextBtn}${leave}`;
  const g = $('btn-get');
  if (g) g.addEventListener('click', async () => {
    toast('已发送下载请求（C 核心 P2P 拉取中）…');
    S.downloadPaused = false;
    await sendCmd('get');
    renderActions(st, phase, c);
  });
  const ps = $('btn-pause');
  if (ps) ps.addEventListener('click', async () => {
    if (S.downloadPaused) {
      S.downloadPaused = false;
      toast('继续下载…');
      await sendCmd('get');
    } else {
      S.downloadPaused = true;
      toast('已暂停下载');
      await sendCmd('pause');
    }
    renderActions(st, phase, c);
  });
  const pb = $('btn-probe');
  if (pb) pb.addEventListener('click', async () => {
    toast('已发送 PROBE，重新登记反射并更新路径…');
    await sendCmd('probe');
  });
  const an = $('btn-announce');
  if (an) an.addEventListener('click', async () => {
    toast('已广播关房 ANNOUNCE，未进房玩家可据此索取清单');
    await sendCmd('announce');
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
let quizDraft = '';
function openQuiz() {
  $('quiz-answer').value = quizDraft;
  $('quiz-modal').hidden = false;
}
$('btn-quiz-cancel').addEventListener('click', () => {
  quizDraft = $('quiz-answer').value;
  $('quiz-modal').hidden = true;
});
$('btn-quiz-submit').addEventListener('click', async () => {
  const a = ($('quiz-answer').value || '').trim();
  if (!a) { toast('请先填写回答', true); return; }
  quizDraft = a;
  $('quiz-modal').hidden = true;
  const ok = await sendCmd('answer ' + a);
  if (ok) { S.answerPending = true; pollNow(); }   // 立即拉取判定：正确→进第二章 / 错误→停留并提示重答
});

// ---------------- 结算 ----------------
function showResult(c) {
  const st = S.state;
  const choiceOk = st?.choice_correct === 1;
  const answerOk = st?.answer_correct === 1;
  const truth = st?.truth_name || c?.murderer || '（见 logs/choice_summary.log）';
  $('result-title').textContent = choiceOk ? '指认成功' : '指认失败';
  $('result-body').innerHTML = `
    <div class="result-truth ${choiceOk ? 'win' : 'lose'}">${choiceOk ? '✓ 你指对了真凶' : '✗ 真凶另有其人'}</div>
    <div class="result-truth" style="margin-top:8px">真凶：${truth}</div>
    <div class="result-list" style="margin-top:12px">
      <div class="result-row"><span class="rn">章间问答</span>
      <span class="rc">${answerOk ? '回答正确' : (st?.answer_correct === 0 ? '回答不符' : '未作答')}</span></div>
      <div class="result-row"><span class="rn">指认记录</span>
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
let keyKnown = { has_api_key: false };   // 最近一次后端返回的密钥状态

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
  loadKeyIntoForm();   // 密钥区异步加载（不阻塞其余设置项）
}

async function loadKeyIntoForm() {
  const state = $('key-state');
  const keyInput = $('set-ark-key');
  const modelInput = $('set-ark-model');
  const llmInput = $('set-ark-llm');
  keyInput.value = modelInput.value = llmInput.value = '';   // 明文不回填，只留占位
  keyInput.placeholder = 'ark-xxxxxxxx（留空则用程序化生成，不影响游玩）';
  try {
    const st = await api('/api/settings/key');
    keyKnown = st;
    state.textContent = st.has_api_key ? `已配置 ${st.fields.ARK_API_KEY || ''}` : '未配置';
    state.className = 'key-state ' + (st.has_api_key ? 'ok' : 'warn');
    keyInput.placeholder = st.has_api_key
      ? `已保存 ${st.fields.ARK_API_KEY || ''}（如需更换请输入新值，留空保持不变）`
      : 'ark-xxxxxxxx（留空则用程序化生成，不影响游玩）';
    if (st.fields.ARK_MODEL) modelInput.placeholder = `已保存 ${st.fields.ARK_MODEL}（留空保持不变）`;
    if (st.fields.ARK_LLM_MODEL) llmInput.placeholder = `已保存 ${st.fields.ARK_LLM_MODEL}（留空保持不变）`;
  } catch (e) {
    state.textContent = '状态读取失败';
    state.className = 'key-state warn';
  }
}

$('btn-settings-save').addEventListener('click', async () => {
  S.nick = ($('set-nick').value || '').trim();
  S.sound = $('set-sound').checked;
  localStorage.setItem(LS.nick, S.nick);
  localStorage.setItem(LS.color, S.color);
  localStorage.setItem(LS.sound, S.sound ? '1' : '0');
  const apiKey = $('set-ark-key').value.trim();
  const model = $('set-ark-model').value.trim();
  const llm = $('set-ark-llm').value.trim();
  // 只有"用户实际输入了内容"才提交；留空的字段不发送 → 后端保留原值不变
  const payload = {};
  if (apiKey) payload.ark_api_key = apiKey;
  if (model) payload.ark_model = model;
  if (llm) payload.ark_llm_model = llm;
  if (Object.keys(payload).length) {
    try {
      keyKnown = await api('/api/settings/key', { method: 'POST', body: payload });
    } catch (e) {
      toast('密钥保存失败：' + e.message, true);
      return;
    }
  }
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
    const prevAns = S.state?.answer_correct;
    const pending = S.answerPending;
    S.state = st;
    syncProgress(st);
    renderGame();
    const nowPhase = st?.phase ?? 0;
    // 判错：仍停在 QUIZ(3)。首次 0 跳变 或 本次刚点提交（连续答错也算），都提示可重答
    if (nowPhase === 3 && st.answer_correct === 0 && (pending || prevAns !== 0)) {
      toast('回答不符，再核对线索后重新提交', true);
    }
    // 判对：仍停在 QUIZ(3)，answer_correct 翻成 1；提示并等用户点“进入第二章”
    if (nowPhase === 3 && st.answer_correct === 1 && (pending || prevAns !== 1)) {
      beep();
      toast('回答正确，可进入第二章');
    }
    if (pending) S.answerPending = false;
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

// 大厅案板卡片“上下位移” + 1 秒响应冷却：
// 鼠标移入后停留满 1s 才上浮（加 .lift），中途移出则取消；移出立即复位。
// 事件委托在大厅桌面 .desk-plane 上，对桌上的纸质卡片 .paper.paper--form 生效。
(function setupBoardLift() {
  const desk = document.querySelector('.desk-plane');
  const T = '_liftTimer';

  // 鼠标在卡内移动（跨过子元素）会反复触发 mouseover；只在“从卡外进入”时启动计时
  desk.addEventListener('mouseover', (e) => {
    const card = e.target.closest('.paper.paper--form');
    if (!card) return;
    if (card.contains(e.relatedTarget)) return;   // 仍在同一张卡内，不重置
    if (card[T]) return;                           // 已在等待 / 已上浮
    card[T] = setTimeout(() => {
      card.classList.add('lift');
      card[T] = null;
    }, 400);
  });

  // 真正离开这张卡：取消计时并立即复位
  desk.addEventListener('mouseout', (e) => {
    const card = e.target.closest('.paper.paper--form');
    if (!card || card.contains(e.relatedTarget)) return;
    if (card[T]) { clearTimeout(card[T]); card[T] = null; }
    card.classList.remove('lift');
  });
})();

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
