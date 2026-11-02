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

            try {
                if (el.style.setProperty) el.style.setProperty(name, value);
                else el.style[name] = value;
            } catch (e) {
                return false;
            }

            var got = el.style.getPropertyValue ? el.style.getPropertyValue(name) : el.style[name];
            return !!(got && String(got).length);
        };
    }

    if (!g.CSS.escape) g.CSS.escape = function (s) { return String(s); };
}());
