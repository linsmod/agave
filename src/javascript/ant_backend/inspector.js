/*
 * inspector.js - hover element inspector for the agave Win32 shell.
 *
 * Injected from the shell (right-click menu / "=" menu -> 注入 inspector.js).
 * It is deliberately plain ES5 and dependency-free: it has to run on the
 * engine's own DOM, and it is evaluated as one script, not loaded as a module.
 *
 * What it does:
 *   - follows the mouse, finds the element under the cursor
 *   - draws an absolutely positioned outline + a semi-transparent info panel
 *     (position:absolute, z-index max, appended under <html>) so it never takes
 *     part in the document flow and never shifts the page
 *   - Esc removes it; injecting it again toggles it off
 *
 * Engine notes (do not "fix" these without checking the engine):
 *   - getBoundingClientRect() returns DOCUMENT coordinates here, not viewport
 *     coordinates (RenderObject::absoluteBoundingBoxRect), so boxes are placed
 *     directly and scroll for free.
 *   - pointer-events is not honoured on HTML boxes, so the overlay would eat
 *     the hover. elementFromPoint() is therefore called with the overlay
 *     display:none; the page sees one synchronous style flip, no repaint.
 *   - pageX/pageY exist on mouse events, but window.pageYOffset is hard-coded
 *     to 0 in the prelude, so scroll offsets are read from documentElement.
 */
(function () {
  'use strict';

  var W = window;
  var D = document;
  var KEY = '__agaveInspector';

  /* Re-injection is a toggle: the menu item is cheap to hit twice. */
  if (W[KEY]) {
    W[KEY].destroy();
    return;
  }

  var ID = '__agave_inspector__';
  var Z = '2147483647';
  var st = {
    root: null,
    box: null,
    tip: null,
    line1: null,
    line2: null,
    line3: null,
    element: null,
    pending: false,
    cursorX: 0,
    cursorY: 0,
    lastX: -1,
    lastY: -1,
    /* Scroll offset derived from the event itself (pageX - clientX). The
     * prelude's pageYOffset is hard-coded to 0 and documentElement.scrollTop
     * reads 0 even after the FrameView scrolled, so the event delta is the
     * only reliable source. */
    scrollLeft: 0,
    scrollTop: 0,
    haveScroll: false
  };

  function setStyle(el, props) {
    for (var k in props) {
      if (Object.prototype.hasOwnProperty.call(props, k)) {
        try { el.style[k] = props[k]; } catch (e) {}
      }
    }
  }

  function make(tag, props) {
    var el = D.createElement(tag);
    if (props) setStyle(el, props);
    return el;
  }

  function setText(el, s) {
    var v = String(s == null ? '' : s);
    if (el.textContent !== v) el.textContent = v;
  }

  function build() {
    var root = make('div', {
      position: 'absolute',
      left: '0px', top: '0px', width: '0px', height: '0px',
      margin: '0px', padding: '0px', border: '0px',
      backgroundColor: 'transparent',
      zIndex: Z, display: 'block', overflow: 'visible',
      pointerEvents: 'none'
    });
    root.id = ID;
    root.setAttribute('data-agave-inspector', '1');

    /* Outline. The 2px border sits outside the element's border box, which
     * also makes very small targets (1x1 tracking pixels) visible. */
    var box = make('div', {
      position: 'absolute', left: '0px', top: '0px',
      width: '0px', height: '0px',
      border: '2px solid rgba(56, 189, 248, 0.95)',
      backgroundColor: 'rgba(56, 189, 248, 0.16)',
      boxSizing: 'content-box',
      zIndex: Z, display: 'none'
    });

    var tip = make('div', {
      position: 'absolute', left: '0px', top: '0px',
      padding: '5px 8px',
      border: '1px solid rgba(56, 189, 248, 0.55)',
      backgroundColor: 'rgba(15, 23, 42, 0.86)',
      color: '#e2e8f0',
      fontSize: '11px',
      fontFamily: 'Consolas, "Courier New", monospace',
      lineHeight: '1.5',
      whiteSpace: 'nowrap',
      borderRadius: '4px',
      zIndex: Z, display: 'none'
    });

    var line1 = make('div', { color: '#7dd3fc' });
    var line2 = make('div', { color: '#e2e8f0' });
    var line3 = make('div', { color: '#94a3b8', display: 'none' });

    tip.appendChild(line1);
    tip.appendChild(line2);
    tip.appendChild(line3);
    root.appendChild(box);
    root.appendChild(tip);
    (D.documentElement || D.body).appendChild(root);

    /* Force layout once so the first getBoundingClientRect() below is real:
     * the bridge's getBoundingClientRect() does not update layout itself, but
     * Element::offsetWidth() does. */
    void root.offsetWidth;

    st.root = root;
    st.box = box;
    st.tip = tip;
    st.line1 = line1;
    st.line2 = line2;
    st.line3 = line3;
  }

  function scrollX() {
    var de = D.documentElement;
    var body = D.body;
    return (de && de.scrollLeft) || (body && body.scrollLeft) || 0;
  }

  function scrollY() {
    var de = D.documentElement;
    var body = D.body;
    return (de && de.scrollTop) || (body && body.scrollTop) || 0;
  }

  /* The element under the cursor with our own overlay out of the way. The
   * overlay is hidden and restored inside one synchronous task, so nothing is
   * painted in between. */
  function hitTest(clientX, clientY) {
    var root = st.root;
    var previous = root.style.display;
    var el = null;

    root.style.display = 'none';
    try {
      el = D.elementFromPoint(clientX, clientY);
    } catch (e) {
      el = null;
    }
    root.style.display = previous || 'block';

    while (el && el.nodeType !== 1) el = el.parentNode;
    return el || null;
  }

  function describe(el) {
    var name = String(el.tagName || '?').toLowerCase();
    var classes = '';
    var truncated = false;

    if (el.id) name += '#' + el.id;

    try { classes = el.getAttribute('class') || ''; } catch (e) { classes = ''; }
    if (classes) {
      var parts = String(classes).split(/\s+/);
      var shown = 0;
      for (var i = 0; i < parts.length; ++i) {
        if (!parts[i]) continue;
        if (shown < 3) {
          name += '.' + parts[i];
        } else {
          truncated = true;
        }
        ++shown;
      }
    }

    if (truncated) name += '...';
    if (name.length > 78) name = name.slice(0, 75) + '...';
    return name;
  }

  function describeStyle(el) {
    var out = [];
    try {
      var cs = W.getComputedStyle(el, null);
      var display = cs.getPropertyValue('display');
      var position = cs.getPropertyValue('position');
      if (display) out.push('display:' + display);
      if (position) out.push('position:' + position);
    } catch (e) {}
    return out.join('   ');
  }

  function place(clientX, clientY, rect) {
    /* Root is positioned at (0,0) inside its containing block, which is not
     * necessarily the document origin (body margin, positioned ancestor), so
     * subtract where the root actually landed. */
    var anchor = st.root.getBoundingClientRect();
    var ox = anchor.left;
    var oy = anchor.top;

    /* Shift by the border width: the outline is a content-box div with a 2px
     * border, so without the shift it grows down/right instead of ringing the
     * element evenly. */
    setStyle(st.box, {
      left: (rect.left - ox - 2) + 'px',
      top: (rect.top - oy - 2) + 'px',
      width: Math.max(0, rect.width) + 'px',
      height: Math.max(0, rect.height) + 'px',
      display: 'block'
    });

    var sx = st.haveScroll ? st.scrollLeft : scrollX();
    var sy = st.haveScroll ? st.scrollTop : scrollY();
    var vw = W.innerWidth || 0;
    var vh = W.innerHeight || 0;

    /* The panel may be display:none from the previous hide(); make it
     * measurable before deciding where it goes, otherwise the first placement
     * uses a 0x0 size and can overflow the viewport edge. */
    st.tip.style.display = 'block';
    var tw = st.tip.offsetWidth;
    var th = st.tip.offsetHeight;
    var px = clientX + sx + 16;
    var py = clientY + sy + 18;

    if (vw && px + tw > sx + vw - 8) px = clientX + sx - tw - 16;
    if (vh && py + th > sy + vh - 8) py = clientY + sy - th - 18;
    if (px < sx + 4) px = sx + 4;
    if (py < sy + 4) py = sy + 4;

    setStyle(st.tip, {
      left: (px - ox) + 'px',
      top: (py - oy) + 'px',
      display: 'block'
    });
  }

  function show(el, clientX, clientY) {
    var rect;

    st.element = el;
    try { rect = el.getBoundingClientRect(); } catch (e) { rect = null; }
    if (!rect) {
      hide();
      return;
    }

    setText(st.line1, describe(el));
    setText(st.line2, Math.round(rect.width) + ' x ' + Math.round(rect.height) +
            '  at ' + Math.round(rect.left) + ', ' + Math.round(rect.top));

    var extra = describeStyle(el);
    if (extra) {
      setText(st.line3, extra);
      st.line3.style.display = 'block';
    } else {
      st.line3.style.display = 'none';
    }

    place(clientX, clientY, rect);
  }

  function hide() {
    st.element = null;
    if (st.box) st.box.style.display = 'none';
    if (st.tip) st.tip.style.display = 'none';
  }

  function update(clientX, clientY) {
    var el;

    if (!st.root) return;

    el = hitTest(clientX, clientY);
    if (!el) {
      hide();
      return;
    }

    /* Defensive: hitTest hides the overlay, so this should be unreachable,
     * but a page can move our nodes around. */
    for (var n = el; n; n = n.parentNode) {
      if (n === st.root) {
        hide();
        return;
      }
    }

    show(el, clientX, clientY);
  }

  function onMouseMove(ev) {
    if (!ev || !st.root) return;

    var x = ev.clientX;
    var y = ev.clientY;

    if (x === st.lastX && y === st.lastY) return;
    st.lastX = x;
    st.lastY = y;
    st.cursorX = x;
    st.cursorY = y;

    /* The engine reports page coordinates (contents-space) too; the delta is
     * the real scroll offset, which the window properties do not expose. */
    if (typeof ev.pageX === 'number' && typeof ev.pageY === 'number') {
      st.scrollLeft = ev.pageX - x;
      st.scrollTop = ev.pageY - y;
      st.haveScroll = true;
    }

    if (st.pending) return;
    st.pending = true;

    /* Coalesce bursts: one hit test per frame is plenty, and the engine's
     * hit test plus style flip is the expensive part. */
    W.setTimeout(function () {
      st.pending = false;
      if (st.root) update(st.cursorX, st.cursorY);
    }, 16);
  }

  function onKeyDown(ev) {
    var key = ev ? String(ev.key || '') : '';
    var code = ev ? ev.keyCode : 0;

    if (key === 'Escape' || code === 27) {
      if (W[KEY]) W[KEY].destroy();
    }
  }

  function destroy() {
    try { D.removeEventListener('mousemove', onMouseMove, true); } catch (e) {}
    try { D.removeEventListener('keydown', onKeyDown, true); } catch (e) {}
    if (st.root && st.root.parentNode) st.root.parentNode.removeChild(st.root);
    st.root = null;
    st.box = null;
    st.tip = null;
    st.element = null;
    W[KEY] = null;
  }

  build();

  W[KEY] = {
    version: 1,
    /* Headless/automation hook: client coordinates, same as a mousemove. */
    inspectAt: function (x, y) {
      update(Number(x) || 0, Number(y) || 0);
      return st.element;
    },
    hide: hide,
    destroy: destroy,
    element: function () { return st.element; },
    root: function () { return st.root; }
  };

  D.addEventListener('mousemove', onMouseMove, true);
  D.addEventListener('keydown', onKeyDown, true);

  if (W.console && W.console.log) {
    W.console.log('[inspector] hover to inspect, Esc to remove');
  }
}());
