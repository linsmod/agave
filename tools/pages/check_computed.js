/* Computed-style capability check for tools/pages/cap_computed.html.
 *
 * Covers the two halves of "getComputedStyle() answers what Chrome answers":
 *   1. shorthands are re-derived from their longhands (margin/padding/border/
 *      border-radius/background/font/list-style/overflow/outline). Before
 *      this, `cs.margin` was undefined and `cs.border` reported the ::before
 *      content ("normal").
 *   2. every property the engine knows has a named accessor (the list comes
 *      from StylePropertyNames.h through __agbStylePropertyNames), so
 *      `cs.minWidth` / `cs.alignSelf` / `cs.margin` exist on the object.
 *
 * Expected values come from headless Chrome on this same page. Long payloads
 * do not survive the agave console (`[con-log]` truncates around 2 KiB), so
 * each assertion is logged as one short `name=ok` line - run_layout_tests.ps1
 * greps for those.
 */
(function () {
    var out = [];

    function cs(id) { return getComputedStyle(document.getElementById(id)); }

    function eq(name, actual, want) {
        out.push(name + '=' + (String(actual) === String(want) ? 'ok'
            : 'FAIL(' + actual + ' != ' + want + ')'));
    }

    var a = cs('a');
    eq('a.margin', a.getPropertyValue('margin'), '1px 2px 3px 4px');
    eq('a.padding', a.getPropertyValue('padding'), '5px 6px');
    eq('a.border', a.getPropertyValue('border'), '2px solid rgb(10, 20, 30)');
    eq('a.borderWidth', a.getPropertyValue('border-width'), '2px');
    eq('a.borderTop', a.getPropertyValue('border-top'), '2px solid rgb(10, 20, 30)');
    eq('a.camel', a.margin, '1px 2px 3px 4px');
    eq('a.styleCamel', a.border, '2px solid rgb(10, 20, 30)');

    var b = cs('b');
    /* Four different sides cannot be expressed by `border`, so CSSOM (and
     * Chrome) answer the empty string; the per-side longhands still answer. */
    eq('b.borderEmpty', b.getPropertyValue('border'), '');
    eq('b.borderWidth', b.getPropertyValue('border-width'), '3px 1px 0px 4px');
    eq('b.borderStyle', b.getPropertyValue('border-style'), 'dashed dotted none solid');
    eq('b.borderColor', b.getPropertyValue('border-color'),
       'rgb(255, 0, 0) rgb(0, 0, 255) rgb(0, 0, 0) rgb(0, 128, 0)');

    var c = cs('c');
    eq('c.radius', c.getPropertyValue('border-radius'), '4px 6px 8px 10px / 1px 2px 3px 4px');
    eq('c.background', c.getPropertyValue('background'),
       'rgb(255, 0, 0) none no-repeat scroll 1px 2px / auto padding-box border-box');

    var d = cs('d');
    /* font-family is reported as one family here on purpose: the engine keeps
     * only the first family of the list (see arch-capability-gaps). */
    eq('d.font', d.getPropertyValue('font'), 'italic 700 13px / 19.5px Arial');
    eq('d.lineHeight', d.getPropertyValue('line-height'), '19.5px');

    var e = cs('e');
    eq('e.listStyle', e.getPropertyValue('list-style'), 'inside none square');
    eq('e.overflow', e.getPropertyValue('overflow'), 'auto');
    eq('e.flex', e.getPropertyValue('flex'), '1 2 30px');

    eq('f.radius', cs('f').getPropertyValue('border-radius'), '4px');
    eq('g.margin', cs('g').getPropertyValue('margin'), '7px');
    eq('h.overflowXY', cs('h').getPropertyValue('overflow'), 'hidden auto');

    var i = cs('i');
    eq('i.radius', i.getPropertyValue('border-radius'), '4px 6px');
    eq('i.tl', i.getPropertyValue('border-top-left-radius'), '4px');
    eq('i.tr', i.getPropertyValue('border-top-right-radius'), '6px');

    var j = cs('j');
    eq('j.outline', j.getPropertyValue('outline'), 'rgb(0, 0, 255) dotted 2px');
    eq('j.outlineWidth', j.getPropertyValue('outline-width'), '2px');
    /* Chrome reports the specified width (medium = 3px) even with none. */
    eq('a.outline', a.getPropertyValue('outline'), 'rgb(0, 0, 0) none 3px');

    eq('l.background', cs('l').getPropertyValue('background'),
       'rgb(1, 2, 3) none repeat scroll 0% 0% / auto padding-box content-box');
    eq('l.origin', cs('l').getPropertyValue('background-origin'), 'padding-box');
    eq('l.clip', cs('l').getPropertyValue('background-clip'), 'content-box');

    /* Named properties exist for every property the engine knows. */
    var named = ['margin', 'padding', 'border', 'borderWidth', 'borderRadius',
                 'minWidth', 'maxWidth', 'minHeight', 'maxHeight', 'alignSelf',
                 'alignItems', 'justifyContent', 'order', 'listStyle', 'inset',
                 'boxShadow', 'textOverflow', 'wordBreak', 'letterSpacing',
                 /* the modern spellings the parser aliases onto -webkit-*:
                  * a missing accessor here is what hid .flexGrow for a while */
                 'flexGrow', 'flexDirection', 'flexWrap', 'transform', 'boxSizing'];
    var missing = [];
    for (var n = 0; n < named.length; ++n) {
        if (!(named[n] in a)) missing.push(named[n]);
    }
    eq('named.missing', missing.join(','), '');
    eq('named.minWidth', a.minWidth, '0px');
    eq('named.webkitAppearance', typeof a.webkitAppearance, 'string');
    eq('named.length', a.length > 100, 'true');
    eq('named.item', typeof a.item(0), 'string');

    /* getPropertyValue takes the CSS spelling; camelCase is not a property
     * name in CSSOM (Chrome answers "" there too). */
    eq('spelling.camelEmpty', a.getPropertyValue('marginTop'), '');

    if (typeof probeExit === 'function') {
        for (var k = 0; k < out.length; ++k) console.log(out[k]);
        probeExit('checked ' + out.length);
    } else {
        for (var k2 = 0; k2 < out.length; ++k2) console.log(out[k2]);
    }
})();
