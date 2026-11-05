(function(g){
  if (typeof g.Image === 'undefined' && g.document) {
    g.Image = function Image() { return g.document.createElement('img'); };
  }
  g.__agbDescribe = function (e) {
    try {
      if (e && e.stack) return String(e.stack);
      if (e && e.message) return String(e.name) + ': ' + String(e.message);
      return String(e);
    } catch (_) { return 'unknown error'; }
  };
  if (g.navigator) {
    if (!g.navigator.platform) g.navigator.platform = 'Win32';
    if (!g.navigator.language) g.navigator.language = 'zh-CN';
    if (!g.navigator.appVersion) g.navigator.appVersion = g.navigator.userAgent || '';
    if (!g.navigator.vendor) g.navigator.vendor = '';
    g.navigator.onLine = true;
    g.navigator.cookieEnabled = true;
    if (!g.navigator.appCodeName) g.navigator.appCodeName = 'Mozilla';
    if (!g.navigator.appName) g.navigator.appName = 'Netscape';
    if (!g.navigator.mimeTypes) g.navigator.mimeTypes = [];
    if (!g.navigator.plugins) g.navigator.plugins = [];
    if (!g.navigator.languages) g.navigator.languages = ['zh-CN', 'zh'];
    if (!g.navigator.hardwareConcurrency) g.navigator.hardwareConcurrency = 4;
    if (!g.navigator.maxTouchPoints) g.navigator.maxTouchPoints = 0;
  }
  {
    var _origin = g.location && g.location.origin ? g.location.origin : '';
    if (!_origin) { _origin = (g.location && g.location.protocol ? g.location.protocol : '') + '//' + (g.location && g.location.host ? g.location.host : ''); }
    g.localStorage = {
      getItem: function (k) { var key = _origin + '|' + String(k); return __agbStorageHas(key) ? String(__agbStorageGet(key)) : null; },
      setItem: function (k, v) { __agbStorageSet(_origin + '|' + String(k), String(v)); },
      removeItem: function (k) { __agbStorageDel(_origin + '|' + String(k)); },
      clear: function () { __agbStorageClear(_origin); },
      key: function (i) { var v = __agbStorageKey(_origin, i); return v === undefined ? null : v; },
      get length() { var n = __agbStorageCount(_origin); return n > 0 ? n : 0; }
    };
    var _ssData = {};
    g.sessionStorage = {
      getItem: function (k) { return Object.prototype.hasOwnProperty.call(_ssData, k) ? _ssData[k] : null; },
      setItem: function (k, v) { _ssData[k] = String(v); },
      removeItem: function (k) { delete _ssData[k]; },
      clear: function () { _ssData = {}; },
      key: function (i) { var ks = Object.keys(_ssData); return i < ks.length ? ks[i] : null; },
      get length() { return Object.keys(_ssData).length; }
    };
  }
  if (!g.requestIdleCallback) {
    /* Must actually run the callback: sites schedule non-critical work here
     * (v.qq.com loads its whole top-nav fragment through it). The old stub
     * returned 1 and never called cb, so that work silently never happened. */
    var __agbIdleSeq = 1, __agbIdle = {};
    g.requestIdleCallback = function (cb) {
      var id = __agbIdleSeq++;
      __agbIdle[id] = g.setTimeout(function () {
        delete __agbIdle[id];
        try { cb({ didTimeout: false, timeRemaining: function () { return 50; } }); } catch (e) {}
      }, 1);
      return id;
    };
    g.cancelIdleCallback = function (id) {
      if (__agbIdle[id]) { g.clearTimeout(__agbIdle[id]); delete __agbIdle[id]; }
    };
  }
  if (!g.createImageBitmap) g.createImageBitmap = function () { return null; };
  if (!g.__agbStyleApi) {
    g.__agbStyleApi = {
      setProperty: function (name, value, priority) { this[String(name)] = value; },
      getPropertyValue: function (name) { var v = this[String(name)]; return v === undefined || v === null ? '' : String(v); },
      removeProperty: function (name) { var old = this[String(name)]; this[String(name)] = ''; return old === undefined ? '' : old; },
      item: function (i) { return ''; },
      length: 0
    };
  }
  if (!g.CSS) g.CSS = {};
  /* CSS.supports is provided by polyfills.js: it has to judge whether a value is
     really implemented (a degraded 'display:grid' must not report true) and it
     can be edited without relinking the engine. */
  if (!g.CSS.escape) g.CSS.escape = function (s) { return String(s); };
  if (!g.matchMedia) {
    g.matchMedia = function (query) {
      /* The CSS side evaluates media queries in C++ (MediaQueryEvaluator); the
         JS side needs the same answers because responsive pages branch on
         matchMedia() before they ever read a computed style. Evaluate the
         queries we can answer honestly and leave the rest false. */
      var text = String(query);
      var dark = false;
      try { dark = !!g.__agbPrefersDark(); } catch (e) { dark = false; }
      var vw = 0, vh = 0, dpr = 1;
      try { vw = g.innerWidth | 0; vh = g.innerHeight | 0; } catch (e) {}
      try { if (g.devicePixelRatio > 0) dpr = g.devicePixelRatio; } catch (e) {}

      function evalFeature(raw) {
        var f = raw.replace(/^\s+|\s+$/g, '').toLowerCase();
        var colon = f.indexOf(':');
        var name = (colon < 0 ? f : f.slice(0, colon)).replace(/\s+/g, '');
        var value = colon < 0 ? '' : f.slice(colon + 1).replace(/^\s+|\s+$/g, '');
        if (name === 'prefers-color-scheme')
          return value === 'dark' ? dark : (value === 'light' ? !dark : false);
        if (name === 'prefers-reduced-motion')
          return value === 'no-preference';
        if (name === 'orientation')
          return value === 'portrait' ? vh >= vw : (value === 'landscape' ? vw > vh : false);
        var min = false, max = false, base = name;
        if (base.slice(0, 4) === 'min-') { min = true; base = base.slice(4); }
        else if (base.slice(0, 4) === 'max-') { max = true; base = base.slice(4); }
        if (base === 'resolution') {
          var n = parseFloat(value);
          if (isNaN(n)) return false;
          var unit = value.replace(/^[-0-9.]+/, '');
          var want = unit.indexOf('dpi') === 0 ? n / 96
                   : (unit.indexOf('dpcm') === 0 ? n * 2.54 / 96 : n);
          return min ? dpr >= want : (max ? dpr <= want : Math.abs(dpr - want) < 0.01);
        }
        var actual;
        if (base === 'width' || base === 'device-width') actual = vw;
        else if (base === 'height' || base === 'device-height') actual = vh;
        else return false;
        var wantPx = parseFloat(value);
        if (isNaN(wantPx)) return false;
        return min ? actual >= wantPx : (max ? actual <= wantPx : actual === wantPx);
      }

      function evalOne(q) {
        var s = q.replace(/^\s+|\s+$/g, '').toLowerCase();
        if (!s) return true;
        var negate = false;
        if (s.slice(0, 4) === 'not ') { negate = true; s = s.slice(4); }
        var features = [], typeText = '', depth = 0, start = -1, i;
        for (i = 0; i < s.length; ++i) {
          var c = s.charAt(i);
          if (c === '(') { if (depth === 0) start = i + 1; depth++; }
          else if (c === ')') { depth--; if (depth === 0 && start >= 0) { features.push(s.slice(start, i)); start = -1; } }
          else if (depth === 0) typeText += c;
        }
        var ok = true;
        var types = typeText.replace(/\band\b/g, ' ').replace(/\bonly\b/g, ' ').replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, '');
        if (types) {
          var t = types.split(' ')[0];
          if (t === 'print' || t === 'speech') ok = false;
        }
        for (var k = 0; k < features.length; ++k) {
          if (!evalFeature(features[k])) { ok = false; break; }
        }
        return negate ? !ok : ok;
      }

      var matches = false;
      var parts = text.split(',');
      for (var p = 0; p < parts.length; ++p) {
        if (evalOne(parts[p])) { matches = true; break; }
      }
      var list = {
        media: String(query),
        matches: matches,
        onchange: null,
        addListener: function () {},
        removeListener: function () {},
        addEventListener: function () {},
        removeEventListener: function () {},
        dispatchEvent: function () { return false; }
      };
      return list;
    };
  }
  {
    var __evtHandlers = ['onload','onerror','onunload','onbeforeunload','onresize','onscroll','onhashchange',
      'onpopstate','onmessage','onfocus','onblur','onclick','onmousedown','onmouseup','onmousemove','onkeydown',
      'onkeyup','onkeypress','ondblclick','oncontextmenu','onwheel','ontouchstart','ontouchend','ontouchmove',
      'onreadystatechange','onstorage','onoffline','ononline','onbeforeprint','onafterprint','onpageshow','onpagehide'];
    for (var __i = 0; __i < __evtHandlers.length; ++__i) {
      try { if (typeof g[__evtHandlers[__i]] === 'undefined') g[__evtHandlers[__i]] = null; } catch (e) {}
      /* Assigning `onload = fn` is the old way of adding a handler; bind it to a
         real listener so the event actually runs it. */
      try {
        if (g.addEventListener && Object.defineProperty) {
          (function (name) {
            var slot = null, installed = false;
            /* One listener per event, forever: assigning onload twice must not
               register twice (removeEventListener is not reliable here). The
               dispatcher always calls whatever the current assignment is. */
            Object.defineProperty(g, name, {
              configurable: true, enumerable: true,
              get: function () { return slot; },
              set: function (fn) {
                slot = fn;
                if (!installed && typeof fn === 'function') {
                  installed = true;
                  /* The return value has to come back out: beforeunload's
                     `return message` form is what the engine reads. */
                  try { g.addEventListener(name.slice(2), function (ev) { if (typeof slot === 'function') { try { return slot.call(g, ev); } catch (e) {} } }); } catch (e) {}
                }
              }
            });
          })(__evtHandlers[__i]);
        }
      } catch (e) {}
    }
    try { if (typeof g.name === 'undefined') g.name = ''; } catch (e) {}
  }
  if (!g.getComputedStyle) {
    g.__computedCamel = ['backgroundColor','backgroundImage','backgroundRepeat','backgroundPosition',
      'backgroundSize','backgroundAttachment','backgroundClip','backgroundOrigin','color','width',
      'height','display','position','top','left','right','bottom','marginTop','marginLeft','marginRight',
      'marginBottom','paddingTop','paddingLeft','paddingRight','paddingBottom','borderTopWidth',
      'borderLeftWidth','borderTopColor','borderColor','font','fontStyle','fontVariant','fontStretch',
      'fontSize','fontFamily','fontWeight','lineHeight',
      'opacity','visibility','overflow','zIndex','textAlign','verticalAlign','float','clear','cursor',
      'whiteSpace','textDecoration','borderRadius','boxShadow','transform','boxSizing',
      'gap','rowGap','columnGap','aspectRatio'];
    /* Accessors live on one shared prototype: defining 40 properties per call
     * made every getComputedStyle() cost ~0.57ms; css3test calls it thousands
     * of times, which is what pinned a core. The raw bridge is ~0.003ms. */
    g.__Computed = function (el, pseudo) { this.__el = el; this.__pseudo = pseudo ? String(pseudo) : ''; };
    (function (proto) {
      function camel(n) { return String(n).replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); }); }
      function dash(n) { return String(n).replace(/[A-Z]/g, function (c) { return '-' + c.toLowerCase(); }); }
      var camelCache = {};
      function camelOf(n) { var c = camelCache[n]; if (c === undefined) { c = camel(n); camelCache[n] = c; } return c; }
      proto.getPropertyValue = function (n) {
        var el = this.__el;
        var pseudo = this.__pseudo;
        var name = String(n);
        var v = '';
        try { v = __agbComputedStyle(el, name, pseudo); } catch (e) { v = ''; }
        if (v) return String(v);
        /* A pseudo box has no inline style to fall back on. */
        if (pseudo) return '';
        var st = (el && el.style) ? el.style : null;
        v = st ? st[camelOf(name)] : undefined;
        if (v === undefined || v === null || v === '') {
          if (name === 'width') return el ? (el.offsetWidth + 'px') : '';
          if (name === 'height') return el ? (el.offsetHeight + 'px') : '';
          return '';
        }
        return String(v);
      };
      proto.setProperty = function (n, v) { var el = this.__el; if (el && el.style) el.style[camelOf(n)] = v; };
      proto.removeProperty = function (n) {
        var el = this.__el;
        if (!el || !el.style) return '';
        var c = camelOf(n); var old = el.style[c]; el.style[c] = ''; return old;
      };
      proto.item = function (i) {
        var el = this.__el; var n = 0;
        if (!el || !el.style) return '';
        for (var k in el.style) { if (n === i) return k; n++; }
        return '';
      };
      if (Object.defineProperty) {
        try {
          Object.defineProperty(proto, 'length', { get: function () {
            var el = this.__el; var i = 0;
            if (el && el.style) { for (var k in el.style) i++; }
            return i;
          } });
        } catch (e) {}
      }
      for (var i = 0; i < g.__computedCamel.length; ++i) {
        (function (name) {
          var cssName = dash(name);
          try {
            Object.defineProperty ? Object.defineProperty(proto, name, {
              get: function () { return this.getPropertyValue(cssName); },
              enumerable: true, configurable: true
            }) : (proto[name] = '');
          } catch (e) { proto[name] = ''; }
        })(g.__computedCamel[i]);
      }
    })(g.__Computed.prototype);
    g.getComputedStyle = function (el, pseudo) { return new g.__Computed(el, pseudo); };
  }
  {
    var _ctors = ['Document','HTMLDocument','Element','HTMLElement','HTMLDivElement','Node','NodeList','HTMLCollection','Event','CustomEvent','MouseEvent','KeyboardEvent','DocumentFragment','Text','Comment','Attr','DOMParser','XMLSerializer','MutationObserver','Storage','Location','Navigator','Screen','History','Window','Performance','CSSStyleDeclaration','HTMLUnknownElement','HTMLAnchorElement','HTMLAreaElement','HTMLAudioElement','HTMLBRElement','HTMLBaseElement','HTMLBodyElement','HTMLButtonElement','HTMLCanvasElement','HTMLDListElement','HTMLDataElement','HTMLDataListElement','HTMLDetailsElement','HTMLDialogElement','HTMLEmbedElement','HTMLFieldSetElement','HTMLFontElement','HTMLFormElement','HTMLFrameElement','HTMLFrameSetElement','HTMLHRElement','HTMLHeadElement','HTMLHeadingElement','HTMLHtmlElement','HTMLIFrameElement','HTMLImageElement','HTMLInputElement','HTMLLIElement','HTMLLabelElement','HTMLLegendElement','HTMLLinkElement','HTMLMapElement','HTMLMediaElement','HTMLMenuElement','HTMLMetaElement','HTMLMeterElement','HTMLModElement','HTMLOListElement','HTMLObjectElement','HTMLOptGroupElement','HTMLOptionElement','HTMLOutputElement','HTMLParagraphElement','HTMLParamElement','HTMLPictureElement','HTMLPreElement','HTMLProgressElement','HTMLQuoteElement','HTMLScriptElement','HTMLSelectElement','HTMLSlotElement','HTMLSourceElement','HTMLSpanElement','HTMLStyleElement','HTMLTableCaptionElement','HTMLTableCellElement','HTMLTableColElement','HTMLTableElement','HTMLTableRowElement','HTMLTableSectionElement','HTMLTemplateElement','HTMLTextAreaElement','HTMLTimeElement','HTMLTitleElement','HTMLTrackElement','HTMLUListElement','HTMLVideoElement','SVGElement','SVGSVGElement'];
  if (typeof g.innerWidth === 'undefined') {
    /* Viewport metrics are per FRAME, not per process: an iframe is a small
     * viewport inside the parent's window, and pages size themselves from
     * these values. They used to be plain properties filled in once when this
     * prelude ran (outerWidth = the top-level width), so a child frame read
     * the parent's 1100 and laid its content out for a window it does not
     * have - WeChat's login QR was sized for the parent page and then clipped
     * inside its 200x200 iframe. Getters keep them in step with whichever
     * document is bound. */
    function innerW() { return (g.document && g.document.documentElement) ? g.document.documentElement.clientWidth : 0; }
    function innerH() { return (g.document && g.document.documentElement) ? g.document.documentElement.clientHeight : 0; }
    function defineViewport(name, getter) {
      if (Object.defineProperty) {
        try { Object.defineProperty(g, name, { get: getter, configurable: true }); return; } catch (e) {}
      }
      g[name] = getter();
    }
    defineViewport('innerWidth', innerW);
    defineViewport('innerHeight', innerH);
    /* outerWidth/outerHeight describe the browser window; with no real window
     * chrome here the frame's own viewport is the best answer, and it must
     * follow the bound document. */
    defineViewport('outerWidth', innerW);
    defineViewport('outerHeight', innerH);
    g.pageXOffset = 0; g.pageYOffset = 0; g.scrollX = 0; g.scrollY = 0;
    g.scrollTo = function (x, y) { if (g.document && g.document.documentElement) { g.document.documentElement.scrollLeft = x; g.document.documentElement.scrollTop = y; } };
    g.scrollBy = function (x, y) { g.scrollTo((g.pageXOffset||0)+x, (g.pageYOffset||0)+y); };
  }
  g.__agbSafe = function (fn, label) {
    return function () {
      try { return fn.apply(this, arguments); }
      catch (e) {
        var msg = (g.__agbDescribe ? g.__agbDescribe(e) : String(e));
        if (g.console && g.console.error) g.console.error('uncaught' + (label ? ' (' + label + ')' : '') + ': ' + msg);
      }
    };
  };
  /* Timers run later, possibly while another frame/control is active: capture
     the current document slot and restore it around the callback. */
  g.__agbBind = function (fn, label) {
    var safe = g.__agbSafe(fn, label);
    var slot = (typeof g.__agbFrameSlot === 'function') ? g.__agbFrameSlot() : -1;
    if (slot < 0) return safe;
    return function () {
      var old = g.__agbActivateSlot(slot);
      try { return safe.apply(this, arguments); }
      finally { g.__agbActivateSlot(old); }
    };
  };
  g.open = function (url) {
    var ok = false;
    try { ok = !!__agbOpenWindow(url == null ? '' : String(url)); } catch (e) { ok = false; }
    if (!ok) return null;
    return { closed: false, focus: function () {}, blur: function () {},
      close: function () { this.closed = true; }, postMessage: function () {} };
  };
  g.close = function () { try { __agbCloseWindow(); } catch (e) {} };
  g.agavePostMessage = function (message) {
    try { __agbPostMessage(message == null ? '' : String(message)); } catch (e) {}
  };
  g.alert = function (message) { try { __agbDialog(0, message == null ? '' : String(message), ''); } catch (e) {} };
  g.confirm = function (message) { try { return !!__agbDialog(1, message == null ? '' : String(message), ''); } catch (e) { return false; } };
  g.prompt = function (message, fallback) { try { var r = __agbDialog(2, message == null ? '' : String(message), fallback == null ? '' : String(fallback)); return r === '' || r === null || r === undefined ? null : String(r); } catch (e) { return null; } };
  if (typeof g.setTimeout === 'function' && !g.__agbTimersWrapped) {
    g.__agbTimersWrapped = true;
    var _st = g.setTimeout, _si = g.setInterval;
    g.setTimeout = function (fn, delay) {
      if (typeof fn !== 'function') return _st.apply(g, arguments);
      var rest = Array.prototype.slice.call(arguments, 2);
      return _st.apply(g, [g.__agbBind(fn, 'timeout'), delay].concat(rest));
    };
    if (typeof _si === 'function') {
      g.setInterval = function (fn, delay) {
        if (typeof fn !== 'function') return _si.apply(g, arguments);
        var rest = Array.prototype.slice.call(arguments, 2);
        return _si.apply(g, [g.__agbBind(fn, 'interval'), delay].concat(rest));
      };
    }
  }
  if (!g.performance) { g.performance = {}; }
  if (!g.performance.timing) {
    g.performance.timing = { navigationStart: 0, fetchStart: 0, domainLookupStart: 0, domainLookupEnd: 0,
      connectStart: 0, connectEnd: 0, requestStart: 0, responseStart: 0, responseEnd: 0,
      domLoading: 0, domInteractive: 0, domContentLoadedEventStart: 0, domContentLoadedEventEnd: 0,
      domComplete: 0, loadEventStart: 0, loadEventEnd: 0 };
  }
  if (!g.performance.now) { g.performance.now = function () { return (new Date()).getTime(); }; }
  if (!g.performance.getEntriesByType) { g.performance.getEntriesByType = function () { return []; }; }
  g.__agbMakeEvent = function (type, target) {
    return {
      type: type, target: target, currentTarget: target,
      bubbles: true, cancelable: true, eventPhase: 2, timeStamp: (new Date()).getTime(),
      defaultPrevented: false, returnValue: true,
      __prevented: false, __stopped: false, __stoppedImmediate: false,
      preventDefault: function () { this.__prevented = true; this.defaultPrevented = true; this.returnValue = false; },
      stopPropagation: function () { this.__stopped = true; },
      stopImmediatePropagation: function () { this.__stopped = true; this.__stoppedImmediate = true; }
    };
  };
  g.__agbMakeClassList = function (el) {
    return {
      add: function (t) { __agbClassOp(el, 'add', t); },
      remove: function (t) { __agbClassOp(el, 'remove', t); },
      contains: function (t) { return !!__agbClassOp(el, 'contains', t); },
      toggle: function (t) { return !!__agbClassOp(el, 'toggle', t); },
      replace: function (a, b) {
        if (!__agbClassOp(el, 'contains', a)) return false;
        __agbClassOp(el, 'remove', a);
        __agbClassOp(el, 'add', b);
        return true;
      }
    };
  };
  /* window/self/top/parent/frames/document/length/frameElement are accessors
   * installed by the engine (bindDocument): they resolve against the document
   * bound right now, so a frame gets its own window object. Assigning them
   * here would shadow those accessors with plain properties again. */
  /* window.postMessage: the engine dispatches the message to every "message"
   * listener in the realm (see globalFramePostMessage). targetOrigin is
   * accepted but not enforced yet; the event carries { data, origin, source }. */
  /* img.decode(): the image is either painted or failed, so the promise
   * resolves at once. Pages chain .then() on it before drawing to canvas. */
  g.__agbDecodeImage = function () {
    return (typeof Promise === 'function') ? Promise.resolve()
                                           : { then: function (f) { if (f) f(); return this; }, catch: function () { return this; } };
  };
  /* document.fonts: a stub FontFaceSet. Text measurement is synchronous here
   * (no web font loading), so an always-resolved set is the honest answer and
   * pages that await fonts.ready do not hang. */
  g.__agbFonts = {
    status: 'loaded',
    size: 0,
    ready: (typeof Promise === 'function') ? Promise.resolve() : { then: function (f) { if (f) f(); return this; } },
    check: function () { return true; },
    add: function () { return this; },
    'delete': function () { return true; },
    forEach: function () {},
    load: function () { return g.__agbDecodeImage(); }
  };
  g.postMessage = function (message, targetOrigin) {
    try {
      __agbFramePostMessage(message, targetOrigin == null ? '' : String(targetOrigin));
    } catch (e) {}
  };
  if (typeof g.requestAnimationFrame === 'undefined') {
    g.requestAnimationFrame = function (cb) { return setTimeout(function () { cb((new Date()).getTime()); }, 16); };
    g.cancelAnimationFrame = function (id) { clearTimeout(id); };
  }
  /* Minimal MutationObserver: the generic ctor stub below leaves instances
   * with no methods, and defensive scripts then die on .observe() (deepseek's
   * main.js). A real implementation needs engine DOM-mutation hooks; this one
   * accepts the full API and never fires, which keeps those guards harmless.
   * Must come before the stub loop so it is not overwritten. */
  if (typeof g.MutationObserver === 'undefined') {
    g.MutationObserver = function (cb) { this._cb = cb; };
    g.MutationObserver.prototype.observe = function (target, options) {};
    g.MutationObserver.prototype.disconnect = function () {};
    g.MutationObserver.prototype.takeRecords = function () { return []; };
  }
    for (var _ci = 0; _ci < _ctors.length; _ci++) {
      if (typeof g[_ctors[_ci]] === 'undefined') g[_ctors[_ci]] = function () { return null; };
    }
  }
  /* base64 -> ArrayBuffer. Binary bodies cross the bridge as base64 because
     WebCore::String cannot carry NUL or non-UTF-8 bytes (wasm/zip/images). */
  if (typeof g.__agbB64ToBuffer !== 'function') {
    g.__agbB64ToBuffer = function (b64) {
      var s = String(b64 == null ? '' : b64), len = s.length, i, n = 0, v = 0, bits = 0;
      var table = g.__agbB64Table;
      if (!table) {
        var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
        table = g.__agbB64Table = {};
        for (i = 0; i < chars.length; i++) table[chars.charCodeAt(i)] = i;
      }
      while (len > 0 && s.charCodeAt(len - 1) === 61) len--;   /* strip padding */
      var out = new Uint8Array((len * 3) >> 2);
      for (i = 0; i < len; i++) {
        v = (v << 6) | (table[s.charCodeAt(i)] | 0);
        bits += 6;
        if (bits >= 8) { bits -= 8; out[n++] = (v >> bits) & 0xff; }
      }
      return out.buffer;
    };
  }
  /* UTF-8 text -> base64. Data URLs carry worker scripts across the bridge
     (btoa alone cannot encode non-ASCII source, and a JSON-escaped string
     would double the payload size). */
  if (typeof g.__agbB64EncodeUtf8 !== 'function') {
    g.__agbB64EncodeUtf8 = function (text) {
      var s = String(text === undefined || text === null ? '' : text), i, j, c;
      var bytes;
      try {
        bytes = new TextEncoder().encode(s);
      } catch (e) {
        bytes = [];
        for (i = 0; i < s.length; i++) {
          c = s.charCodeAt(i);
          if (c < 0x80) bytes.push(c);
          else if (c < 0x800) bytes.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f));
          else if (c >= 0xd800 && c < 0xdc00 && i + 1 < s.length) {
            var lo = s.charCodeAt(++i);
            var cp = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
            bytes.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f),
                       0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f));
          } else {
            bytes.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f));
          }
        }
      }
      var binary = '';
      for (i = 0; i < bytes.length; i += 0x2000) {
        var chunk = [];
        for (j = i; j < bytes.length && j < i + 0x2000; j++) chunk.push(bytes[j]);
        binary += String.fromCharCode.apply(null, chunk);
      }
      return btoa(binary);
    };
  }
  if (typeof g.__agbDecodeUtf8 !== 'function') {
    g.__agbDecodeUtf8 = function (buffer) {
      if (!buffer) return '';
      try { return new TextDecoder('utf-8').decode(new Uint8Array(buffer)); } catch (e) {}
      var b = new Uint8Array(buffer), s = '', i = 0;
      while (i < b.length) {
        var c = b[i++];
        if (c < 0x80) { s += String.fromCharCode(c); continue; }
        if (c >= 0xc0 && c < 0xe0 && i < b.length) { s += String.fromCharCode(((c & 0x1f) << 6) | (b[i++] & 0x3f)); continue; }
        if (c >= 0xe0 && c < 0xf0 && i + 1 < b.length) { s += String.fromCharCode(((c & 0x0f) << 12) | ((b[i] & 0x3f) << 6) | (b[i + 1] & 0x3f)); i += 2; continue; }
        s += '\ufffd';
      }
      return s;
    };
  }
  if (typeof g.__agbResolveUrl !== 'function') {
    g.__agbResolveUrl = function (u) {
      var s = String(u == null ? '' : u);
      if (!s || /^[a-zA-Z][a-zA-Z0-9+.\-]*:/.test(s)) return s;
      var base = (g.document && g.document.baseURI) ? String(g.document.baseURI)
               : (g.location && g.location.href ? String(g.location.href) : '');
      if (!base || !/^[a-zA-Z][a-zA-Z0-9+.\-]*:\/\//.test(base)) return s;
      var origin = base.replace(/^([a-zA-Z][a-zA-Z0-9+.\-]*:\/\/[^\/]*).*$/, '$1');
      if (s.charAt(0) === '#') return base.split('#')[0] + s;
      /* Protocol-relative (`//v.qq.com/x`): keep the page's scheme. It used to
       * fall into the single-slash branch and became
       * `https://v.qq.com//v.qq.com/x`, so v.qq's header slice 404'd. */
      if (s.substring(0, 2) === '//') {
        var scheme = base.match(/^([a-zA-Z][a-zA-Z0-9+.\-]*):/);
        return (scheme ? scheme[1] + ':' : '') + s;
      }
      if (s.charAt(0) === '/') return origin + s;
      var path = base.split('#')[0].split('?')[0];
      path = path.substring(0, path.lastIndexOf('/') + 1);
      return path + s;
    };
  }
  if (typeof g.XMLHttpRequest === 'undefined') {
    var XHR = function XMLHttpRequest() {
      this.readyState = 0; this.status = 0; this.statusText = ''; this.responseText = ''; this.response = '';
      this.responseType = ''; this.responseURL = ''; this.timeout = 0;
      this.onreadystatechange = null; this.onload = null; this.onerror = null; this.onloadend = null; this.onabort = null; this.onprogress = null;
      this._url = null; this._method = 'GET'; this._async = true; this._headers = {}; this._buffer = null;
    };
    XHR.prototype.open = function (method, url, async) {
      this._method = method || 'GET'; this._url = url; this._async = (async !== false);
      this.readyState = 1; this._fire();
    };
    XHR.prototype.setRequestHeader = function (k, v) { this._headers[k] = v; };
    XHR.prototype.getAllResponseHeaders = function () { return ''; };
    XHR.prototype.getResponseHeader = function () { return null; };
    XHR.prototype.abort = function () { this.readyState = 0; if (this.onabort) this.onabort.call(this, this); };
    XHR.prototype.overrideMimeType = function () {};
    XHR.prototype.addEventListener = function (type, fn) { this['_ev_' + type] = fn; };
    XHR.prototype.removeEventListener = function (type) { this['_ev_' + type] = null; };
    XHR.prototype._fire = function (type) {
      if (this.onreadystatechange) this.onreadystatechange.call(this, this);
      if (type) {
        var h = this['on' + type] || this['_ev_' + type];
        if (h) h.call(this, this);
      }
    };
    XHR.prototype._finish = function (status, payload, binary) {
      this.status = status;
      this.statusText = status ? 'OK' : '';
      if (binary) {
        this._buffer = g.__agbB64ToBuffer(payload);
        this.responseText = '';
        if (this.responseType === 'blob') {
          try { this.response = new Blob([this._buffer]); } catch (e) { this.response = this._buffer; }
        } else {
          this.response = this._buffer;
        }
      } else {
        this.responseText = (payload === undefined || payload === null) ? '' : String(payload);
        if (this.responseType === 'json') {
          try { this.response = JSON.parse(this.responseText); } catch (e) { this.response = null; }
        } else if (this.responseType === 'blob') {
          try { this.response = new Blob([this.responseText]); } catch (e) { this.response = this.responseText; }
        } else {
          this.response = this.responseText;
        }
      }
      this.readyState = 4;
      this._fire();
      if (!status) this._fire('error');
      else this._fire('load');
      this._fire('loadend');
    };
    XHR.prototype.send = function (body) {
      var self = this;
      var target = g.__agbResolveUrl(this._url);
      this.responseURL = target;
      if (body !== undefined && body !== null) this._body = body;
      var payload = (this._body === undefined || this._body === null) ? '' : String(this._body);
      var ctype = this._headers['Content-Type'] || this._headers['content-type'] || '';
      var method = String(this._method || 'GET').toUpperCase();
      var wantBinary = this.responseType === 'arraybuffer' || this.responseType === 'blob';
      if (!this._async) {
        if (wantBinary) {
          var b64 = null;
          try { b64 = __agbHttpGetBin(target); } catch (e) { b64 = null; }
          this._finish((b64 === undefined || b64 === null) ? 0 : 200, b64, true);
        } else {
          var text = null;
          try { text = __agbHttpGet(target); } catch (e) { text = null; }
          this._finish((text === undefined || text === null) ? 0 : 200, text, false);
        }
        return;
      }
      /* Async: the C side owns the request and delivers (status, body) on a
       * later frame, so the UI keeps running. */
      /* Everything goes through the full request entry: pages POST JSON to
         auth/RPC/fingerprint endpoints, and a GET without body made those
         fail and retry in a loop. */
      __agbHttpRequestEx(target, method, ctype, payload, function (status, b64) {
        self._finish(status, b64, true);
      });
    };
    g.XMLHttpRequest = XHR;
  }
  /* fetch(): ant's own implementation expects its CLI network stack, so back it
   * with our async binary request; a buffered Response answers arrayBuffer()
   * and blob() which wasm/zip loaders need. Relative URLs resolve against the
   * document base (fetch('app.wasm') used to hit the network verbatim). */
  if (typeof Promise === 'function') {
    g.fetch = function (input, init) {
      var url = g.__agbResolveUrl((typeof input === 'string') ? input : (input && input.url));
      var opts = init || {};
      var method = String(opts.method || 'GET').toUpperCase();
      var ctype = '';
      if (opts.headers) {
        if (typeof opts.headers.get === 'function') {
          ctype = opts.headers.get('content-type') || '';
        } else {
          ctype = opts.headers['Content-Type'] || opts.headers['content-type'] || '';
        }
      }
      var payload = (opts.body === undefined || opts.body === null) ? '' : String(opts.body);
      return new Promise(function (resolve, reject) {
        __agbHttpRequestEx(url, method, ctype, payload, function (status, b64) {
          if (!status) { reject(new Error('network error: ' + url)); return; }
          var buffer = g.__agbB64ToBuffer(b64);
          var response = {
            ok: status >= 200 && status < 300,
            status: status,
            statusText: (status >= 200 && status < 300) ? 'OK' : '',
            url: url, redirected: false, type: 'basic',
            headers: {
              get: function () { return null; },
              has: function () { return false; },
              forEach: function () {},
              entries: function () { return []; }
            },
            arrayBuffer: function () { return Promise.resolve(buffer); },
            blob: function () {
              try { return Promise.resolve(new Blob([buffer])); } catch (e) { return Promise.resolve(buffer); }
            },
            text: function () { return Promise.resolve(g.__agbDecodeUtf8(buffer)); },
            json: function () { return response.text().then(function (t) { return JSON.parse(t); }); },
            clone: function () { return response; }
          };
          resolve(response);
        });
      });
    };
  }
  /* instantiateStreaming/compileStreaming expect a streamed Response; ours is
   * buffered, so wrap them: fetch -> arrayBuffer -> instantiate. */
  if (typeof WebAssembly === 'object' && WebAssembly) {
    WebAssembly.instantiateStreaming = function (source, imports) {
      return Promise.resolve(source).then(function (r) {
        return (r && typeof r.arrayBuffer === 'function') ? r.arrayBuffer() : r;
      }).then(function (buf) {
        return Promise.resolve(WebAssembly.instantiate(buf, imports));
      });
    };
    WebAssembly.compileStreaming = function (source) {
      return Promise.resolve(source).then(function (r) {
        return (r && typeof r.arrayBuffer === 'function') ? r.arrayBuffer() : r;
      }).then(function (buf) {
        return Promise.resolve(WebAssembly.compile(buf));
      });
    };
  }
  if (typeof g.define === 'undefined') {
    var mods = {};
    var def = function define(name, deps, factory) {
      if (typeof name !== 'string') { factory = deps; deps = name; name = null; }
      if (!(deps && typeof deps.length === 'number')) { factory = deps; deps = []; }
      var args = [], i;
      for (i = 0; i < deps.length; i++) args.push(mods[deps[i]] || {});
      var exports = {};
      var r = (typeof factory === 'function') ? factory.apply(exports, args) : exports;
      var mod = (r === undefined) ? exports : r;
      if (name) mods[name] = mod;
      return mod;
    };
    def.amd = {};
  /* SVG viewBox: an object whose fields read and write the attribute. */
  g.__agbViewBox = function (el) {
    function parts() {
      var s = String(el.getAttribute('viewBox') || '0 0 0 0').split(/[\s,]+/);
      var out = [], i, n;
      for (i = 0; i < 4; i++) { n = parseFloat(s[i]); out.push(isNaN(n) ? 0 : n); }
      return out;
    }
    function setField(i, value) {
      var a = parts(), n = Number(value);
      a[i] = isNaN(n) ? 0 : n;
      el.setAttribute('viewBox', a.join(' '));
    }
    var rect = {}, names = ['x', 'y', 'width', 'height'], i;
    function make(i) {
      Object.defineProperty(rect, names[i], {
        enumerable: true, configurable: true,
        get: function () { return parts()[i]; },
        set: function (v) { setField(i, v); }
      });
    }
    for (i = 0; i < 4; i++) make(i);
    return { baseVal: rect, animVal: rect };
  };
    g.define = def;
    if (typeof g.require === 'undefined') {
      g.require = function require(deps, cb) {
        if (typeof deps === 'string') return mods[deps] || {};
        var args = [], i;
        if (deps) for (i = 0; i < deps.length; i++) args.push(mods[deps[i]] || {});
        if (cb) cb.apply(null, args);
      };
    }
  }

  /* ---- Web Workers --------------------------------------------------------
   * ant's Worker is real: its own thread, its own isolate, structured-clone
   * messaging, error/close events. What it cannot do is load a script over
   * the network - worker_resolve_entry only accepts data:, blob: and file:
   * URLs, while every page on the web starts its workers from http(s). So:
   *
   *   data:/blob:/file:  the native constructor, untouched (the script is
   *                      local already, so construction stays synchronous)
   *   http(s)/relative    fetched here through the async bridge, then handed
   *                      to the native constructor as a data: URL carrying a
   *                      preamble that restores what the data: entry loses:
   *                      location, origin and importScripts
   *
   * The constructor never blocks on the network. postMessage() calls made
   * before the script is in place are queued and flushed in order, which is
   * what a browser does while the worker's event loop is starting.
   */
  var _agbNativeWorker = (typeof g.Worker === 'function') ? g.Worker : null;
  var _agbWorkerSeq = 0;
  var _agbWorkerPending = {};

  function _agbWorkerJsStr(v) {
    return JSON.stringify(String(v === undefined || v === null ? '' : v));
  }

  /* The bits of URL the worker scope needs; parsed here because the worker
   * only ever sees the data: URL we hand it. */
  function _agbWorkerParseUrl(u) {
    var s = String(u === undefined || u === null ? '' : u);
    var m = /^([a-zA-Z][a-zA-Z0-9+.-]*:)\/\/([^\/?#]*)([^?#]*)(\?[^#]*)?(#.*)?$/.exec(s);
    var out = { href: s, protocol: '', host: '', hostname: '', port: '',
                pathname: '', search: '', hash: '', origin: '' };
    if (!m) {
      out.pathname = s;
      return out;
    }
    out.protocol = m[1];
    out.host = m[2];
    out.pathname = m[3] || '/';
    out.search = m[4] || '';
    out.hash = m[5] || '';
    var hp = m[2].split(':');
    out.hostname = hp[0];
    out.port = hp.length > 1 ? hp[1] : '';
    out.origin = m[1] + '//' + m[2];
    return out;
  }

  /* Source string evaluated inside the worker before the page's script. */
  function _agbWorkerPreamble(entryUrl, name, imports) {
    var p = _agbWorkerParseUrl(entryUrl);
    var table = imports || {};
    var lines = [
      ';(function (S) {',
      '  var ENTRY = ' + _agbWorkerJsStr(entryUrl) + ';',
      '  var ORIGIN = ' + _agbWorkerJsStr(p.origin) + ';',
      '  var LOC = ' + JSON.stringify(p) + ';',
      '  var IMPORTS = ' + JSON.stringify(table) + ';',
      '  function agbAbs(u) {',
      '    if (/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(u)) return u;',
      '    if (u.charAt(0) === "/") return ORIGIN + u;',
      '    var b = ENTRY.split("#")[0].split("?")[0];',
      '    return b.substring(0, b.lastIndexOf("/") + 1) + u;',
      '  }',
      '  LOC.toString = function () { return this.href; };',
      '  S.location = LOC;',
      '  S.origin = ORIGIN;',
      '  if (!S.name) S.name = ' + _agbWorkerJsStr(name) + ';',
      /* importScripts() is synchronous by spec, so the scripts it can reach
       * are fetched by the page before the worker starts and evaluated from
       * this table. Anything not in it cannot be loaded here at all. */
      '  S.importScripts = function () {',
      '    for (var i = 0; i < arguments.length; i++) {',
      '      var u = agbAbs(String(arguments[i]));',
      '      var src = IMPORTS[u];',
      '      if (src === undefined)',
      '        throw new Error("importScripts: cannot load \'" + u + "\' (not prefetched)");',
      '      (0, eval)(src);',
      '    }',
      '  };',
      '})(self);',
      ''
    ];
    return lines.join('\n');
  }

  /* importScripts('literal') arguments in a script, in source order. */
  function _agbWorkerScanImports(src) {
    var out = [], m, text = String(src || '');
    var call = /importScripts\s*\(([^)]*)\)/g;
    while ((m = call.exec(text))) {
      var args = m[1], lit = /['"]([^'"]*)['"]/g, one;
      while ((one = lit.exec(args))) {
        if (one[1]) out.push(one[1]);
      }
    }
    return out;
  }

  /* Fetches the entry's importScripts() targets so the preamble can hand them
   * to the worker synchronously. Two levels deep is enough for the classic
   * scripts that still use importScripts (bundlers inline everything). */
  function _agbWorkerPrefetch(entryUrl, src, done) {
    var p = _agbWorkerParseUrl(entryUrl);
    var imports = {}, queue = [], seen = {}, qi = 0, inflight = 0, finished = false;

    function absolute(u) {
      var s = String(u || '');
      if (/^[a-zA-Z][a-zA-Z0-9+.-]*:/.test(s)) return s;
      if (s.charAt(0) === '/') return p.origin + s;
      var base = entryUrl.split('#')[0].split('?')[0];
      return base.substring(0, base.lastIndexOf('/') + 1) + s;
    }

    function enqueue(list, depth) {
      for (var i = 0; i < list.length; i++) {
        var abs = absolute(list[i]);
        if (seen[abs]) continue;
        seen[abs] = true;
        queue.push([abs, depth]);
      }
    }

    function pump() {
      while (qi < queue.length) {
        var item = queue[qi++];
        inflight++;
        (function (abs, depth) {
          __agbHttpRequestEx(abs, 'GET', '', '', function (status, b64) {
            inflight--;
            /* Only a 2xx body is a script: an error page is valid HTML and
             * would otherwise be evaluated as JavaScript. */
            if (status >= 200 && status < 300) {
              var text = '';
              try { text = g.__agbDecodeUtf8(g.__agbB64ToBuffer(b64)); } catch (e) { text = ''; }
              imports[abs] = text;
              if (depth < 2) enqueue(_agbWorkerScanImports(text), depth + 1);
            }
            pump();
          });
        })(item[0], item[1]);
      }
      if (inflight === 0 && qi >= queue.length && !finished) {
        finished = true;
        done(imports);
      }
    }

    enqueue(_agbWorkerScanImports(src), 1);
    pump();
  }

  function _agbWorkerEvent(w, type, native) {
    var ev = g.__agbMakeEvent(type, w);
    ev.data = native ? native.data : undefined;
    ev.origin = (native && native.origin) ? native.origin : '';
    ev.lastEventId = '';
    ev.source = null;
    ev.ports = (native && native.ports) || [];
    if (native) {
      if (native.message !== undefined) ev.message = native.message;
      if (native.filename !== undefined) ev.filename = native.filename;
      if (native.lineno !== undefined) ev.lineno = native.lineno;
      if (native.colno !== undefined) ev.colno = native.colno;
      if (native.error !== undefined) ev.error = native.error;
    }
    return ev;
  }

  function _agbWorkerReport(e) {
    /* Listener exceptions surface as page errors instead of vanishing. */
    setTimeout(function () { throw e; }, 0);
  }

  function _agbWorkerFail(w, message) {
    if (w._failed || w._terminated) return;
    w._failed = true;
    try { if (g.console && g.console.error) g.console.error('Worker error: ' + message); } catch (e) {}
    var ev = g.__agbMakeEvent('error', w);
    ev.message = String(message);
    ev.filename = w._url;
    ev.lineno = 0;
    ev.colno = 0;
    ev.error = null;
    w._dispatch(ev);
  }

  function _agbWorkerAttach(w) {
    var n = w._native;
    if (!n) return;
    n.onmessage = function (e) { w._dispatch(_agbWorkerEvent(w, 'message', e)); };
    n.onmessageerror = function (e) { w._dispatch(_agbWorkerEvent(w, 'messageerror', e)); };
    n.onerror = function (e) {
      var ev = _agbWorkerEvent(w, 'error', e);
      ev.cancelable = true;
      var handled = w._dispatch(ev);
      /* Returning true from onerror (or preventDefault()) suppresses the
       * default report, exactly like the browser's error handling. */
      if (handled === true && typeof ev.preventDefault === 'function') ev.preventDefault();
    };
  }

  function _agbWorkerFlush(w) {
    var queued = w._queue;
    w._queue = [];
    for (var i = 0; i < queued.length; i++) {
      try { w._native.postMessage(queued[i][0], queued[i][1]); } catch (e) {}
    }
  }

  function _agbWorkerStart(w, target, options) {
    if (!_agbNativeWorker) {
      _agbWorkerFail(w, 'Worker is not available in this build');
      return;
    }
    var name = (options && typeof options.name === 'string') ? options.name : '';

    /* Already-local script: let ant start it directly. */
    if (/^(data|blob|file):/i.test(target)) {
      w._native = new _agbNativeWorker(target, options || undefined);
      _agbWorkerAttach(w);
      _agbWorkerFlush(w);
      return;
    }

    var id = ++_agbWorkerSeq;
    w._id = id;
    _agbWorkerPending[id] = w;

    __agbHttpRequestEx(target, 'GET', '', '', function (status, b64) {
      if (w._terminated || _agbWorkerPending[id] !== w) return;
      delete _agbWorkerPending[id];
      w._id = 0;
      if (status < 200 || status >= 300) {
        _agbWorkerFail(w, 'Failed to load worker script: ' + target + ' (' + status + ')');
        return;
      }
      var src;
      try { src = g.__agbDecodeUtf8(g.__agbB64ToBuffer(b64)); }
      catch (e) { _agbWorkerFail(w, 'Cannot decode worker script: ' + target); return; }

      _agbWorkerPrefetch(target, src, function (imports) {
        if (w._terminated || w._native) return;
        var dataUrl = 'data:text/javascript;base64,' +
          g.__agbB64EncodeUtf8(_agbWorkerPreamble(target, name, imports) + src);
        try {
          w._native = new _agbNativeWorker(dataUrl, options || undefined);
        } catch (e) {
          _agbWorkerFail(w, String((e && e.message) || e));
          return;
        }
        _agbWorkerAttach(w);
        _agbWorkerFlush(w);
      });
    });
  }

  function _agbWorkerDispatch(w, ev) {
    if (!ev || !ev.type) return undefined;
    ev.target = w;
    ev.currentTarget = w;
    var handler = w['on' + ev.type], result;
    if (typeof handler === 'function') {
      try { result = handler.call(w, ev); } catch (e) { _agbWorkerReport(e); }
    }
    var list = (w._listeners[ev.type] || []).slice();
    for (var i = 0; i < list.length; i++) {
      try {
        if (typeof list[i] === 'function') list[i].call(w, ev);
        else if (list[i] && typeof list[i].handleEvent === 'function') list[i].handleEvent(ev);
      } catch (e) { _agbWorkerReport(e); }
    }
    return result;
  }

  function Worker(scriptURL, options) {
    if (!(this instanceof Worker))
      throw new TypeError("Failed to construct 'Worker': use the 'new' operator");
    if (scriptURL === undefined)
      throw new TypeError("Failed to construct 'Worker': 1 argument required, but only 0 present");
    var raw = String(scriptURL);
    var target = g.__agbResolveUrl(raw);
    if (!target)
      throw new SyntaxError("Failed to construct 'Worker': invalid URL '" + raw + "'");

    this.onmessage = null;
    this.onmessageerror = null;
    this.onerror = null;
    this._listeners = {};
    this._queue = [];
    this._native = null;
    this._terminated = false;
    this._failed = false;
    this._id = 0;
    this._url = target;

    try { _agbWorkerStart(this, target, options); }
    catch (e) { _agbWorkerFail(this, String((e && e.message) || e)); }
  }

  Worker.prototype.constructor = Worker;
  Worker.prototype._dispatch = function (ev) { return _agbWorkerDispatch(this, ev); };

  Worker.prototype.postMessage = function (message, transfer) {
    if (this._terminated) return;
    if (!this._native) {
      this._queue.push([message, transfer]);
      return;
    }
    this._native.postMessage(message, transfer);
  };

  Worker.prototype.terminate = function () {
    this._terminated = true;
    this._queue.length = 0;
    if (this._id && _agbWorkerPending[this._id]) delete _agbWorkerPending[this._id];
    this._id = 0;
    if (this._native) this._native.terminate();
  };

  Worker.prototype.addEventListener = function (type, listener) {
    if (typeof listener !== 'function' &&
        !(listener && typeof listener.handleEvent === 'function')) return;
    var list = this._listeners[type] || (this._listeners[type] = []);
    for (var i = 0; i < list.length; i++) if (list[i] === listener) return;
    list.push(listener);
  };

  Worker.prototype.removeEventListener = function (type, listener) {
    var list = this._listeners[type];
    if (!list) return;
    for (var i = 0; i < list.length; i++) {
      if (list[i] === listener) { list.splice(i, 1); return; }
    }
  };

  Worker.prototype.dispatchEvent = function (ev) {
    var handled = _agbWorkerDispatch(this, ev);
    return !(ev && ev.defaultPrevented) && handled !== false;
  };

  Worker.prototype.ref = function () { if (this._native) this._native.ref(); };
  Worker.prototype.unref = function () { if (this._native) this._native.unref(); };

  if (Object.defineProperty) {
    Object.defineProperty(Worker.prototype, 'threadId', {
      configurable: true, enumerable: false,
      get: function () { return this._native ? this._native.threadId : -1; }
    });
  }

  if (_agbNativeWorker) {
    try { g.Worker = Worker; } catch (e) {}
  }
  /* Shared/service workers would need a second host protocol (shared scopes,
   * precache, install/fetch events). Leaving them undefined makes pages take
   * their non-worker path instead of half-working. */
  try { g.SharedWorker = undefined; } catch (e) {}
  try { g.ServiceWorker = undefined; } catch (e) {}

  /* `value` accessor on the input/textarea/select interface prototypes.
   *
   * React's input value tracker (react-dom/src/client/inputValueTracking) does
   *   Object.getOwnPropertyDescriptor(node.constructor.prototype, 'value')
   * and only installs _valueTracker when that returns a real {get, set}
   * descriptor. The engine's interface prototypes have no `value` entry, so
   * the tracker never existed and updateValueIfChanged() answered false for
   * every keystroke: the controlled-input loop (input event -> onChange ->
   * setState -> value write-back) never ran, and sites that filter input -
   * DeepSeek's phone field rewrites the value down to digits - accepted every
   * character. Reading back through the wrapper still goes through the
   * per-wrapper catch-all property (own properties win over the prototype),
   * so normal `el.value` reads/writes are unchanged; only the explicit
   * descriptor path React uses picks these functions up. */
  if (g.Object && g.Object.defineProperty) {
    var _agbValueIfaces = ['HTMLInputElement', 'HTMLTextAreaElement', 'HTMLSelectElement'];
    for (var _agbVii = 0; _agbVii < _agbValueIfaces.length; _agbVii++) {
      try {
        var _agbCtor = g[_agbValueIfaces[_agbVii]];
        var _agbProto = _agbCtor && _agbCtor.prototype;
        if (_agbProto && !g.Object.getOwnPropertyDescriptor(_agbProto, 'value')) {
          /* The guard matters: a plain `this.value` inside the accessor
           * recurses into itself whenever `this` is the prototype (a site
           * assigning HTMLInputElement.prototype.value, or anything reading
           * it off the prototype) and hangs the page mid-boot. With the guard
           * the prototype-level access answers undefined/ignores, while
           * wrappers resolve `this.value` through their own catch-all before
           * the prototype is ever consulted. */
          (function (proto) {
            /* The implementation must not read `this.value`: React re-exports
             * this very descriptor as an own property on the wrapper, so a
             * self-referencing accessor would recurse forever. Route through
             * the bridge instead, which reads/writes the DOM element
             * directly. */
            g.Object.defineProperty(proto, 'value', {
              configurable: true, enumerable: true,
              get: function () {
                try { return g.__agbInputValue(this); } catch (e) { return undefined; }
              },
              set: function (v) {
                try { g.__agbInputValue(this, v); } catch (e) {}
              }
            });
          })(_agbProto);
        }
      } catch (e) {}
    }
  }

  /* Vite's legacy plugin decides between the modern and the legacy bundle in
   * an inline module: `import.meta.url, import("_").catch(...),
   * async function*(){}().next(), !file:&&(__vite_is_modern_browser=true)`.
   * When the dynamic import suspends, the flag lands after the fallback script
   * already ran, the legacy bundle loads and dies on SystemJS, and the client
   * app never mounts (v.qq.com: empty top nav). This engine does support modern
   * modules, so answer the probe up front; the page's own script can only agree. */
  if (typeof g.__vite_is_modern_browser === 'undefined')
    g.__vite_is_modern_browser = true;

})(window);
