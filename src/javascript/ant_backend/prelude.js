(function(g){
  if (typeof g.Image === 'undefined' && g.document) {
    g.Image = function Image() { return g.document.createElement('img'); };
  }
  /* new Audio([src]) is how most players build their element (Howler,
   * video.js, every "play a sound on click" snippet). Without it the global
   * is undefined and those paths throw before touching the DOM. */
  if (typeof g.Audio === 'undefined' && g.document) {
    g.Audio = function Audio(src) {
      var el = g.document.createElement('audio');
      if (src !== undefined && src !== null) el.src = String(src);
      return el;
    };
  }
  g.__agbDescribe = function (e) {
    try {
      if (e && e.stack) return String(e.stack);
      if (e && e.message) return String(e.name) + ': ' + String(e.message);
      return String(e);
    } catch (_) { return 'unknown error'; }
  };
  /* An uncaught exception must never close the browser. ant exits the process
   * for an error thrown from a setImmediate()/queueMicrotask() callback unless
   * the host marked uncaught errors non-fatal; that flag is logged as set and
   * still not honoured on those paths, so one broken async callback in a page
   * took the whole process down (deepseek's React scheduler threw from a
   * setImmediate and the probe exited with code 1). A process-level
   * uncaughtException listener is ant's other supported route: it reports the
   * error and keeps going. The page sees the same console.error line it would
   * in Chrome (an uncaught error, not a crash). */
  (function () {
    var proc = g.process;
    if (!proc || typeof proc.on !== 'function' || typeof proc.emit !== 'function')
      return;
    try {
      proc.on('uncaughtException', function (err, origin) {
        try {
          var msg = (typeof g.__agbDescribe === 'function' ? g.__agbDescribe(err) : String(err));
          if (g.console && g.console.error)
            g.console.error('uncaught' + (origin ? ' (' + origin + ')' : '') + ': ' + msg);
        } catch (_) {}
      });
    } catch (_) {}
  }());
  /* TimeRanges for media.buffered / played / seekable. The engine hands over
   * one range [0, end]; players call `ranges.end(ranges.length - 1)` (and some
   * `range.end(0)` behind a length check), so both methods have to exist or
   * they die with "undefined is not a function". */
  g.__agbTimeRangesFactory = function (end) {
    var e = (typeof end === 'number' && end > 0) ? end : 0;
    return {
      start: function () { return 0; },
      end: function (index) {
        if (index !== 0 || e <= 0)
          throw new RangeError('index out of range');
        return e;
      }
    };
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
    if (!g.navigator.product) g.navigator.product = 'Gecko';
    /* UA sniffers call this unconditionally (ga.js: `b.javaEnabled()`);
     * a missing method throws "undefined is not a function" and kills the
     * rest of the script. No JVM here, so the honest answer is false. */
    if (!g.navigator.javaEnabled) g.navigator.javaEnabled = function () { return false; };
    if (!g.navigator.mimeTypes) g.navigator.mimeTypes = [];
    if (!g.navigator.plugins) g.navigator.plugins = [];
    if (!g.navigator.languages) g.navigator.languages = ['zh-CN', 'zh'];
    if (!g.navigator.hardwareConcurrency) g.navigator.hardwareConcurrency = 4;
    if (!g.navigator.maxTouchPoints) g.navigator.maxTouchPoints = 0;
    /* navigator.clipboard.writeText: "copy to clipboard" buttons on real sites
     * (and inspector.js) go through this; the host primitive writes the Win32
     * clipboard from the engine thread, same as ctrl+c. No permission model —
     * the page can write whenever it wants. */
    if (typeof g.__agbClipboardSetText === 'function' && !g.navigator.clipboard) {
      var _agbClipboardResult = function (ok) {
        if (typeof Promise === 'function')
          return ok ? Promise.resolve() : Promise.reject(new Error('clipboard write failed'));
        return { then: function (f) { if (ok && f) f(); return this; },
                 'catch': function (f) { if (!ok && f) f(); return this; } };
      };
      g.navigator.clipboard = {
        writeText: function (text) {
          var ok = false;
          try { ok = !!g.__agbClipboardSetText(text == null ? '' : String(text)); } catch (e) { ok = false; }
          return _agbClipboardResult(ok);
        },
        write: function () { return _agbClipboardResult(false); }
      };
    }
    /* navigator.mediaCapabilities: Chrome has it on every page; feature
     * detectors read `.encodingInfo`/`.decodingInfo` and call them. Report the
     * codecs as unsupported instead of throwing a TypeError. */
    if (!g.navigator.mediaCapabilities && typeof g.Promise === 'function') {
      var _agbMediaCapInfo = function () {
        return g.Promise.resolve({ supported: false, smooth: false, powerEfficient: false });
      };
      g.navigator.mediaCapabilities = {
        decodingInfo: _agbMediaCapInfo,
        encodingInfo: _agbMediaCapInfo,
        addEventListener: function () {},
        removeEventListener: function () {}
      };
    }
    /* navigator.bluetooth: present in desktop Chrome; the probe/embedder has no
     * radio. getAvailability answers false and requestDevice rejects rather
     * than making `navigator.bluetooth.getAvailability` a TypeError. */
    if (!g.navigator.bluetooth && typeof g.Promise === 'function') {
      g.navigator.bluetooth = {
        getAvailability: function () { return g.Promise.resolve(false); },
        requestDevice: function () {
          return g.Promise.reject(new Error('Bluetooth is not supported in this build'));
        }
      };
    }
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
      /* style[i] / style.item(i): the declaration's property names in order; the numeric read itself is answered by the
       * engine's styleGetter. */
      item: function (i) { var v = this[String(i | 0)]; return v === undefined || v === null ? '' : String(v); },
      length: 0
    };
  }
  /* Constructable stylesheets + adoptedStyleSheets (issues #18 phase D).
   *
   * new CSSStyleSheet() is legal in Chrome (adopted sheets) while the engine's
   * interface constructor throws "Illegal constructor". The wrapper below keeps
   * the interface identity (`instanceof CSSStyleSheet` stays true for the engine's
   * own sheets) and gives the constructed object replaceSync/replace; adopting it
   * on a Document or ShadowRoot injects one <style> element per target, which is
   * what actually applies the rules (there is no style-scoping engine here).
   *
   * Known limit: reading cssRules on a *constructed* sheet hits the native
   * accessor and throws; component code uses replaceSync, not cssRules. */
  if (typeof g.CSSStyleSheet === 'function' && !g.CSSStyleSheet.__agbConstructable) {
    (function () {
      var Native = g.CSSStyleSheet;
      var adopted = new Map();      /* target (document or shadow root) -> { sheets, style } */

      function isSheet(value) {
        return value && value.__agbSheet === true;
      }

      function combinedText(sheets) {
        var text = [], i;
        for (i = 0; i < sheets.length; ++i) text.push(sheets[i].__agbCssText || '');
        return text.join('\n');
      }

      function refresh(target, entry) {
        var isDocument = target && target.nodeType === 9;
        var doc = isDocument ? target : ((target && target.ownerDocument) || g.document);
        /* A document's sheet lives in <head>; a shadow root's <style> is appended
         * to the root, which the facade backs by the host element. */
        var host = isDocument ? (target.head || target.documentElement) : target;
        var text = combinedText(entry.sheets);

        if (!host || !doc) return;
        if (!entry.style) {
          entry.style = doc.createElement('style');
          entry.style.setAttribute('data-adopted', '');
          host.appendChild(entry.style);
        }
        entry.style.textContent = text;
      }

      function entryFor(target) {
        var entry = adopted.get(target);
        if (!entry) { entry = { sheets: [], style: null }; adopted.set(target, entry); }
        return entry;
      }

      function defineAdopted(proto, name) {
        if (!proto) return;
        try {
          Object.defineProperty(proto, 'adoptedStyleSheets', {
            configurable: true,
            enumerable: true,
            get: function () { return entryFor(this).sheets.slice(); },
            set: function (value) {
              var entry = entryFor(this), list = [], i;
              value = value || [];
              for (i = 0; i < value.length; ++i) {
                if (!isSheet(value[i])) {
                  var error = new Error('adoptedStyleSheets accepts constructable CSSStyleSheet objects only');
                  error.name = 'TypeError';
                  throw error;
                }
                list.push(value[i]);
              }
              entry.sheets = list;
              refresh(this, entry);
            }
          });
        } catch (e) { /* read-only prototype in some realm */ }
      }

      function Ctor() {
        var sheet = Object.create(Ctor.prototype);

        sheet.__agbSheet = true;
        sheet.__agbCssText = '';
        sheet.replaceSync = function (text) {
          sheet.__agbCssText = String(text === undefined ? '' : text);
          adopted.forEach(function (entry) {
            if (entry.sheets.indexOf(sheet) >= 0) refreshEntry(sheet, entry);
          });
          return undefined;
        };
        sheet.replace = function (text) {
          sheet.replaceSync(text);
          return Promise.resolve(sheet);
        };
        return sheet;
      }

      /* keep the engine's own sheets instanceof-compatible */
      Ctor.prototype = Native.prototype;
      Ctor.__agbConstructable = true;
      g.CSSStyleSheet = Ctor;

      /* The interface classes may not be exposed yet when the prelude runs (they
       * are per realm and created around the same bind), so install now, again on
       * the next tick, and from attachShadow() (which is where a ShadowRoot first
       * exists). Idempotent: a second defineProperty just replaces the accessor. */
      var installedDocument = null, installedShadow = null;
      function installAdopted() {
        if (g.Document && g.Document.prototype !== installedDocument) {
          installedDocument = g.Document.prototype;
          defineAdopted(installedDocument, 'Document');
        }
        if (g.ShadowRoot && g.ShadowRoot.prototype !== installedShadow) {
          installedShadow = g.ShadowRoot.prototype;
          defineAdopted(installedShadow, 'ShadowRoot');
        }
      }
      installAdopted();
      try { setTimeout(installAdopted, 0); } catch (e) { /* no timers yet */ }
      g.__agbInstallAdopted = installAdopted;

      /* replaceSync needs to re-render every target that adopted the sheet */
      function refreshEntry(sheet, entry) {
        var target = null;
        adopted.forEach(function (value, key) { if (value === entry) target = key; });
        if (target) refresh(target, entry);
      }
    }());
  }

  /* attachShadow (issues #18 phase C): the spec checks live here because they have
   * to throw real error types; the C++ side only records the root and returns the
   * wrapper whose children are the host's real children (no shadow tree in this
   * tree's WebCore, so what a component renders is what layout paints). */
  if (!Element.prototype.attachShadow && typeof g.__agbAttachShadow === 'function') {
    (function () {
      var FORBIDDEN = { a: 1, area: 1, base: 1, br: 1, col: 1, embed: 1, hr: 1,
                        img: 1, input: 1, link: 1, meta: 1, param: 1, source: 1,
                        track: 1, wbr: 1 };
      function domError(name, message) {
        var error = new Error(message);
        error.name = name;
        return error;
      }
      Element.prototype.attachShadow = function (init) {
        if (typeof g.__agbInstallAdopted === 'function') g.__agbInstallAdopted();
        var mode = init && init.mode;
        var tag = String(this.localName || this.tagName || '').toLowerCase();

        /* Validate before creating anything: an invalid mode or a forbidden
         * host must not leave a half-attached root behind. */
        if (mode !== 'open' && mode !== 'closed')
          throw domError('TypeError', '"' + mode + '" is not a valid shadow root mode');
        if (FORBIDDEN[tag])
          throw domError('NotSupportedError', '<' + tag + '> cannot host a shadow root');

        var root = g.__agbAttachShadow(this, mode);

        if (!root)
          throw domError('NotSupportedError',
                         'attachShadow is not supported on <' + tag + '> (or a shadow root is already attached)');
        return root;
      };
    }());
  }

  /* customElements (issues #18 phase A): the registry storage is per document and
   * lives in C++; this object owns the spec validation (name rules, duplicates),
   * whenDefined()'s promise bookkeeping and the upgrade() walk. */
  if (!g.customElements && typeof g.__agbCEDefine === 'function') {
    (function () {
      var pending = {};
      var NAME = /^[a-z][.0-9_a-z]*-[\-.0-9_a-z]*$/;
      var RESERVED = { 'annotation-xml': 1, 'color-profile': 1, 'font-face': 1,
                       'font-face-src': 1, 'font-face-uri': 1, 'font-face-format': 1,
                       'font-face-name': 1, 'missing-glyph': 1 };

      function domError(name, message) {
        var error = new Error(message);
        error.name = name;
        return error;
      }

      function resolvePending(name, ctor) {
        var list = pending[name];
        if (!list) return;
        delete pending[name];
        for (var i = 0; i < list.length; ++i) {
          try { list[i](ctor); } catch (e) { /* consumer's reaction */ }
        }
      }

      g.customElements = {
        define: function (name, ctor, options) {
          if (typeof name !== 'string' || !NAME.test(name) || RESERVED[name])
            throw domError('SyntaxError', '"' + name + '" is not a valid custom element name');
          if (typeof ctor !== 'function')
            throw domError('TypeError', 'The custom element constructor is not a function');
          /* The duplicate rejection lives in C++ (__agbCEDefine returns 0) so the
           * debug trace sees every attempt, including the rejected one. */
          if (!g.__agbCEDefine(name, ctor))
            throw domError('NotSupportedError', 'the name "' + name + '" has already been used');
          resolvePending(name, ctor);
          return undefined;
        },
        get: function (name) {
          var ctor = g.__agbCEGet(String(name));
          return ctor === undefined || ctor === null ? undefined : ctor;
        },
        getName: function (ctor) {
          var name = g.__agbCEGetName(ctor);
          return name === undefined || name === null ? null : name;
        },
        whenDefined: function (name) {
          name = String(name);
          if (g.__agbCEHas(name)) return Promise.resolve(g.__agbCEGet(name));
          return new Promise(function (resolve) {
            (pending[name] || (pending[name] = [])).push(resolve);
          });
        },
        upgrade: function (root) {
          g.__agbCEUpgrade(root || g.document);
        }
      };
    }());
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
        /* Units matter: parseFloat('100rem') is 100, which made
           `(min-width: 100rem)` match every viewport. Media-query rem/em use
           the CSS initial font size (16px, same as Chrome); vw/vh are the
           viewport share; anything else is honest only when px. */
        var m = /^\s*(-?[0-9]*\.?[0-9]+)\s*([a-z%]*)\s*$/.exec(value);
        if (!m) return false;
        var wantPx = parseFloat(m[1]);
        if (isNaN(wantPx)) return false;
        var unit = m[2];
        if (unit === 'rem' || unit === 'em') wantPx *= 16;
        else if (unit === 'vw') wantPx = wantPx * vw / 100;
        else if (unit === 'vh') wantPx = wantPx * vh / 100;
        else if (unit && unit !== 'px') return false;
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
    /* The names come from the generated table the CSS parser is built from
     * (StylePropertyNames.h via __agbStylePropertyNames), so every property the
     * engine knows has an accessor: shorthands (`cs.margin`, `cs.padding`,
     * `cs.border`, `cs.background`) and longhands the old hand-written list of
     * 40 omitted (`cs.minWidth`, `cs.alignSelf`, `cs.webkitAppearance`) used to
     * read back as undefined while Chrome answers with a value. */
    var __computedNames = null;
    try {
      if (typeof g.__agbStylePropertyNames === 'function') __computedNames = g.__agbStylePropertyNames();
    } catch (e) {}
    if (!__computedNames || !__computedNames.length) {
      __computedNames = ['backgroundColor','backgroundImage','backgroundRepeat','backgroundPosition',
        'backgroundSize','backgroundAttachment','backgroundClip','backgroundOrigin','color','width',
        'height','display','position','top','left','right','bottom','marginTop','marginLeft','marginRight',
        'marginBottom','paddingTop','paddingLeft','paddingRight','paddingBottom','borderTopWidth',
        'borderLeftWidth','borderTopColor','borderColor','font','fontStyle','fontVariant','fontStretch',
        'fontSize','fontFamily','fontWeight','lineHeight',
        'opacity','visibility','overflow','zIndex','textAlign','verticalAlign','float','clear','cursor',
        'whiteSpace','textDecoration','borderRadius','boxShadow','transform','boxSizing',
        'gap','rowGap','columnGap','aspectRatio',
        'flex','flexGrow','flexShrink','flexBasis','margin','padding','border','borderWidth',
        'borderStyle','minWidth','minHeight','maxWidth','maxHeight','alignSelf','alignItems',
        'justifyContent','order','listStyle','outline','inset','transition'];
    }
    g.__computedCamel = __computedNames;
    /* Accessors live on one shared prototype: defining them per getComputedStyle()
     * call made every call cost ~0.57ms; css3test calls it thousands of times,
     * which is what pinned a core. The raw bridge is ~0.003ms. */
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
      /* length/item describe the declaration (the supported CSS properties),
       * like a real computed CSSStyleDeclaration; they used to enumerate the
       * element's *inline* style object, which answered 0 for every element
       * without a style attribute. */
      proto.item = function (i) {
        var name = g.__computedCamel[i];
        return name === undefined ? '' : dash(name);
      };
      if (Object.defineProperty) {
        try {
          Object.defineProperty(proto, 'length', { get: function () {
            return g.__computedCamel.length;
          } });
        } catch (e) {}
      }
      for (var i = 0; i < g.__computedCamel.length; ++i) {
        (function (name) {
          var cssName = dash(name);
          function define(alias) {
            try {
              Object.defineProperty ? Object.defineProperty(proto, alias, {
                get: function () { return this.getPropertyValue(cssName); },
                enumerable: true, configurable: true
              }) : (proto[alias] = '');
            } catch (e) { proto[alias] = ''; }
          }
          define(name);
          /* `-webkit-*` properties answer to both spellings in Chrome
           * (cs.webkitAppearance and cs.WebkitAppearance). */
          if (name.slice(0, 6) === 'Webkit') define('w' + name.slice(1));
        })(g.__computedCamel[i]);
      }
    })(g.__Computed.prototype);
    g.getComputedStyle = function (el, pseudo) { return new g.__Computed(el, pseudo); };
  }
  {
    var _ctors = ['Document','HTMLDocument','Element','HTMLElement','HTMLDivElement','Node','NodeList','HTMLCollection','Event','CustomEvent','MouseEvent','KeyboardEvent','DocumentFragment','Text','Comment','Attr','DOMParser','XMLSerializer','MutationObserver','Storage','Location','Navigator','Screen','History','Window','Performance','CSSStyleDeclaration','HTMLUnknownElement','HTMLAnchorElement','HTMLAreaElement','HTMLAudioElement','HTMLBRElement','HTMLBaseElement','HTMLBodyElement','HTMLButtonElement','HTMLCanvasElement','HTMLDListElement','HTMLDataElement','HTMLDataListElement','HTMLDetailsElement','HTMLDialogElement','HTMLEmbedElement','HTMLFieldSetElement','HTMLFontElement','HTMLFormElement','HTMLFrameElement','HTMLFrameSetElement','HTMLHRElement','HTMLHeadElement','HTMLHeadingElement','HTMLHtmlElement','HTMLIFrameElement','HTMLImageElement','HTMLInputElement','HTMLLIElement','HTMLLabelElement','HTMLLegendElement','HTMLLinkElement','HTMLMapElement','HTMLMediaElement','HTMLMenuElement','HTMLMetaElement','HTMLMeterElement','HTMLModElement','HTMLOListElement','HTMLObjectElement','HTMLOptGroupElement','HTMLOptionElement','HTMLOutputElement','HTMLParagraphElement','HTMLParamElement','HTMLPictureElement','HTMLPreElement','HTMLProgressElement','HTMLQuoteElement','HTMLScriptElement','HTMLSelectElement','HTMLSlotElement','HTMLSourceElement','HTMLSpanElement','HTMLStyleElement','HTMLTableCaptionElement','HTMLTableCellElement','HTMLTableColElement','HTMLTableElement','HTMLTableRowElement','HTMLTableSectionElement','HTMLTemplateElement','HTMLTextAreaElement','HTMLTimeElement','HTMLTitleElement','HTMLTrackElement','HTMLUListElement','HTMLVideoElement','SVGElement','SVGSVGElement'];
  /* Scroll offsets and scrollTo/scrollBy are installed UNCONDITIONALLY: the
   * window getter the host installs on a realm's global object already answers
   * innerWidth, and the old guard below (	ypeof g.innerWidth === 'undefined')
   * then skipped this whole block - so window.scrollTo/scrollY/pageYOffset went
   * missing (R151). */
  {
    function scrollX() {
      var d = g.document;
      if (!d) return 0;
      var se = d.documentElement, b = d.body;
      var v = se ? se.scrollLeft : 0;
      if (!v && b && b.scrollLeft) v = b.scrollLeft;
      return v || 0;
    }
    function scrollY() {
      var d = g.document;
      if (!d) return 0;
      var se = d.documentElement, b = d.body;
      var v = se ? se.scrollTop : 0;
      if (!v && b && b.scrollTop) v = b.scrollTop;
      return v || 0;
    }
    if (Object.defineProperty) {
      Object.defineProperty(g, 'pageXOffset', { get: scrollX, configurable: true });
      Object.defineProperty(g, 'pageYOffset', { get: scrollY, configurable: true });
      Object.defineProperty(g, 'scrollX', { get: scrollX, configurable: true });
      Object.defineProperty(g, 'scrollY', { get: scrollY, configurable: true });
    } else {
      g.pageXOffset = g.scrollX = 0;
      g.pageYOffset = g.scrollY = 0;
    }
    g.scrollTo = function (x, y) {
      var d = g.document;
      if (!d) return;
      var se = d.scrollingElement || d.documentElement || d.body;
      if (!se) return;
      if (x !== undefined && x !== null) se.scrollLeft = x;
      if (y !== undefined && y !== null) se.scrollTop = y;
    };
    g.scrollBy = function (x, y) { g.scrollTo(scrollX() + (x || 0), scrollY() + (y || 0)); };
  }
  if (typeof g.innerWidth === 'undefined') {
    /* Viewport metrics are per FRAME, not per process: an iframe is a small
     * viewport inside the parent's window, and pages size themselves from
     * these values. They used to be plain properties filled in once when this
     * prelude ran (outerWidth = the top-level width), so a child frame read
     * the parent's 1100 and laid its content out for a window it does not
     * have - WeChat's login QR was sized for the parent page and then clipped
     * inside its 200x200 iframe. Getters keep them in step with whichever
     * document is bound. */
    function innerW() {
      if (typeof g.__agbViewportWidth === 'function') { try { return g.__agbViewportWidth(g.document); } catch (e) {} }
      return (g.document && g.document.documentElement) ? g.document.documentElement.clientWidth : 0;
    }
    function innerH() {
      if (typeof g.__agbViewportHeight === 'function') { try { return g.__agbViewportHeight(g.document); } catch (e) {} }
      return (g.document && g.document.documentElement) ? g.document.documentElement.clientHeight : 0;
    }
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

  }
  if (typeof g.devicePixelRatio === 'undefined') {
    /* The engine does not expose a DPR to the page (the window binding is not
     * installed on the global). 1 is correct for 100% zoom / 96 DPI, which is
     * what the probe uses; a real DPR getter can replace this later. */
    try {
      Object.defineProperty(g, 'devicePixelRatio', { get: function () { return 1; }, configurable: true });
    } catch (e) {
      g.devicePixelRatio = 1;
    }
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
     the current realm slot and restore its realm + document around the
     callback. In per-frame-realm mode the slot is a realm number (0 = main
     frame, 1..N = frame realms), never another realm's document slot (R130). */
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
  g.open = function (url, name) {
    var ok = false;
    try {
      ok = !!__agbOpenWindow(url == null ? '' : String(url),
                             name == null ? '' : String(name));
    } catch (e) { ok = false; }
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
  /* ---- window identity (B2) -------------------------------------------
   * `window` is the realm's global object, so it was a bare Object with
   * @@toStringTag "global": `window instanceof Window` was false and
   * Object.prototype.toString said [object global]. Relinking the global's
   * [[Prototype]] to Window.prototype (which the bridge already chains to
   * EventTarget.prototype) makes instanceof/constructor/toString agree with a
   * browser, without moving any of the window's own properties. */
  (function () {
    var W = g.Window;
    if (typeof W !== 'function' || !W.prototype) return;
    if (Object.getPrototypeOf && Object.getPrototypeOf(g) === W.prototype) {
      /* already relinked (same realm, new document) */
    } else if (Object.setPrototypeOf) {
      try { Object.setPrototypeOf(g, W.prototype); } catch (e) {}
    }
    try {
      if (typeof Symbol === 'function' && Symbol.toStringTag && Object.defineProperty)
        Object.defineProperty(g, Symbol.toStringTag, { value: 'Window', configurable: true });
    } catch (e) {}
  })();
  /* ---- event interface objects (B2) ------------------------------------
   * Engine-dispatched events used to be plain objects, so `e instanceof Event`
   * was false and Object.prototype.toString gave [object Object]. The kernel
   * already ships a real native Event (accessors, native preventDefault that
   * cancels WebCore defaults, EventTarget dispatch). This block keeps it as
   * the base and adds the interface prototypes on top:
   *
   *   MouseEvent.prototype -> UIEvent.prototype -> Event.prototype -> Object
   *
   * A facade instance is a real native Event whose [[Prototype]] is re-linked
   * to its interface (Object.setPrototypeOf keeps the native brand, so every
   * accessor and method still runs on it) plus own data properties for the
   * interface fields. __agbMakeEvent() builds events that way, and the C
   * dispatcher keeps reading the same cancel flags.
   *
   * UIEvent/FocusEvent/... used to be bare polyfill constructors inheriting
   * Event.prototype, so reading an inherited accessor on one of their instances
   * (`new UIEvent('x').type`) threw "Value of this must be of type Event". */
  (function () {
    try {
    var nativeE = g.Event;
    var E = (typeof nativeE === 'function' && nativeE.prototype) ? nativeE : null;
    if (!E) {
      /* A realm created after engine bootstrap historically had no kernel
       * Event (the events module only initialises the main realm). The host
       * now copies the real constructors into frame realms; this fallback
       * keeps the facade working in any realm that still lacks one. */
      E = function Event(type, options) {
        var opts = options || {};
        var ev = {
          type: type == null ? '' : String(type),
          target: null, currentTarget: null, eventPhase: 0,
          bubbles: !!opts.bubbles, cancelable: !!opts.cancelable,
          defaultPrevented: false, returnValue: true, composed: false,
          isTrusted: false, timeStamp: (new Date()).getTime(),
          __prevented: false, __stopped: false, __stoppedImmediate: false
        };
        ev.preventDefault = function () {
          this.__prevented = true; this.defaultPrevented = true; this.returnValue = false;
        };
        ev.stopPropagation = function () { this.__stopped = true; };
        ev.stopImmediatePropagation = function () {
          this.__stopped = true; this.__stoppedImmediate = true;
        };
        ev.composedPath = function () { return []; };
        return ev;
      };
      E.prototype = {};
      own(E.prototype, 'constructor', E, false);
      setTag(E.prototype, 'Event');
      if (typeof g.Event === 'undefined')
        g.Event = E;
    }

    /* Helpers first: __agbMakeEvent calls them even if building the interface
     * prototypes below throws in some unusual realm. */
    g.__agbEventPhase = function (evt, current, phase) {
      if (!evt) return;
      own(evt, 'currentTarget', current === undefined ? null : current, true);
      own(evt, 'eventPhase', phase | 0, true);
    };
    g.__agbEventSetTarget = function (evt, target) {
      if (!evt) return;
      own(evt, 'target', target === undefined ? null : target, true);
    };
    g.__agbEventTrusted = function (evt) {
      if (!evt) return;
      own(evt, 'isTrusted', true, true);
    };

    if (typeof E !== 'function' || !E.prototype) return;

    function own(obj, name, value, enumerable) {
      try {
        Object.defineProperty(obj, name, {
          value: value, writable: true, enumerable: !!enumerable, configurable: true
        });
      } catch (e) {
        try { obj[name] = value; } catch (e2) {}
      }
    }
    function setTag(proto, name) {
      try {
        if (typeof Symbol === 'function' && Symbol.toStringTag && Object.defineProperty)
          Object.defineProperty(proto, Symbol.toStringTag, { value: name, configurable: true });
      } catch (e) {}
    }
    /* flags the C dispatcher reads; native methods must still run so
     * WebCore's own default handling (links, beforeinput) sees the cancel. */
    if (!E.prototype.__agbFlagsPatched) {
      var nPrevent = E.prototype.preventDefault;
      var nStop = E.prototype.stopPropagation;
      var nStopImm = E.prototype.stopImmediatePropagation;
      if (typeof nPrevent === 'function') E.prototype.preventDefault = function () {
        own(this, '__prevented', true, false);
        return nPrevent.call(this);
      };
      if (typeof nStop === 'function') E.prototype.stopPropagation = function () {
        own(this, '__stopped', true, false);
        return nStop.call(this);
      };
      if (typeof nStopImm === 'function') E.prototype.stopImmediatePropagation = function () {
        own(this, '__stopped', true, false);
        own(this, '__stoppedImmediate', true, false);
        return nStopImm.call(this);
      };
      own(E.prototype, '__agbFlagsPatched', 1, false);
    }

    /* (name, parent ctor or null, [field, default, ...]) */
    function iface(name, parent, fields) {
      var Ctor = function (type, options) {
        var ev;
        try { ev = new E(type == null ? '' : String(type), options || {}); }
        catch (e) { ev = new E(''); }
        try { Object.setPrototypeOf(ev, Ctor.prototype); } catch (e) {}
        var opts = options || {};
        for (var i = 0; i < fields.length; i += 2) {
          var key = fields[i];
          own(ev, key, opts[key] === undefined ? fields[i + 1] : opts[key], true);
        }
        return ev;
      };
      Ctor.prototype = Object.create(parent ? parent.prototype : E.prototype);
      try {
        Object.defineProperty(Ctor.prototype, 'constructor', {
          value: Ctor, writable: true, enumerable: false, configurable: true
        });
      } catch (e) { Ctor.prototype.constructor = Ctor; }
      setTag(Ctor.prototype, name);
      try { Object.defineProperty(Ctor, 'name', { value: name, configurable: true }); } catch (e) {}
      own(Ctor, 'NONE', 0, false);
      g[name] = Ctor;
      return Ctor;
    }

    /* The kernel's Event.prototype is a null-prototype object, so even a plain
     * native event was not `instanceof Object`. Link it (CustomEvent/… already
     * inherit from Event.prototype, so the whole event family gets it). */
    try {
      if (Object.getPrototypeOf && Object.getPrototypeOf(E.prototype) === null &&
          Object.setPrototypeOf)
        Object.setPrototypeOf(E.prototype, Object.prototype);
    } catch (e) {}

    var uiEvent = iface('UIEvent', null,
      ['view', null, 'detail', 0]);
    var mouse = iface('MouseEvent', uiEvent,
      ['detail', 0, 'screenX', 0, 'screenY', 0, 'clientX', 0, 'clientY', 0,
       'pageX', 0, 'pageY', 0, 'button', 0, 'buttons', 0, 'relatedTarget', null,
       'ctrlKey', false, 'shiftKey', false, 'altKey', false, 'metaKey', false,
       'movementX', 0, 'movementY', 0]);
    iface('KeyboardEvent', uiEvent,
      ['key', '', 'code', '', 'location', 0, 'ctrlKey', false, 'shiftKey', false,
       'altKey', false, 'metaKey', false, 'repeat', false, 'isComposing', false,
       'keyCode', 0, 'charCode', 0, 'which', 0]);
    iface('FocusEvent', uiEvent, ['relatedTarget', null]);
    iface('InputEvent', uiEvent, ['data', null, 'inputType', '', 'isComposing', false]);
    iface('WheelEvent', mouse,
      ['deltaX', 0, 'deltaY', 0, 'deltaZ', 0, 'deltaMode', 0]);
    iface('PointerEvent', mouse,
      ['pointerId', 0, 'pointerType', '', 'isPrimary', false, 'width', 1, 'height', 1,
       'pressure', 0, 'tangentialPressure', 0, 'tiltX', 0, 'tiltY', 0, 'twist', 0]);
    iface('DragEvent', mouse, ['dataTransfer', null]);
    iface('CompositionEvent', uiEvent, ['data', '', 'locale', '']);
    iface('ClipboardEvent', null, ['clipboardData', null]);
    iface('TouchEvent', uiEvent,
      ['touches', [], 'targetTouches', [], 'changedTouches', [],
       'ctrlKey', false, 'shiftKey', false, 'altKey', false, 'metaKey', false]);
    iface('MessageEvent', null,
      ['data', null, 'origin', '', 'lastEventId', '', 'source', null, 'ports', []]);
    iface('PopStateEvent', null, ['state', null]);
    iface('HashChangeEvent', null, ['oldURL', '', 'newURL', '']);
    iface('PageTransitionEvent', null, ['persisted', false]);

    /* C updates currentTarget/eventPhase as it walks the tree (the helpers
     * are defined above, before the prototypes). */
    } catch (err) { g.__agbEventError = String(err); }
  })();
  /* (type, target, kind, bubbles, cancelable): every value-carrying field is
   * an own data property, so the C dispatcher's later writes (clientX, key,
   * data, state, persisted) update them. */
  g.__agbMakeEvent = function (type, target, kind, bubbles, cancelable) {
    var Ctor = (kind && typeof g[kind] === 'function') ? g[kind] : null;
    var ev = null;
    var opts = { bubbles: bubbles !== false, cancelable: cancelable !== false };
    if (Ctor) {
      try { ev = new Ctor(type == null ? '' : String(type), opts); } catch (e) { ev = null; }
    }
    if (!ev) {
      try { ev = new g.Event(type == null ? '' : String(type), opts); }
      catch (e) { ev = {}; }
    }
    g.__agbEventSetTarget(ev, target === undefined ? null : target);
    g.__agbEventPhase(ev, target === undefined ? null : target, 2);
    g.__agbEventTrusted(ev);
    return ev;
  };
  /* ---- collections (B2-2) ----------------------------------------------
   * NodeList/HTMLCollection are bridge classes: length, item(), namedItem()
   * and indexed access are answered in C (live kinds read the DOM), so the
   * iteration protocol is the only JS-side part. Chrome gives NodeList
   * forEach/keys/values/entries/@@iterator and HTMLCollection only
   * @@iterator (no forEach). */
  (function () {
    function define(proto, name, value) {
      try {
        Object.defineProperty(proto, name, {
          value: value, writable: true, configurable: true
        });
      } catch (e) {
        try { proto[name] = value; } catch (e2) {}
      }
    }
    function makeIterator(list, mode) {
      var index = 0;
      var iterator = {
        next: function () {
          if (index >= list.length)
            return { value: undefined, done: true };
          var i = index++;
          if (mode === 1) return { value: i, done: false };
          if (mode === 2) return { value: [i, list[i]], done: false };
          return { value: list[i], done: false };
        }
      };
      if (typeof Symbol === 'function' && Symbol.iterator)
        iterator[Symbol.iterator] = function () { return iterator; };
      return iterator;
    }
    function install(name, withForEach) {
      var Ctor = g[name];
      if (typeof Ctor !== 'function' || !Ctor.prototype)
        return;
      var proto = Ctor.prototype;
      if (!proto.values) define(proto, 'values', function () { return makeIterator(this, 0); });
      if (!proto.keys) define(proto, 'keys', function () { return makeIterator(this, 1); });
      if (!proto.entries) define(proto, 'entries', function () { return makeIterator(this, 2); });
      if (typeof Symbol === 'function' && Symbol.iterator && !proto[Symbol.iterator])
        define(proto, Symbol.iterator, proto.values);
      if (withForEach && !proto.forEach) define(proto, 'forEach', function (callback, thisArg) {
        if (typeof callback !== 'function')
          throw new TypeError('forEach: callback is not a function');
        for (var i = 0; i < this.length; ++i)
          callback.call(thisArg, this[i], i, this);
      });
    }
    install('NodeList', true);
    install('HTMLCollection', false);
    /* The CSSOM lists are array-likes too (length + indexed answered in C). GitHub's
     * core bundle spreads document.styleSheets / sheet.cssRules and died with
     * "not iterable" (issues #22). */
    install('StyleSheetList', false);
    install('CSSRuleList', false);
    install('MediaList', false);
    install('FileList', true);
    install('DOMTokenList', true);
    install('RadioNodeList', false);
    install('NamedNodeMap', true);

  /* --- NodeFilter constants (DOM2 Traversal) ------------------------------- *
   * The walker itself is a real C++ interface now (issues #27): document.
   * createTreeWalker()/TreeWalker come from buildANT, and the filter callback is
   * invoked by WebCore's traversal engine. Only the constants object remains
   * here, because Chrome exposes NodeFilter as a function with enumerable
   * FILTER_ACCEPT / SHOW_ELEMENT constants. */
  if (typeof g.NodeFilter === 'undefined') {
    var nodeFilterCtor = function NodeFilter() {};
    var nodeFilterConstants = {
      FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3,
      SHOW_ALL: 0xFFFFFFFF, SHOW_ELEMENT: 1, SHOW_ATTRIBUTE: 2, SHOW_TEXT: 4,
      SHOW_CDATA_SECTION: 8, SHOW_ENTITY_REFERENCE: 16, SHOW_ENTITY: 32,
      SHOW_PROCESSING_INSTRUCTION: 64, SHOW_COMMENT: 128, SHOW_DOCUMENT: 256,
      SHOW_DOCUMENT_TYPE: 512, SHOW_DOCUMENT_FRAGMENT: 1024, SHOW_NOTATION: 2048
    };
    for (var nodeFilterKey in nodeFilterConstants)
      nodeFilterCtor[nodeFilterKey] = nodeFilterConstants[nodeFilterKey];
    g.NodeFilter = nodeFilterCtor;
  }

  /* requestFullscreen()/exitFullscreen(): state and fullscreenchange are C++
   * (issues #27). Chrome applies the transition and resolves the Promise in a
   * task, so the deferred state change lives here; a page that attaches its
   * listener after the call still sees the event. */
  (function () {
    var installedElement = null, installedDocument = null;

    function installFullscreen() {
      var elementProto = g.Element && g.Element.prototype;
      var documentProto = g.Document && g.Document.prototype;

      if (elementProto && elementProto !== installedElement && !elementProto.requestFullscreen) {
        installedElement = elementProto;
        var request = function () {
          var element = this;

          return new Promise(function (resolve, reject) {
            setTimeout(function () {
              if (g.__agbRequestFullscreen && g.__agbRequestFullscreen(element)) resolve();
              else reject(new TypeError("Failed to execute 'requestFullscreen' on 'Element': the element is not connected"));
            }, 0);
          });
        };

        Object.defineProperty(elementProto, 'requestFullscreen',
                              { value: request, writable: true, configurable: true });
        Object.defineProperty(elementProto, 'webkitRequestFullscreen',
                              { value: request, writable: true, configurable: true });
      }

      if (documentProto && documentProto !== installedDocument && !documentProto.exitFullscreen) {
        installedDocument = documentProto;
        var exit = function () {
          var doc = this;

          return new Promise(function (resolve, reject) {
            setTimeout(function () {
              if (g.__agbExitFullscreen && g.__agbExitFullscreen(doc)) resolve();
              else reject(new TypeError("Failed to execute 'exitFullscreen' on 'Document'"));
            }, 0);
          });
        };

        Object.defineProperty(documentProto, 'exitFullscreen',
                              { value: exit, writable: true, configurable: true });
        Object.defineProperty(documentProto, 'webkitExitFullscreen',
                              { value: exit, writable: true, configurable: true });
      }
    }

    installFullscreen();
    try { setTimeout(installFullscreen, 0); } catch (e) { /* no timers yet */ }
    g.__agbInstallFullscreen = installFullscreen;
  })();

  /* Element.scrollTo()/scrollBy(): the 2008 Element IDL has no scroll API, but
   * pages call it on any scroll container (Baidu's telemetry creates a
   * `<t_in>` element and immediately calls scrollTo(0,0)). scrollTop/scrollLeft
   * already route to RenderLayer, so the polyfill just forwards to them. */
  (function () {
    function installElementScroll() {
      var elementProto = g.Element && g.Element.prototype;
      if (!elementProto || elementProto.scrollTo) return;

      function coords(x, y) {
        if (x && typeof x === 'object') return { left: x.left, top: x.top };
        return { left: x, top: y };
      }
      Object.defineProperty(elementProto, 'scrollTo', {
        configurable: true, writable: true,
        value: function (x, y) {
          var c = coords(x, y);
          if (c.left != null) this.scrollLeft = Number(c.left) || 0;
          if (c.top != null) this.scrollTop = Number(c.top) || 0;
        }
      });
      Object.defineProperty(elementProto, 'scrollBy', {
        configurable: true, writable: true,
        value: function (x, y) {
          var c = coords(x, y);
          if (c.left != null) this.scrollLeft = (Number(this.scrollLeft) || 0) + (Number(c.left) || 0);
          if (c.top != null) this.scrollTop = (Number(this.scrollTop) || 0) + (Number(c.top) || 0);
        }
      });
    }
    installElementScroll();
    try { setTimeout(installElementScroll, 0); } catch (e) { /* no timers yet */ }
    g.__agbInstallElementScroll = installElementScroll;
  })();

  /* Fullscreen: requestFullscreen()/exitFullscreen()/document.fullscreenElement
   * are C++ (issues #27): state + fullscreenchange live in the engine, so a page
   * that awaits the Promise and reads the document state behaves like Chrome.
   * The shell window itself is not resized yet (roadmap). */
  })();
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
  /* document.createEvent + CustomEvent.prototype.initCustomEvent.
   *
   * Svelte's createEventDispatcher (what vConsole's switch uses to open the
   * panel, and what any older library calls) does:
   *     var e = document.createEvent('CustomEvent');
   *     e.initCustomEvent(type, bubbles, cancelable, detail);
   * The DOM bridge exposes the Event/CustomEvent constructors but neither the
   * legacy factory nor initCustomEvent, so that line died with
   * "undefined is not a function" and the click handler never ran (R172).
   * `detail` is a getter-only field on the engine's CustomEvent prototype, so
   * the init method defines an own data property that shadows it. */
  if (typeof g.document !== 'undefined' && g.document &&
      typeof g.document.createEvent !== 'function') {
    g.document.createEvent = function (type) {
      var t = String(type == null ? '' : type).toLowerCase();
      try {
        if (t === 'customevent') return new g.CustomEvent('');
        if ((t === 'mouseevent' || t === 'mouseevents') &&
            typeof g.MouseEvent === 'function') return new g.MouseEvent('');
        if ((t === 'keyboardevent' || t === 'keyevents') &&
            typeof g.KeyboardEvent === 'function') return new g.KeyboardEvent('');
        return new g.Event('');
      } catch (e) {
        return new g.Event('');
      }
    };
  }
  if (typeof g.CustomEvent === 'function' && g.CustomEvent.prototype &&
      typeof g.CustomEvent.prototype.initCustomEvent !== 'function') {
    g.CustomEvent.prototype.initCustomEvent = function (type, bubbles, cancelable, detail) {
      if (typeof this.initEvent === 'function') this.initEvent(type, bubbles, cancelable);
      try {
        Object.defineProperty(this, 'detail', {
          value: detail, writable: true, enumerable: true, configurable: true
        });
      } catch (e) {
        try { this.detail = detail; } catch (e2) {}
      }
      return undefined;
    };
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
    /* Field state mirrors the XMLHttpRequest spec where pages can observe it:
     * open() resets the response (status 0, empty header list), getResponseHeader
     * is case-insensitive and returns null when absent, getAllResponseHeaders is
     * lowercase + sorted + CRLF-joined, and Set-Cookie is filtered from both, as
     * Chrome does. responseText throws InvalidStateError once responseType is
     * neither '' nor 'text'. */
    var XHR = function XMLHttpRequest() {
      this.readyState = 0; this.status = 0; this.statusText = '';
      this.responseType = ''; this.responseURL = ''; this.timeout = 0;
      this.onreadystatechange = null; this.onload = null; this.onerror = null; this.onloadend = null; this.onabort = null; this.onprogress = null;
      this._url = null; this._method = 'GET'; this._async = true; this._headers = {};
      this._buffer = null;
      this._text = '';                 /* decoded body, backing responseText */
      this._resp = '';                 /* backing response when not text/binary */
      this._headerPairs = [];          /* [[name, value], ...] as received */
      this._mimeOverride = null;       /* charset from overrideMimeType() */
      this._reachedHeaders = false;    /* readyState 2 already delivered */
      this._reachedLoading = false;    /* readyState 3 already delivered */
      this._reqId = 0;                 /* native request id, for abort/timeout */
      this._timer = null;              /* xhr.timeout timer */
      this._finished = false;          /* timeout/abort already answered */
    };
    /* responseText per spec: valid for '' and 'text'; throws an
     * InvalidStateError DOMException otherwise (pages branch on name). */
    Object.defineProperty(XHR.prototype, 'responseText', {
      configurable: true, enumerable: true,
      get: function () {
        var t = this.responseType;
        if (t !== '' && t !== 'text') {
          var msg = "Failed to read the 'responseText' property from 'XMLHttpRequest':"
                    + " The value is only accessible if the object's 'responseType'"
                    + " is '' or 'text' (was '" + t + "').";
          try { throw new DOMException(msg, 'InvalidStateError'); }
          catch (e) {
            if (e && e.name === 'InvalidStateError') throw e;
            var err = new Error(msg);
            err.name = 'InvalidStateError';
            throw err;
          }
        }
        return this._text;
      },
      set: function (v) { this._text = v; }
    });
    /* response: the whole body in the shape responseType asks for. */
    Object.defineProperty(XHR.prototype, 'response', {
      configurable: true, enumerable: true,
      get: function () {
        var t = this.responseType;
        if (this.readyState !== 4) return (t === '' || t === 'text') ? '' : null;
        if (t === '' || t === 'text') return this._text;
        return this._resp;
      },
      set: function (v) { this._resp = v; }
    });
    /* Header lookup is case-insensitive; Set-Cookie is never exposed (the spec
     * filters it for both accessors, Chrome agrees). */
    XHR.prototype._headerValue = function (name) {
      var want = String(name).toLowerCase();
      if (want === 'set-cookie' || want === 'set-cookie2') return null;
      var pairs = this._headerPairs;
      if (!pairs) return null;
      for (var i = 0; i < pairs.length; i++) {
        if (String(pairs[i][0]).toLowerCase() === want) return pairs[i][1];
      }
      return null;
    };
    XHR.prototype.open = function (method, url, async) {
      this._method = method || 'GET'; this._url = url; this._async = (async !== false);
      /* open() resets the response state. */
      this.status = 0; this.statusText = '';
      this._headerPairs = []; this._buffer = null; this._text = ''; this._resp = '';
      this._mimeOverride = null;
      this._reachedHeaders = false;
      this._reachedLoading = false;
      this._clearTimer();
      this._reqId = 0;
      this._finished = false;
      this.readyState = 1; this._fireState();
    };
    XHR.prototype._clearTimer = function () {
      if (this._timer !== null) { clearTimeout(this._timer); this._timer = null; }
    };
    XHR.prototype._abortNative = function () {
      var id = this._reqId;
      this._reqId = 0;
      if (id && typeof __agbHttpAbort === 'function') {
        try { __agbHttpAbort(id); } catch (e) { }
      }
    };
    XHR.prototype.setRequestHeader = function (k, v) { this._headers[k] = v; };
    XHR.prototype.getAllResponseHeaders = function () {
      var pairs = this._headerPairs;
      if (!pairs || !pairs.length) return '';
      var rows = [];
      for (var i = 0; i < pairs.length; i++) {
        var name = String(pairs[i][0]).toLowerCase();
        if (name === 'set-cookie' || name === 'set-cookie2') continue;
        rows.push(name + ': ' + pairs[i][1]);
      }
      /* The spec sorts by byte order of the lowered name. */
      rows.sort();
      return rows.length ? rows.join('\r\n') + '\r\n' : '';
    };
    XHR.prototype.getResponseHeader = function (name) {
      if (this.readyState < 2) return null;
      return this._headerValue(name);
    };
    /* abort(): drop the native request, then answer the page with the spec's
     * sequence (readystatechange -> abort -> loadend). Before this it only set
     * readyState and called onabort, so the socket kept downloading. */
    XHR.prototype.abort = function () {
      this._clearTimer();
      if (this._reqId === 0 && this.readyState !== 4) { this.readyState = 0; return; }
      if (this._finished) return;
      this._finished = true;
      this._abortNative();
      this.status = 0; this.statusText = '';
      this.readyState = 4;
      this._fireState();
      this._fire('abort');
      this._fire('loadend');
    };
    /* overrideMimeType(): only the charset part is used here - it overrides the
     * one from Content-Type when decoding the body. Legal after open() and
     * before send(); discarded by the next open(). */
    XHR.prototype.overrideMimeType = function (mime) {
      if (this.readyState === 4) return;
      var m = String(mime == null ? '' : mime);
      var cs = /;\s*charset\s*=\s*"?([^";\s]+)"?/i.exec(m);
      this._mimeOverride = cs ? cs[1] : null;
    };
    XHR.prototype.addEventListener = function (type, fn) { this['_ev_' + type] = fn; };
    XHR.prototype.removeEventListener = function (type) { this['_ev_' + type] = null; };
    /* onreadystatechange belongs to readyState transitions only; load/error/
     * loadend are separate events. Firing it from every _fire(type) made page
     * code observe readyState 4 twice (Chrome: 1,2,3,4). */
    XHR.prototype._fireState = function () {
      if (this.onreadystatechange) this.onreadystatechange.call(this, this);
    };
    XHR.prototype._fire = function (type) {
      if (type) {
        var h = this['on' + type] || this['_ev_' + type];
        if (h) h.call(this, this);
      }
    };
    /* Decodes the body bytes: an override charset wins over the header's, and a
     * header with no charset is UTF-8 (XMLHttpRequest's final fallback is UTF-8,
     * not windows-1252 - verified against Chrome). */
    XHR.prototype._decode = function (bytes) {
      var label = this._mimeOverride;
      if (!label) {
        var ct = this._headerValue('content-type');
        if (ct) {
          var m = /;\s*charset\s*=\s*"?([^";\s]+)"?/i.exec(String(ct));
          if (m) label = m[1];
        }
      }
      if (!label || /^utf-?8$/i.test(label)) return g.__agbDecodeUtf8(bytes);
      try {
        return new TextDecoder(label).decode(new Uint8Array(bytes));
      } catch (e) {
        /* Ant has no legacy CJK codecs; a page asking for gbk gets replacement
         * characters from the UTF-8 pass rather than a silent empty body. */
        return g.__agbDecodeUtf8(bytes);
      }
    };
    XHR.prototype._finish = function (status, payload, binary, meta) {
      this._clearTimer();
      if (this._finished) return;
      this._finished = true;
      this.status = status || 0;
      this.statusText = (meta && meta.statusText) ? String(meta.statusText) : '';
      if (meta && meta.headers) this._headerPairs = meta.headers;

      var type = this.responseType;
      var asText = (type === '' || type === undefined || type === null || type === 'text');

      /* The transport hands the whole response over at once, so the headers and
       * body become available in the same turn. Page code still expects the
       * spec's state machine, so walk 2 and 3 before 4 - the observable
       * sequence Chrome produces for a normal request is 1,2,3,4. The guard is
       * a local flag, not readyState: a handler may have written readyState. */
      if (status) {
        if (!this._reachedHeaders) { this._reachedHeaders = true; this.readyState = 2; this._fireState(); }
        if (!this._reachedLoading) { this._reachedLoading = true; this.readyState = 3; this._fireState(); }
      }

      if (binary) {
        /* The async path always delivers bytes (base64), because a text-only
         * channel cannot carry non-UTF-8 or NUL bodies. responseText is still
         * required for the default responseType ('' / 'text'): leaving it
         * empty made every JSON endpoint's xhr.responseText === '' and
         * JSON.parse() fail on the caller's side. Decode the bytes once and
         * answer both views from them. */
        this._buffer = g.__agbB64ToBuffer(payload);
        if (asText || type === 'json') {
          try { this._text = this._decode(this._buffer); } catch (e) { this._text = ''; }
        } else {
          this._text = '';
        }
        if (type === 'json') {
          try { this._resp = JSON.parse(this._text); } catch (e) { this._resp = null; }
        } else if (type === 'blob') {
          try { this._resp = new Blob([this._buffer]); } catch (e) { this._resp = this._buffer; }
        } else if (asText) {
          this._resp = this._text;
        } else {
          this._resp = this._buffer;
        }
      } else {
        this._text = (payload === undefined || payload === null) ? '' : String(payload);
        if (type === 'json') {
          try { this._resp = JSON.parse(this._text); } catch (e) { this._resp = null; }
        } else if (type === 'blob') {
          try { this._resp = new Blob([this._text]); } catch (e) { this._resp = this._text; }
        } else {
          this._resp = this._text;
        }
      }
      /* Responses reaching readyState 4 with a status are always 2xx here:
       * the transport reports failures as status 0. */
      this.readyState = 4;
      this._fireState();
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
        if (wantBinary || typeof __agbHttpGetEx === 'function') {
          /* One entry point carries body + status + headers; the older
           * single-value __agbHttpGet stays as the fallback for engine/JS skew. */
          var res = null;
          try { res = __agbHttpGetEx(target); } catch (e) { res = null; }
          if (res === undefined || res === null) {
            this._finish(0, null, false, null);
            return;
          }
          if (typeof res === 'string') {
            this._finish(200, res, false, null);   /* old engine */
            return;
          }
          if (wantBinary) {
            /* No binary sync entry for a full request; re-encode the text. */
            this._finish(res.status, g.__agbB64EncodeUtf8(res.body || ''), true, res);
          } else {
            this._finish(res.status, res.body || '', false, res);
          }
          return;
        }
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
         fail and retry in a loop. The returned id is what lets abort() and
         xhr.timeout kill the request on the network side too. */
      var reqId = __agbHttpRequestEx(target, method, ctype, payload, function (status, b64, meta) {
        if (self._finished) return;
        self._reqId = 0;
        self._finish(status, b64, true, meta);
      });
      this._reqId = (typeof reqId === 'number') ? reqId : 0;

      /* xhr.timeout: the spec's timer, not the transport's (WinHTTP quantizes
       * its own timeouts to ~4 s, and the page must see `timeout` at the
       * requested time). The native request is aborted for real. */
      if (this.timeout > 0) {
        this._timer = setTimeout(function () {
          self._timer = null;
          if (self._finished || self.readyState === 4) return;
          self._finished = true;
          self._abortNative();
          self.status = 0; self.statusText = '';
          self.readyState = 4;
          self._fireState();
          self._fire('timeout');
          self._fire('loadend');
        }, this.timeout);
      }
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
      var signal = opts.signal;
      return new Promise(function (resolve, reject) {
        var reqId = 0;
        var finished = false;

        function cleanup() {
          if (signal && signal.removeEventListener) {
            try { signal.removeEventListener('abort', onAbort); } catch (e) { }
          }
        }
        function onAbort() {
          if (finished) return;
          finished = true;
          if (reqId && typeof __agbHttpAbort === 'function') {
            try { __agbHttpAbort(reqId); } catch (e) { }
          }
          cleanup();
          var err;
          try { err = new DOMException('The operation was aborted.', 'AbortError'); }
          catch (e) { err = new Error('The operation was aborted.'); err.name = 'AbortError'; }
          reject(err);
        }

        reqId = __agbHttpRequestEx(url, method, ctype, payload, function (status, b64) {
          if (finished) return;
          finished = true;
          cleanup();
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

        /* AbortSignal: reject with AbortError and drop the native request, the
         * same way a timeout drops an XHR. An already-aborted signal never
         * starts the request at all. */
        if (signal) {
          if (signal.aborted) { onAbort(); return; }
          if (signal.addEventListener) signal.addEventListener('abort', onAbort);
        }
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
    var ev = g.__agbMakeEvent(type, w,
      (type === 'message' || type === 'messageerror') ? 'MessageEvent' : 'Event');
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
    var ev = g.__agbMakeEvent('error', w, 'Event');
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

  /* Lifecycle marker for the probe harness (`--wait-dcl`): this script runs at
   * document bind, before the page's own scripts, so its listener is the only
   * reliable "DOMContentLoaded happened" signal the host can get without an
   * engine callback. Top frame only - an iframe's DCL arrives earlier and
   * would arm the host too soon; the URL is carried for sanity checks. Hosts
   * that did not ask simply ignore the message (the probe filters it unless
   * --wait-dcl was given). */
  try {
    if (g.document && g.document.addEventListener && g.top === g) {
      g.document.addEventListener('DOMContentLoaded', function () {
        try {
          if (typeof g.agavePostMessage === 'function')
            g.agavePostMessage('__agb_dcl__' + (g.location ? g.location.href : ''));
        } catch (e) {}
      }, false);
    }
  } catch (e) {}

})(window);
