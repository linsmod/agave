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
    /* 33.333% is stored as 1/128 of a percent (Length), so it reads back as
     * 33.3281%; Chrome keeps the authored precision. Locks in current
     * behaviour, not the ideal one. */
    out.push('h.basis=' + cs('h').flexBasis);

    var t = document.getElementById('t');
    out.push('gv.shrink=' + cs('t').getPropertyValue('flex-shrink'));
    out.push('gv.basis=' + cs('t').getPropertyValue('flex-basis'));

    t.style.flexShrink = '7';
    t.style.flexBasis = '5px';
    out.push('set.flex=' + cs('t').flex);

    probeExit(out.join('\n'));
})();
