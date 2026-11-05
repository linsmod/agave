/* Shared geometry reporter for tools/pages/layout/*.html.
 *
 * Every element carrying data-case="name" is reported as
 *     name:left,top,width,height[|prop=value...]
 * Rects are getBoundingClientRect() (viewport coordinates, sub-pixel), props
 * come from data-props="prop1,prop2" evaluated on getComputedStyle().
 *
 * The same file runs in agave (via --eval-file, which calls probeExit) and in
 * Chrome (layout_diff.py evaluates __measure() over CDP), so both sides see
 * exactly one implementation of "what is the answer".
 */
function __measure() {
    function fmt(n) {
        return Math.round(n * 100) / 100;
    }
    function rectOf(el) {
        var r = el.getBoundingClientRect();
        return fmt(r.left) + ',' + fmt(r.top) + ',' + fmt(r.width) + ',' + fmt(r.height);
    }

    var out = [];
    var els = document.querySelectorAll('[data-case]');
    for (var i = 0; i < els.length; i++) {
        var el = els[i];
        var line = el.getAttribute('data-case') + ':' + rectOf(el);
        var props = el.getAttribute('data-props');
        if (props) {
            var cs = getComputedStyle(el);
            var names = props.split(',');
            for (var j = 0; j < names.length; j++) {
                var p = names[j];
                if (!p) continue;
                line += '|' + p + '=' + cs.getPropertyValue(p);
            }
        }
        out.push(line);
    }

    var de = document.documentElement;
    out.push('viewport:' + de.clientWidth + 'x' + de.clientHeight +
             ' scroll:' + de.scrollWidth + 'x' + de.scrollHeight +
             ' inner:' + window.innerWidth + 'x' + window.innerHeight);
    return out.join('\n');
}

/* agave probe: --eval-file runs this as the whole script and probeExit ends
 * the run with the payload on stdout. Chrome never defines probeExit. */
if (typeof probeExit === 'function') probeExit(__measure());
