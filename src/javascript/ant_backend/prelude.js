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
  if (!g.requestIdleCallback) { g.requestIdleCallback = function (cb) { return 1; }; g.cancelIdleCallback = function () {}; }
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
      var list = {
        media: String(query),
        matches: false,
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
      'whiteSpace','textDecoration','borderRadius','boxShadow','transform'];
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
    Object.defineProperty ? Object.defineProperty(g, 'innerWidth', { get: function () { return g.document && g.document.documentElement ? g.document.documentElement.clientWidth : 0; } }) : (g.innerWidth = 0);
    Object.defineProperty ? Object.defineProperty(g, 'innerHeight', { get: function () { return g.document && g.document.documentElement ? g.document.documentElement.clientHeight : 0; } }) : (g.innerHeight = 0);
    g.outerWidth = g.innerWidth; g.outerHeight = g.innerHeight;
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
      toggle: function (t) { return !!__agbClassOp(el, 'toggle', t); }
    };
  };
  g.window = g; g.self = g; g.top = g; g.parent = g; g.frames = g;
  if (!g.frameElement) g.frameElement = null;
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

  /* Ant's native Worker constructor crashes on anything but a usable script
   * URL (it strdups a NULL) and workers need an environment we do not
   * provide. Hiding the constructors makes pages take their non-worker path
   * instead of killing the process. polyfills.js runs after this file and
   * installs a harmless Worker stub (sites subclass the constructor), so the
   * order matters: hide here, stub later. */
  try { g.Worker = undefined; } catch (e) {}
  try { g.SharedWorker = undefined; } catch (e) {}
  try { g.ServiceWorker = undefined; } catch (e) {}
})(window);