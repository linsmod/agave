/* Report for hidpi_probe.html (docs/design-hidpi.md). Run through the probe:
 *
 *   agave_shell.exe --probe tools/pages/hidpi/hidpi_probe.html \
 *       --device-scale 150 --eval-file tools/pages/hidpi/check_hidpi.js
 *
 * Two coordinate spaces are asserted on purpose, because a DPI change has to
 * keep them apart:
 *
 *   CSS px    - everything the DOM reports: window.devicePixelRatio is
 *               zoom * deviceScale, innerWidth is the viewport divided by it,
 *               getBoundingClientRect()/elementFromPoint() take CSS px.
 *   device px - everything the host and the shot path use: hit testing, --click
 *               and --shot-rect (see run_hidpi_tests.ps1).
 *
 * The page's geometry is fixed, so the same script run at --device-scale 100
 * prints dpr=1 / inner=1100x800 / boxRect=0,0 100x100; at 150 it prints dpr=1.5
 * with the CSS rect unchanged (the box is still "100 CSS px") and the device
 * pixels in the screenshot 1.5x bigger.
 */
var out = [];
var de = document.documentElement;
var box = document.getElementById('box');
var target = document.getElementById('target');
var half = document.getElementById('half');

function push(k, v) { out.push(k + '=' + v); }
function px(v) { return Math.round(v); }
function rect(el) {
  var r = el.getBoundingClientRect();
  return px(r.left) + ',' + px(r.top) + ' ' + px(r.width) + 'x' + px(r.height);
}

push('dpr', window.devicePixelRatio);
push('inner', window.innerWidth + 'x' + window.innerHeight);
push('declient', de.clientWidth + 'x' + de.clientHeight);
push('boxRect', rect(box));
push('boxOffset', box.offsetWidth + 'x' + box.offsetHeight);
push('targetRect', rect(target));
push('halfWidth', px(half.getBoundingClientRect().width));

/* Media queries see the same ratio the page does - both the C++ evaluator
 * (a real CSS rule, see #mq in the page) and the matchMedia shim. */
push('mqCSS', getComputedStyle(document.getElementById('mq')).backgroundColor);
push('mqMin15', window.matchMedia ? window.matchMedia('(-webkit-min-device-pixel-ratio: 1.5)').matches : 'n/a');
push('mqMin2', window.matchMedia ? window.matchMedia('(-webkit-min-device-pixel-ratio: 2)').matches : 'n/a');
push('mqRes2', window.matchMedia ? window.matchMedia('(min-resolution: 2dppx)').matches : 'n/a');

/* elementFromPoint() takes CSS px: 200 is the target's left edge in CSS px even
 * though it sits at device 300. */
var hit = document.elementFromPoint(200, 10);
var miss = document.elementFromPoint(199, 10);
push('efpAt200', hit ? hit.id || hit.tagName : 'null');
push('efpAt199', miss ? (miss.id || miss.tagName) : 'null');

/* --click lands in device px (see the runner), so this counter is really an
 * assertion about the hit-test space. */
push('clicks', window.__clicks);

probeExit(out.join(' '));
