/* flex computed values: getComputedStyle(el).flex / .flexGrow / .flexShrink /
 * .flexBasis, plus the name-based getPropertyValue path and an inline-style
 * round trip. Expected strings live in tools/run_layout_tests.ps1 (group
 * "flexom") and were taken from headless Chrome (R115). */
(function () {
    function cs(id) { return getComputedStyle(document.getElementById(id)); }

    var out = [];
    out.push('a.flex=' + cs('a').flex);
    out.push('a.grow=' + cs('a').flexGrow);
    out.push('a.shrink=' + cs('a').flexShrink);
    out.push('a.basis=' + cs('a').flexBasis);

    out.push('b.flex=' + cs('b').flex);
    out.push('c.flex=' + cs('c').flex);
    out.push('d.flex=' + cs('d').flex);
    out.push('e.flex=' + cs('e').flex);

    out.push('f.basis=' + cs('f').flexBasis);
    out.push('g.basis=' + cs('g').flexBasis);
    /* calc() inside the `flex` shorthand: the sheet rewriter encodes it as a
     * token, so the shorthand loop must decode it as well. */
    out.push('i.flex=' + cs('i').flex);
    out.push('j.flex=' + cs('j').flex);
    /* Percentages are stored at 1/10000 of a percent since R118, so the
     * authored 33.333% reads back exactly like Chrome reports it. */
    out.push('h.basis=' + cs('h').flexBasis);

    var t = document.getElementById('t');
    out.push('gv.shrink=' + cs('t').getPropertyValue('flex-shrink'));
    out.push('gv.basis=' + cs('t').getPropertyValue('flex-basis'));

    t.style.flexShrink = '7';
    t.style.flexBasis = '5px';
    out.push('set.flex=' + cs('t').flex);

    probeExit(out.join('\n'));
})();
