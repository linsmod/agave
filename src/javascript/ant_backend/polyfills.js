/*
 * Runtime polyfills for the Agave engine.
 *
 * Loaded from disk on every document bind, so they can be edited without
 * rebuilding libagave. Keep everything here idempotent: the file may be
 * evaluated more than once (every navigation).
 */
(function () {
    'use strict';

    var g = window;

    /* --- modern DOM methods ------------------------------------------------ */
    if (document && document.createElement) {
        var proto = Object.getPrototypeOf(document.createElement('div'));

        if (proto) {
            if (!proto.append) {
                proto.append = function () {
                    for (var i = 0; i < arguments.length; ++i) {
                        var n = arguments[i];
                        if (n === null || n === undefined) continue;
                        this.appendChild(typeof n === 'string' ? document.createTextNode(n) : n);
                    }
                    return this;
                };
            }

            if (!proto.prepend) {
                proto.prepend = function () {
                    var first = this.firstChild;
                    for (var i = 0; i < arguments.length; ++i) {
                        var n = arguments[i];
                        if (n === null || n === undefined) continue;
                        this.insertBefore(typeof n === 'string' ? document.createTextNode(n) : n, first);
                    }
                    return this;
                };
            }

            if (!proto.before) {
                proto.before = function () {
                    var p = this.parentNode;
                    if (!p) return;
                    for (var i = 0; i < arguments.length; ++i) p.insertBefore(arguments[i], this);
                };
            }

            if (!proto.after) {
                proto.after = function () {
                    var p = this.parentNode;
                    if (!p) return;
                    var next = this.nextSibling;
                    for (var i = 0; i < arguments.length; ++i) p.insertBefore(arguments[i], next);
                };
            }

            if (!proto.remove) {
                proto.remove = function () {
                    if (this.parentNode) this.parentNode.removeChild(this);
                };
            }

            if (!proto.replaceWith) {
                proto.replaceWith = function () {
                    var p = this.parentNode;
                    if (!p) return;
                    var next = this.nextSibling;
                    for (var i = 0; i < arguments.length; ++i) p.insertBefore(arguments[i], this);
                    p.removeChild(this);
                    if (next) this.nextSibling;
                };
            }

            if (!proto.matches) {
                proto.matches = function (sel) {
                    var p = this.parentNode || this.ownerDocument;
                    if (!p || !p.querySelectorAll) return false;
                    var list = p.querySelectorAll(sel);
                    for (var i = 0; i < list.length; ++i) if (list[i] === this) return true;
                    return false;
                };
            }

            if (!proto.closest) {
                proto.closest = function (sel) {
                    var n = this;
                    while (n && n.nodeType === 1) {
                        if (n.matches && n.matches(sel)) return n;
                        n = n.parentNode;
                    }
                    return null;
                };
            }

            if (!proto.hasAttribute || !('dataset' in document.createElement('div'))) {
                /* dataset is nice to have but comes from the engine; ignore. */
            }
        }
    }

    /* --- instanceof Node -------------------------------------------------- */
    if (typeof Symbol !== 'undefined' && Symbol.hasInstance) {
        var isNode = function (o) { return !!o && typeof o.nodeType === 'number'; };
        var handlers = [
            [Node, isNode],
            [Element, function (o) { return isNode(o) && o.nodeType === 1; }],
            [HTMLElement, function (o) { return isNode(o) && o.nodeType === 1; }],
            [Text, function (o) { return isNode(o) && o.nodeType === 3; }],
            [Comment, function (o) { return isNode(o) && o.nodeType === 8; }],
            [DocumentFragment, function (o) { return isNode(o) && o.nodeType === 11; }]
        ];

        for (var h = 0; h < handlers.length; ++h) {
            var ctor = handlers[h][0];
            if (typeof ctor !== 'function') continue;
            try {
                Object.defineProperty(ctor, Symbol.hasInstance, {
                    configurable: true, writable: true, value: handlers[h][1]
                });
            } catch (e) { /* read only constructor */ }
        }
    }

    /* --- <style>.sheet ----------------------------------------------------
     * Feature sniffing reads style.sheet.cssRules outside any try/catch, so an
     * undefined sheet aborts the whole script. An empty CSSOM makes the sniffing
     * report "not supported" instead. */
    try {
        var probeStyle = document.createElement('style');
        var styleProto = Object.getPrototypeOf(probeStyle);

        if (styleProto && typeof probeStyle.sheet === 'undefined') {
            Object.defineProperty(styleProto, 'sheet', {
                configurable: true,
                get: function () {
                    return {
                        cssRules: [],
                        rules: [],
                        insertRule: function () { return 0; },
                        deleteRule: function () {},
                        addRule: function () { return -1; },
                        removeRule: function () {},
                        ownerNode: this,
                        disabled: false
                    };
                }
            });
        }
    } catch (e) { /* no stylesheet support to patch */ }

    /* --- matchMedia -------------------------------------------------------- */
    if (!g.matchMedia) {
        g.matchMedia = function (query) {
            return {
                media: String(query),
                matches: false,
                onchange: null,
                addListener: function () {},
                removeListener: function () {},
                addEventListener: function () {},
                removeEventListener: function () {},
                dispatchEvent: function () { return false; }
            };
        };
    }

    /* --- CSS.supports ------------------------------------------------------ */
    if (!g.CSS) g.CSS = {};

    if (!g.CSS.supports) {
        g.CSS.supports = function (a, b) {
            var decl = (b !== undefined) ? String(a) + ':' + String(b) : String(a);
            var colon = decl.indexOf(':');

            if (colon < 0) return false;

            var name = decl.slice(0, colon).replace(/^\s+|\s+$/g, '');
            var value = decl.slice(colon + 1).replace(/^\s+|\s+$/g, '');
            var el = document.createElement('div');

            if (!name || !value) return false;

            /* var() is valid at parse time regardless of whether the variable
             * is defined at that point: support is about the syntax. */
            if (/var\(/i.test(value)) return true;

            try {
                if (el.style.setProperty) el.style.setProperty(name, value);
                else el.style[name] = value;
            } catch (e) {
                return false;
            }

            var got = el.style.getPropertyValue ? el.style.getPropertyValue(name) : el.style[name];

            if (!got) return false;

            function norm(s) {
                return String(s).toLowerCase().replace(/\s+/g, ' ').replace(/^\s+|\s+$/g, '');
            }

            var want = norm(value), actual = norm(got);

            /* Grid is not implemented: CSSParser degrades display:grid to
             * block so the declaration survives, and the style object may hand
             * the original text back. Report it as unsupported. */
            if (norm(name) === 'display' && (want === 'grid' || want === 'inline-grid')) return false;

            if (want === actual) return true;

            if (norm(name) === 'display') {
                if (want === 'flex' && actual === '-webkit-box') return true;
                if (want === 'inline-flex' && actual === '-webkit-inline-box') return true;
            }

            return false;
        };
    }

    if (!g.CSS.escape) g.CSS.escape = function (s) { return String(s); };
}());

/* Ant's URLSearchParams is a branded native class, and `class X extends
 * URLSearchParams` loses the subclass' own methods with it (the VM does not
 * install them when the base is a native constructor). Real browsers allow it
 * and libraries rely on it - browserscore.dev's URLParams does - so wrap the
 * native object in a plain JS constructor that delegates. */
(function () {
    var g = typeof window !== 'undefined' ? window : this;
    var Native = g.URLSearchParams;

    if (!Native || g.__agbURLSearchParamsWrapper)
        return;

    g.__agbURLSearchParamsWrapper = true;

    function URLSearchParams(init) {
        this.__agbNativeParams = (init === undefined) ? new Native() : new Native(init);
    }

    URLSearchParams.prototype.append = function (k, v) { this.__agbNativeParams.append(k, v); };
    URLSearchParams.prototype['delete'] = function (k) { this.__agbNativeParams['delete'](k); };
    URLSearchParams.prototype.get = function (k) { return this.__agbNativeParams.get(k); };
    URLSearchParams.prototype.getAll = function (k) { return this.__agbNativeParams.getAll(k); };
    URLSearchParams.prototype.has = function (k) { return this.__agbNativeParams.has(k); };
    URLSearchParams.prototype.set = function (k, v) { this.__agbNativeParams.set(k, v); };
    URLSearchParams.prototype.sort = function () { this.__agbNativeParams.sort(); };
    URLSearchParams.prototype.toString = function () { return String(this.__agbNativeParams); };
    URLSearchParams.prototype.keys = function () { return this.__agbNativeParams.keys(); };
    URLSearchParams.prototype.values = function () { return this.__agbNativeParams.values(); };
    URLSearchParams.prototype.entries = function () { return this.__agbNativeParams.entries(); };
    URLSearchParams.prototype.forEach = function (fn, thisArg) {
        this.__agbNativeParams.forEach(function (value, key) { fn.call(thisArg, value, key, this); });
    };

    g.URLSearchParams = URLSearchParams;
}());

/* Console guard: a page that logs in a render loop (Vue warnings, debug
 * leftovers) can flood the console with deep object dumps, which makes the
 * terminal unusable and burns time formatting. Rate limit to 200 messages per
 * second, print how many were dropped, and shorten object arguments.
 *
 * console may not exist yet when the polyfills run, so install on a timer
 * until it appears. */
(function () {
    var g = typeof window !== 'undefined' ? window : this;
    var tries = 0;

    if (g.__agbConsoleGuard)
        return;

    function install() {
        if (g.__agbConsoleGuard)
            return;

        if (!g.console || typeof g.console.log !== 'function') {
            if (tries++ < 200) setTimeout(install, 10);
            return;
        }

        g.__agbConsoleGuard = true;

        var perSecond = 200;
        var bucket = 0;
        var dropped = 0;
        var lastSecond = 0;

        function shorten(value) {
            if (value === null || typeof value !== 'object')
                return value;
            try {
                if (value instanceof Error || (value.constructor && value.constructor.name === 'Error'))
                    return value.name + ': ' + value.message;
                return Object.prototype.toString.call(value) === '[object Array]'
                    ? '[Array(' + value.length + ')]'
                    : '[Object ' + (value.constructor && value.constructor.name || '') + ']';
            } catch (e) {
                return '[Object]';
            }
        }

        function wrap(level) {
            var original = g.console[level];
            var lastKey = null;    /* fast-path key of the previous message */
            var counts = { };      /* suppressed repeats per first-line key */
            var seen = { };        /* keys that already printed once */
            var seenCount = 0;

            if (typeof original !== 'function')
                return;

            g.console[level] = function () {
                var now = Date.now();
                var args = [];
                var key;
                var i;

                /* Consecutive-duplicate throttle first: libraries log the
                 * same warning once per DOM node (a Vue dev build produced
                 * 1600+ identical warns on one page). The key is the FIRST
                 * LINE of the first string argument only - the message body
                 * - because the trailing component trace differs on every
                 * call and would break dedup. Repeats accumulate in counts[]
                 * and are summarized once per second. */
                key = '';
                for (i = 0; i < arguments.length; i++) {
                    if (i === 0 && typeof arguments[i] === 'string') {
                        key = arguments[i].split('\n')[0].slice(0, 200);
                        break;
                    }
                }

                /* Seen-registry: the first sighting of a warning prints;
                 * every later occurrence (even minutes apart, non-consecutively)
                 * only increments the per-second suppression counter. The
                 * registry is capped so unique-but-noisy pages can't grow it
                 * without bound. */
                if (seen[key] || seenCount >= 128) {
                    counts[key] = (counts[key] || 0) + 1;
                    lastKey = key;
                    return;
                }
                seen[key] = 1;
                seenCount++;
                lastKey = key;

                if (now !== lastSecond) {
                    var total = 0;
                    var worst = null;
                    var worstCount = 0;

                    for (var k in counts) {
                        total += counts[k];
                        if (counts[k] > worstCount) {
                            worstCount = counts[k];
                            worst = k;
                        }
                    }

                    lastSecond = now;
                    bucket = 0;

                    if (total > 0) {
                        original.call(g.console, '[console] ' + total
                            + ' duplicate message(s) suppressed; top: '
                            + (worst ? worst.slice(0, 120) : '?') + ' x' + worstCount);
                        counts = { };
                        lastKey = null;
                    }

                    if (dropped > 0) {
                        original.call(g.console, '[console] ' + dropped + ' message(s) dropped');
                        dropped = 0;
                    }
                }

                if (++bucket > perSecond) {
                    dropped++;
                    return;
                }

                for (i = 0; i < arguments.length; i++)
                    args.push(shorten(arguments[i]));

                /* Route through the engine log sink so page console output gets
                 * the same "[ 123.4ms]" prefix as [evt]/[nav] and the
                 * [con-call] tag. Fallback: tag by hand if the sink is missing
                 * (old engine build). */
                if (typeof g.__agbConsoleLine === 'function') {
                    g.__agbConsoleLine(args.join(' '));
                    return;
                }

                args.unshift('[con-call]');
                return original.apply(g.console, args);
            };
        }

        wrap('log');
        wrap('info');
        wrap('warn');
        wrap('error');
        wrap('debug');
    }

    install();
}());

/* Unhandled promise rejections: Ant follows Node and calls process.exit() when
 * nothing handles them. The browser-side hook Ant checks before that is
 * window.onunhandledrejection (see Ant's js_fire_unhandled_rejection), so log
 * and mark the event handled there instead of letting the engine die. */
(function () {
    var g = typeof window !== 'undefined' ? window : this;

    if (!g || g.__agbRejectionHook)
        return;

    g.__agbRejectionHook = true;
    g.onunhandledrejection = function (event) {
        try {
            if (event && event.preventDefault)
                event.preventDefault();
            if (g.console && g.console.error)
                g.console.error('Uncaught (in promise)', event && event.reason);
        } catch (e) {}
        return true;
    };
}());

/* Workers are not implemented. Sites still subclass the constructor to wrap
 * it (deepseek's main.js does `class extends Worker` to intercept blob URLs),
 * so the stub must be a real constructor, not undefined — otherwise the
 * subclass declaration itself dies with "Class extends value is not a
 * constructor". This one never runs anything; postMessage/terminate are
 * silent no-ops. */
(function () {
    var g = typeof window !== 'undefined' ? window : this;

    if (!g || typeof g.Worker !== 'undefined')
        return;

    g.Worker = function (url, options) {};
    g.Worker.prototype.postMessage = function (msg) {};
    g.Worker.prototype.terminate = function () {};
}());

/* Event/DOM constructor surface. Scripts read `X.prototype` for feature
 * detection long before they construct anything (React's event system,
 * drag-and-drop and selection helpers all do), and an undefined constructor
 * turns that into "Cannot read properties of undefined (reading 'prototype')".
 * A bare constructor with a prototype is enough for those probes; the ones
 * that carry data get a minimal no-op implementation. */
(function () {
    var g = typeof window !== 'undefined' ? window : this;
    if (!g) return;

    var base = (typeof g.Event === 'function') ? g.Event.prototype : null;
    var names = ['UIEvent', 'FocusEvent', 'PointerEvent', 'WheelEvent', 'InputEvent',
        'CompositionEvent', 'TouchEvent', 'DragEvent', 'ClipboardEvent', 'AnimationEvent',
        'TransitionEvent', 'ProgressEvent', 'StorageEvent', 'PopStateEvent',
        'HashChangeEvent', 'TextEvent', 'PageTransitionEvent', 'CloseEvent',
        'Selection', 'ShadowRoot', 'HTMLSlotElement', 'HTMLContentElement'];

    for (var i = 0; i < names.length; i++) {
        var name = names[i];
        if (typeof g[name] !== 'undefined')
            continue;
        var C = function () {};
        try {
            C.prototype = base && Object.create ? Object.create(base) : { constructor: C };
            C.prototype.constructor = C;
            Object.defineProperty(C, 'name', { value: name, configurable: true });
        } catch (e) { C.prototype = {}; C.prototype.constructor = C; }
        g[name] = C;
    }

    if (typeof g.Range === 'undefined') {
        function Range() {
            this.startContainer = null; this.endContainer = null;
            this.startOffset = 0; this.endOffset = 0; this.collapsed = true;
        }
        Range.prototype.setStart = function () {};
        Range.prototype.setEnd = function () {};
        Range.prototype.selectNode = function () {};
        Range.prototype.selectNodeContents = function () {};
        Range.prototype.collapse = function () {};
        Range.prototype.detach = function () {};
        Range.prototype.getBoundingClientRect = function () {
            return { left: 0, top: 0, right: 0, bottom: 0, width: 0, height: 0, x: 0, y: 0 };
        };
        Range.prototype.getClientRects = function () { return []; };
        Range.prototype.cloneContents = function () { return null; };
        Range.prototype.deleteContents = function () {};
        Range.prototype.insertNode = function () {};
        Range.prototype.surroundContents = function () {};
        g.Range = Range;
    }

    if (typeof g.DataTransfer === 'undefined') {
        function DataTransfer() { this.dropEffect = 'none'; this.effectAllowed = 'all'; this.files = []; this.items = []; this.types = []; }
        DataTransfer.prototype._data = null;
        DataTransfer.prototype.getData = function (type) {
            var d = this._data;
            return d && d[type] ? d[type] : '';
        };
        DataTransfer.prototype.setData = function (type, value) {
            if (!this._data) this._data = {};
            this._data[type] = String(value);
            if (this.types.indexOf(type) < 0) this.types.push(type);
        };
        DataTransfer.prototype.clearData = function (type) {
            if (!this._data) return;
            if (type) delete this._data[type];
            else this._data = null;
        };
        DataTransfer.prototype.setDragImage = function () {};
        g.DataTransfer = DataTransfer;
    }
}());
