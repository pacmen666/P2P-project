/* ============================================================
 * 放大镜光标 · 第一人称侦探工作台
 *  - cursor:none，放大镜跟随指针
 *  - 镜片内克隆整个 #app 做 scale(2) 实时放大（真放大，非 hover）
 *  - 悬停 [data-ev]：镜框点亮、浮签显示 编号/名称/说明、物件发光
 *  - 悬停输入框/下拉：自动让开并恢复文本光标
 * ============================================================ */
(function () {
  'use strict';

  var SCALE = 2;                 // 放大倍数
  var SIZE = 236;                // 放大镜整体尺寸
  var OFFSET_X = -46;            // 镜片中心相对指针偏移
  var OFFSET_Y = -92;
  var HOVER_MARGIN = 60;         // 指针距视口边缘多近时翻转浮签

  var app, mag, lens, glass, desk, tag, clone;
  var pointer = { x: innerWidth / 2, y: innerHeight / 3, inside: false };
  var rebuildTimer = null;
  var lastTarget = null;
  var noGlass = false;
  // mode: 'lens' 完整放大镜；'tag' 仅提示浮签（大厅）；'off' 关闭（局内默认，不常驻）
  var mode = 'lens';
  var lensEnabled = false;      // 局内是否由工具栏开启了放大镜
  var LOBBY_ID = 'view-lobby';

  function activeView() {
    var v = app && app.querySelector('.view:not([hidden])');
    return v ? v.id : null;
  }

  function hideLens() {
    mag.classList.remove('is-show', 'is-hot', 'is-field');
    document.body.classList.remove('mag-on');
    noGlass = false;
    if (clone) { clone.parentNode.removeChild(clone); clone = null; }
    clearLit();
  }

  /* 统一切换模式：tag=大厅提示板；lens=完整放大镜；off=局内关闭（默认，不常驻） */
  function setMode(want) {
    if (want === mode) return;
    mode = want;
    if (want === 'lens') {
      mag.classList.remove('is-tag-only');
      rebuildClone();
      if (pointer.inside) {
        mag.classList.add('is-show');
        document.body.classList.add('mag-on');
      }
    } else {
      hideLens();
      if (want === 'tag') {
        mag.classList.add('is-tag-only');
      } else {
        mag.classList.remove('is-tag-only');
      }
    }
  }

  /* 视图变化：大厅→提示板；局内→默认关闭，是否启用放大镜由工具栏决定 */
  function applyMode() {
    if (activeView() === LOBBY_ID) { setMode('tag'); return; }
    setMode(lensEnabled ? 'lens' : 'off');
  }

  /* 工具栏调用：开启/关闭放大镜镜片；返回是否已开启 */
  function setEnabled(on) {
    lensEnabled = !!on;
    if (activeView() !== LOBBY_ID) setMode(lensEnabled ? 'lens' : 'off');
    return lensEnabled;
  }
  function isEnabled() { return lensEnabled; }

  /* ---------- 构建放大镜 DOM ---------- */
  function buildMag() {
    mag = document.createElement('div');
    mag.id = 'magnifier';
    mag.className = 'mag';
    mag.innerHTML =
      '<div class="mag-tag" id="magTag">' +
        '<span class="mt-no" id="magNo">EVIDENCE</span>' +
        '<span class="mt-name" id="magName">移动放大镜检视案卷</span>' +
        '<span class="mt-meta" id="magMeta">悬停在证物与文件上可查看线索</span>' +
        '<i class="mt-pin"></i>' +
      '</div>' +
      '<div class="mag-grip">' +
        '<span class="mag-neck"></span>' +
        '<span class="mag-handle"></span>' +
      '</div>' +
      '<div class="mag-lens" id="magLens">' +
        '<div class="mag-desk" id="magDesk"></div>' +
        '<div class="mag-glass" id="magGlass"></div>' +
        '<span class="mag-rim"></span>' +
        '<span class="mag-glare"></span>' +
      '</div>';
    document.body.appendChild(mag);

    lens = mag.querySelector('#magLens');
    glass = mag.querySelector('#magGlass');
    desk = mag.querySelector('#magDesk');
    tag = mag.querySelector('#magTag');
  }

  var rect = { left: 0, top: 0, width: 0, height: 0 };

  /* ---------- 测量真实 #app，给克隆设一致宽高（保证内部布局对齐） ---------- */
  function measure() {
    if (!app) return;
    rect = app.getBoundingClientRect();
    if (clone) {
      clone.style.width = rect.width + 'px';
      clone.style.height = rect.height + 'px';
    }
  }

  /* ---------- （重新）构建放大克隆 ---------- */
  function rebuildClone() {
    if (!app || !glass) return;
    var fresh = app.cloneNode(true);
    fresh.id = 'mag-clone';
    fresh.removeAttribute('hidden');
    fresh.classList.add('mag-clone');
    // 真实页面里 hidden 的视图，克隆里也必须隐藏（否则多视图堆叠导致错位发黑）
    var hidden = app.querySelectorAll('[hidden]');
    for (var i = 0; i < hidden.length; i++) {
      if (hidden[i].id) {
        var inClone = fresh.querySelector('#' + CSS.escape(hidden[i].id));
        if (inClone) inClone.setAttribute('hidden', '');
      }
    }
    if (clone && clone.parentNode) clone.parentNode.removeChild(clone);
    glass.innerHTML = '';
    glass.appendChild(fresh);
    clone = fresh;
    measure();
    updateClone();
  }

  function scheduleRebuild() {
    if (rebuildTimer) clearTimeout(rebuildTimer);
    rebuildTimer = setTimeout(rebuildClone, 180);
  }

  /* ---------- 根据指针更新克隆位移（核心放大对齐） ---------- */
  function updateClone() {
    if (!clone) return;
    // 镜片中心在视口中的坐标
    var lensX = pointer.x + OFFSET_X;
    var lensY = pointer.y + OFFSET_Y;
    // 换算为相对 #app 左上的坐标
    var cx = lensX - rect.left;
    var cy = lensY - rect.top;
    clone.style.transform =
      'translate(' + (SIZE / 2 - cx * SCALE) + 'px,' +
      (SIZE / 2 - cy * SCALE) + 'px) scale(' + SCALE + ')';
  }

  /* ---------- 定位放大镜（left/top，保证内部 fixed 背景对齐视口） ---------- */
  function magLeft() { return Math.round(pointer.x - SIZE / 2 + OFFSET_X); }
  function magTop() { return Math.round(pointer.y - SIZE / 2 + OFFSET_Y); }

  function positionMag() {
    var ml = magLeft(), mt = magTop();
    mag.style.left = ml + 'px';
    mag.style.top = mt + 'px';
    // desk 原点对齐视口原点（lens 内边距 30）
    desk.style.left = -(ml + 30) + 'px';
    desk.style.top = -(mt + 30) + 'px';
  }

  /* ---------- 浮签方位翻转 ---------- */
  function orientTag() {
    var flipX = pointer.x > innerWidth - 280;
    var flipY = pointer.y < innerHeight * 0.38;
    tag.classList.toggle('flip-x', flipX);
    tag.classList.toggle('flip-y', flipY);
    tag.classList.remove('left', 'below');
    if (pointer.x < HOVER_MARGIN + 120) tag.classList.add('left');
    if (pointer.y > innerHeight - HOVER_MARGIN - 120) tag.classList.add('below');
  }

  /* ---------- 探查当前指针下的元素 ---------- */
  function isField(el) {
    if (!el) return false;
    var tagName = el.tagName;
    return tagName === 'INPUT' || tagName === 'TEXTAREA' || tagName === 'SELECT';
  }

  function probe() {
    if (mode === 'off') { clearLit(); return; }
    var el = document.elementFromPoint(pointer.x, pointer.y);

    // 输入框 / 下拉：让开放大镜、恢复光标（仅 lens 模式需要隐藏镜片）
    if (isField(el)) {
      if (mode === 'tag') { clearLit(); return; }
      if (!noGlass) {
        noGlass = true;
        mag.classList.add('is-field');
        document.body.classList.remove('mag-on');
      }
      clearLit();
      return;
    }
    if (noGlass && mode === 'lens') {
      noGlass = false;
      mag.classList.remove('is-field');
      document.body.classList.add('mag-on');
    }

    var target = el ? el.closest('[data-ev]') : null;

    if (target !== lastTarget) {
      clearLit();
      lastTarget = target;
      if (target) {
        target.classList.add('is-lit');
        setTag(target.getAttribute('data-ev'),
                target.getAttribute('data-evno'),
                target.getAttribute('data-evmeta'));
        mag.classList.add('is-hot');
      } else {
        setTag(null, null, null);
        mag.classList.remove('is-hot');
      }
    }
  }

  function clearLit() {
    if (lastTarget) lastTarget.classList.remove('is-lit');
    lastTarget = null;
  }

  function setTag(name, no, meta) {
    var elNo = mag.querySelector('#magNo');
    var elName = mag.querySelector('#magName');
    var elMeta = mag.querySelector('#magMeta');
    if (name) {
      elNo.textContent = no || 'EVIDENCE';
      elName.textContent = name;
      elMeta.textContent = meta || '点击查看 / 操作';
      elNo.style.display = 'block';
      elMeta.style.display = meta ? 'block' : 'none';
    } else {
      elNo.textContent = 'DESK';
      elName.textContent = '侦探工作台';
      elMeta.textContent = '移动放大镜检视桌上的文件与案卷';
      elNo.style.display = 'block';
      elMeta.style.display = 'block';
    }
  }

  /* ---------- 指针事件 ---------- */
  function onMove(x, y) {
    pointer.x = x;
    pointer.y = y;
    if (!pointer.inside) {
      pointer.inside = true;
      if (mode === 'tag') {
        mag.classList.add('is-show');
      } else if (!noGlass) {
        mag.classList.add('is-show');
        document.body.classList.add('mag-on');
      }
    }
    if (mode === 'tag') {
      mag.style.left = pointer.x + 'px';
      mag.style.top = pointer.y + 'px';
      orientTag();
      probe();
    } else {
      positionMag();
      updateClone();
      orientTag();
      probe();
    }
  }

  function onLeave() {
    pointer.inside = false;
    mag.classList.remove('is-show', 'is-hot');
    document.body.classList.remove('mag-on');
    clearLit();
  }

  /* ---------- 启动 ---------- */
  function init() {
    app = document.getElementById('app');
    if (!app) return;
    buildMag();
    orientTag();
    setTag(null, null, null);
    applyMode();            // 若初始即在大厅，则只启用提示板

    window.addEventListener('mousemove', function (e) {
      onMove(e.clientX, e.clientY);
    }, { passive: true });

    document.addEventListener('mouseleave', onLeave);
    document.addEventListener('mouseout', function (e) {
      if (!e.relatedTarget) onLeave();
    });
    window.addEventListener('blur', onLeave);
    window.addEventListener('scroll', function () { measure(); updateClone(); }, { passive: true });

    // 视图 hidden 变化 → 切换镜片/提示板模式；游戏区子节点频繁重建 → 节流刷新克隆
    var mo = new MutationObserver(function (muts) {
      var viewChanged = muts.some(function (m) {
        return m.type === 'attributes' &&
               m.target.classList && m.target.classList.contains('view');
      });
      if (viewChanged) applyMode();
      else if (mode === 'lens') scheduleRebuild();
    });
    mo.observe(app, { childList: true, subtree: true,
                      attributes: true, attributeFilter: ['hidden'] });
    window.addEventListener('resize', function () {
      measure();
      orientTag();
      updateClone();
    });
  }

  window.Magnifier = { setEnabled: setEnabled, isEnabled: isEnabled };

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
