/* ============================================================
 * 侦探工具栏 · 局内
 *  inspect   普通检视（默认，系统光标，无特殊显示）
 *  magnifier 放大镜：启用 magnifier.js 镜片（cursor:none + 真放大）
 *  uv        紫外灯：跟随指针的紫外光圈，显现隐藏痕迹，圈外压暗偏紫
 *  scan      指纹刷：标记全部可探查物（data-ev），青色脉冲高亮
 * 工具栏仅在 #view-game 内生效；回到大厅自动复位为普通检视。
 * ============================================================ */
(function () {
  'use strict';

  var RADIUS = 190;             // 紫外光圈半径(px)
  var tool = 'inspect';
  var rail, overlay;
  var pointer = { x: innerWidth / 2, y: innerHeight / 3 };

  function bodyClasses(on) {
    document.body.classList.toggle('tool-uv', on && tool === 'uv');
    document.body.classList.toggle('tool-scan', on && tool === 'scan');
  }

  function moveOverlay(x, y) {
    pointer.x = x; pointer.y = y;
    if (tool !== 'uv' || !overlay) return;
    overlay.style.background =
      'radial-gradient(circle ' + RADIUS + 'px at ' + x + 'px ' + y + 'px,' +
      'rgba(120,90,255,0) 0%, rgba(120,90,255,.05) 45%,' +
      'rgba(40,20,90,.42) 72%, rgba(16,8,44,.72) 100%)';
  }

  function setTool(next) {
    if (next === tool) return;
    tool = next;

    // 关闭上一个工具的所有效果
    bodyClasses(false);
    if (window.Magnifier && Magnifier.isEnabled && Magnifier.isEnabled()) {
      Magnifier.setEnabled(false);
    }
    if (overlay) overlay.hidden = true;

    if (tool === 'magnifier') {
      if (window.Magnifier) Magnifier.setEnabled(true);
    } else if (tool === 'uv') {
      if (overlay) overlay.hidden = false;
      document.body.classList.add('tool-uv');
      moveOverlay(pointer.x, pointer.y);
    } else if (tool === 'scan') {
      document.body.classList.add('tool-scan');
    }

    rail.querySelectorAll('.tool-btn').forEach(function (b) {
      b.classList.toggle('active', b.dataset.tool === tool);
    });
  }

  /* 游戏视图显隐：离开局内复位为普通检视并清理 */
  function watchView() {
    var game = document.getElementById('view-game');
    if (!game) return;
    var mo = new MutationObserver(function () {
      if (game.hidden && tool !== 'inspect') setTool('inspect');
    });
    mo.observe(game, { attributes: true, attributeFilter: ['hidden'] });
  }

  function init() {
    rail = document.getElementById('tool-rail');
    overlay = document.getElementById('uv-overlay');
    if (!rail) return;

    rail.querySelectorAll('.tool-btn').forEach(function (b) {
      b.addEventListener('click', function () { setTool(b.dataset.tool); });
    });

    window.addEventListener('mousemove', function (e) {
      moveOverlay(e.clientX, e.clientY);
    }, { passive: true });

    watchView();
  }

  if (document.readyState === 'loading') {
    document.addEventListener('DOMContentLoaded', init);
  } else {
    init();
  }
})();
